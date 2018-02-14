// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/util/debug-util.h"

#include <dirent.h>
#ifndef __linux__
#include <sched.h>
#endif
#ifdef __linux__
#include <syscall.h>
#else
#include <sys/syscall.h>
#endif
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <climits>
#include <csignal>
#include <ctime>
#include <memory>
#include <ostream>
#include <string>

#include <glog/logging.h>
#include <glog/raw_logging.h>
#ifdef __linux__
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#endif

#include "kudu/gutil/dynamic_annotations.h"
#include "kudu/gutil/hash/city.h"
#include "kudu/gutil/linux_syscall_support.h"
#include "kudu/gutil/macros.h"
#include "kudu/gutil/spinlock.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/gutil/strings/numbers.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/debug/leak_annotations.h"
#ifndef __linux__
#include "kudu/util/debug/sanitizer_scopes.h"
#endif
#include "kudu/util/debug/unwind_safeness.h"
#include "kudu/util/errno.h"
#include "kudu/util/monotime.h"
#include "kudu/util/scoped_cleanup.h"
#include "kudu/util/thread.h"

using std::string;
using std::unique_ptr;
using std::vector;

#if defined(__APPLE__)
typedef sig_t sighandler_t;
#endif

// In coverage builds, this symbol will be defined and allows us to flush coverage info
// to disk before exiting.
#if defined(__APPLE__)
  // OS X does not support weak linking at compile time properly.
  #if defined(COVERAGE_BUILD)
extern "C" void __gcov_flush() __attribute__((weak_import));
  #else
extern "C" void (*__gcov_flush)() = nullptr;
  #endif
#else
extern "C" {
__attribute__((weak))
void __gcov_flush();
}
#endif

// Evil hack to grab a few useful functions from glog
namespace google {

extern int GetStackTrace(void** result, int max_depth, int skip_count);

// Symbolizes a program counter.  On success, returns true and write the
// symbol name to "out".  The symbol name is demangled if possible
// (supports symbols generated by GCC 3.x or newer).  Otherwise,
// returns false.
bool Symbolize(void *pc, char *out, int out_size);

namespace glog_internal_namespace_ {
extern void DumpStackTraceToString(string *s);
} // namespace glog_internal_namespace_
} // namespace google

// The %p field width for printf() functions is two characters per byte.
// For some environments, add two extra bytes for the leading "0x".
static const int kPrintfPointerFieldWidth = 2 + 2 * sizeof(void*);

// The signal that we'll use to communicate with our other threads.
// This can't be in used by other libraries in the process.
static int g_stack_trace_signum = SIGUSR2;

// Protects g_stack_trace_signum and the installation of the signal
// handler.
static base::SpinLock g_signal_handler_lock(base::LINKER_INITIALIZED);

namespace kudu {

bool IsCoverageBuild() {
  return __gcov_flush != nullptr;
}

void TryFlushCoverage() {
  static base::SpinLock lock(base::LINKER_INITIALIZED);

  // Flushing coverage is not reentrant or thread-safe.
  if (!__gcov_flush || !lock.TryLock()) {
    return;
  }

  __gcov_flush();

  lock.Unlock();
}


namespace stack_trace_internal {

// Simple notification mechanism based on futex.
//
// We use this instead of a mutex and condvar because we need
// to signal it from a signal handler, and mutexes are not async-safe.
//
// pthread semaphores are async-signal-safe but their timedwait function
// only supports wall clock waiting, which is a bit dangerous since we
// need strict timeouts here.
class CompletionFlag {
 public:

  // Mark the flag as complete, waking all waiters.
  void Signal() {
    complete_ = true;
#ifndef __APPLE__
    sys_futex(reinterpret_cast<int32_t*>(&complete_),
              FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
              INT_MAX, // wake all
              0 /* ignored */);
#endif
  }

  // Wait for the flag to be marked as complete, up until the given deadline.
  // Returns true if the flag was marked complete before the deadline.
  bool WaitUntil(MonoTime deadline) {
    if (complete_) return true;

    MonoTime now = MonoTime::Now();
    while (now < deadline) {
#ifndef __APPLE__
      MonoDelta rem = deadline - now;
      struct timespec ts;
      rem.ToTimeSpec(&ts);
      sys_futex(reinterpret_cast<int32_t*>(&complete_),
                FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                0, // wait if value is still 0
                reinterpret_cast<struct kernel_timespec *>(&ts));
#else
      sched_yield();
#endif
      if (complete_) {
        return true;
      }
      now = MonoTime::Now();
    }
    return complete_;
  }

  void Reset() {
    complete_ = false;
  }

  bool complete() const {
    return complete_;
  }
 private:
  std::atomic<int32_t> complete_ { 0 };
};


// A pointer to this structure is passed as signal data to a thread when
// a stack trace is being remotely requested.
struct SignalData {
  // The actual destination for the stack trace collected from the target thread.
  StackTrace* stack;

  static const int kNotInUse = 0;
  static const int kDumpInProgress = -1;
  // Either one of the above constants, or if the dumper thread
  // is waiting on a response, the tid that it is waiting on.
  std::atomic<int64_t> queued_to_tid { kNotInUse };

  // Signaled when the target thread has successfully collected its stack.
  // The dumper thread waits for this to become true.
  CompletionFlag result_ready;
};

} // namespace stack_trace_internal

using stack_trace_internal::SignalData;

namespace {

// Signal handler for our stack trace signal.
// We expect that the signal is only sent from DumpThreadStack() -- not by a user.
void HandleStackTraceSignal(int /*signum*/, siginfo_t* info, void* /*ucontext*/) {
  // Signal handlers may be invoked at any point, so it's important to preserve
  // errno.
  int save_errno = errno;
  SCOPED_CLEANUP({
      errno = save_errno;
    });
  auto* sig_data = reinterpret_cast<SignalData*>(info->si_ptr);
  DCHECK(sig_data);
  if (!sig_data) {
    // Maybe the signal was sent by a user instead of by ourself, ignore it.
    return;
  }
  ANNOTATE_HAPPENS_AFTER(sig_data);
  int64_t my_tid = Thread::CurrentThreadId();

  // If we were slow to process the signal, the sender may have given up and
  // no longer wants our stack trace. In that case, the 'sig' object will
  // no longer contain our thread.
  if (!sig_data->queued_to_tid.compare_exchange_strong(my_tid, SignalData::kDumpInProgress)) {
    return;
  }
  // Marking it as kDumpInProgress ensures that the caller thread must now wait
  // for our response, since we are writing directly into their StackTrace object.
  sig_data->stack->Collect(/*skip_frames=*/1);
  sig_data->queued_to_tid = SignalData::kNotInUse;
  sig_data->result_ready.Signal();
}

bool InitSignalHandlerUnlocked(int signum) {
  enum InitState {
    UNINITIALIZED,
    INIT_ERROR,
    INITIALIZED
  };
  static InitState state = UNINITIALIZED;

  // If we've already registered a handler, but we're being asked to
  // change our signal, unregister the old one.
  if (signum != g_stack_trace_signum && state == INITIALIZED) {
    struct sigaction old_act;
    PCHECK(sigaction(g_stack_trace_signum, nullptr, &old_act) == 0);
    if (old_act.sa_sigaction == &HandleStackTraceSignal) {
      signal(g_stack_trace_signum, SIG_DFL);
    }
  }

  // If we'd previously had an error, but the signal number
  // is changing, we should mark ourselves uninitialized.
  if (signum != g_stack_trace_signum) {
    g_stack_trace_signum = signum;
    state = UNINITIALIZED;
  }

  if (state == UNINITIALIZED) {
    struct sigaction old_act;
    PCHECK(sigaction(g_stack_trace_signum, nullptr, &old_act) == 0);
    if (old_act.sa_handler != SIG_DFL &&
        old_act.sa_handler != SIG_IGN) {
      state = INIT_ERROR;
      LOG(WARNING) << "signal handler for stack trace signal "
                   << g_stack_trace_signum
                   << " is already in use: "
                   << "Kudu will not produce thread stack traces.";
    } else {
      // No one appears to be using the signal. This is racy, but there is no
      // atomic swap capability.
      struct sigaction act;
      memset(&act, 0, sizeof(act));
      act.sa_sigaction = &HandleStackTraceSignal;
      act.sa_flags = SA_SIGINFO | SA_RESTART;
      struct sigaction old_act;
      CHECK_ERR(sigaction(g_stack_trace_signum, &act, &old_act));
      sighandler_t old_handler = old_act.sa_handler;
      if (old_handler != SIG_IGN &&
          old_handler != SIG_DFL) {
        LOG(FATAL) << "raced against another thread installing a signal handler";
      }
      state = INITIALIZED;
    }
  }
  return state == INITIALIZED;
}

} // anonymous namespace

Status SetStackTraceSignal(int signum) {
  base::SpinLockHolder h(&g_signal_handler_lock);
  if (!InitSignalHandlerUnlocked(signum)) {
    return Status::InvalidArgument("unable to install signal handler");
  }
  return Status::OK();
}

StackTraceCollector::StackTraceCollector(StackTraceCollector&& other) noexcept
    : tid_(other.tid_),
      sig_data_(other.sig_data_) {
  other.tid_ = 0;
  other.sig_data_ = nullptr;
}

StackTraceCollector::~StackTraceCollector() {
  if (sig_data_) {
    RevokeSigData();
  }
}

#ifdef __linux__
void StackTraceCollector::RevokeSigData() {
  // We have several cases to consider. Though it involves some simple code
  // duplication, each case is handled separately for clarity.

  // 1) We have sent the signal, and the thread has completed the stack trace.
  //    This this is the "happy path".
  if (sig_data_->result_ready.complete()) {
    delete sig_data_;
    sig_data_ = nullptr;
    return;
  }

  // 2) Timed out, but signal still pending and signal handler not yet invoked.
  //    In this case, we need to make sure that when it does later get the signal,
  //    it doesn't attempt to read 'sig_data_' after we've freed it. However
  //    we can still safely free the stack trace object itself.
  //
  // 3) Timed out, but signal has been delivered and the stack tracing is in
  //    progress. In this case we have to wait for it to finish. This case should
  //    be very rare, since the trace collection itself is typically fast.
  //
  // We can distinguish between case (2) and (3) using the 'queued_to_tid' member:
  int64_t old_val = sig_data_->queued_to_tid.exchange(SignalData::kNotInUse);

  // In case (2), the signal handler hasn't started collecting a stack trace, so we
  // were able to exchange it out and see that it was still "queued". In this case,
  // if the signal later gets delivered, we can't free the sig_data_ struct itself.
  // We intentionally leak it. Note, however, that when it does run, it will
  // see that we exchanged out its tid from 'queued_to_tid' and therefore won't
  // attempt to write into the stack_ structure.
  //
  // TODO(todd) instead of leaking, we can insert these lost structs into a
  // global free-list, and then reuse them the next time we want to send a
  // signal. The re-use is safe since access is limited to a specific tid.
  if (old_val == tid_) {
    DLOG(WARNING) << "Leaking SignalData structure " << sig_data_ << " after lost signal "
                  << "to thread " << tid_;
    ANNOTATE_LEAKING_OBJECT_PTR(sig_data_);
    sig_data_ = nullptr;
    return;
  }

  // In case (3), the signal handler started running but isn't complete yet.
  // We have no choice but to await completion
  CHECK(old_val == SignalData::kDumpInProgress);
  CHECK(sig_data_->result_ready.WaitUntil(MonoTime::Max()));

  delete sig_data_;
  sig_data_ = nullptr;
}


Status StackTraceCollector::TriggerAsync(int64_t tid, StackTrace* stack) {
  CHECK(!sig_data_ && tid_ == 0) << "TriggerAsync() must not be called more than once per instance";

  // Ensure that our signal handler is installed.
  {
    base::SpinLockHolder h(&g_signal_handler_lock);
    if (!InitSignalHandlerUnlocked(g_stack_trace_signum)) {
      return Status::NotSupported("unable to take thread stack: signal handler unavailable");
    }
  }

  std::unique_ptr<SignalData> data(new SignalData());
  // Set the target TID in our communication structure, so if we end up with any
  // delayed signal reaching some other thread, it will know to ignore it.
  data->queued_to_tid = tid;
  data->stack = CHECK_NOTNULL(stack);

  // We use the raw syscall here instead of kill() to ensure that we don't accidentally
  // send a signal to some other process in the case that the thread has exited and
  // the TID been recycled.
  siginfo_t info;
  memset(&info, 0, sizeof(info));
  info.si_signo = g_stack_trace_signum;
  info.si_code = SI_QUEUE;
  info.si_pid = getpid();
  info.si_uid = getuid();
  info.si_value.sival_ptr = data.get();
  // Since we're using a signal to pass information between the two threads,
  // we need to help TSAN out and explicitly tell it about the happens-before
  // relationship here.
  ANNOTATE_HAPPENS_BEFORE(data.get());
  if (syscall(SYS_rt_tgsigqueueinfo, getpid(), tid, g_stack_trace_signum, &info) != 0) {
    return Status::NotFound("unable to deliver signal: process may have exited");
  }

  // The signal is now pending to the target thread. We don't store it in a unique_ptr
  // inside the class since we need to be careful to destruct it safely in case the
  // target thread hasn't yet received the signal when this instance gets destroyed.
  sig_data_ = data.release();
  tid_ = tid;

  return Status::OK();
}

Status StackTraceCollector::AwaitCollection(MonoTime deadline) {
  CHECK(sig_data_) << "Must successfully call TriggerAsync() first";

  // We give the thread ~1s to respond. In testing, threads typically respond within
  // a few milliseconds, so this timeout is very conservative.
  //
  // The main reason that a thread would not respond is that it has blocked signals. For
  // example, glibc's timer_thread doesn't respond to our signal, so we always time out
  // on that one.
  bool got_result = sig_data_->result_ready.WaitUntil(deadline);
  RevokeSigData();
  if (!got_result) {
    return Status::TimedOut("thread did not respond: maybe it is blocking signals");
  }

  return Status::OK();
}

#else  // __linux__
Status StackTraceCollector::TriggerAsync(int64_t tid_, StackTrace* stack) {
  return Status::NotSupported("unsupported platform");
}
Status StackTraceCollector::AwaitCollection() {
  return Status::NotSupported("unsupported platform");
}
void StackTraceCollector::RevokeSigData() {
}
#endif // __linux__

Status GetThreadStack(int64_t tid, StackTrace* stack) {
  StackTraceCollector c;
  RETURN_NOT_OK(c.TriggerAsync(tid, stack));
  RETURN_NOT_OK(c.AwaitCollection(MonoTime::Now() + MonoDelta::FromSeconds(1)));
  return Status::OK();
}

string DumpThreadStack(int64_t tid) {
  StackTrace trace;
  Status s = GetThreadStack(tid, &trace);
  if (s.ok()) {
    return trace.Symbolize();
  }
  return strings::Substitute("<$0>", s.ToString());
}

Status ListThreads(vector<pid_t> *tids) {
#ifndef __linux__
  return Status::NotSupported("unable to list threads on this platform");
#else
  DIR *dir = opendir("/proc/self/task/");
  if (dir == NULL) {
    return Status::IOError("failed to open task dir", ErrnoToString(errno), errno);
  }
  struct dirent *d;
  while ((d = readdir(dir)) != NULL) {
    if (d->d_name[0] != '.') {
      uint32_t tid;
      if (!safe_strtou32(d->d_name, &tid)) {
        LOG(WARNING) << "bad tid found in procfs: " << d->d_name;
        continue;
      }
      tids->push_back(tid);
    }
  }
  closedir(dir);
  return Status::OK();
#endif // __linux__
}

string GetStackTrace() {
  string s;
  google::glog_internal_namespace_::DumpStackTraceToString(&s);
  return s;
}

string GetStackTraceHex() {
  char buf[1024];
  HexStackTraceToString(buf, 1024);
  return buf;
}

void HexStackTraceToString(char* buf, size_t size) {
  StackTrace trace;
  trace.Collect(1);
  trace.StringifyToHex(buf, size);
}

string GetLogFormatStackTraceHex() {
  StackTrace trace;
  trace.Collect(1);
  return trace.ToLogFormatHexString();
}

// Bogus empty function which we use below to fill in the stack trace with
// something readable to indicate that stack trace collection was unavailable.
void CouldNotCollectStackTraceBecauseInsideLibDl() {
}

void StackTrace::Collect(int skip_frames) {
  if (!debug::SafeToUnwindStack()) {
    // Build a fake stack so that the user sees an appropriate message upon symbolizing
    // rather than seeing an empty stack.
    uintptr_t f_ptr = reinterpret_cast<uintptr_t>(&CouldNotCollectStackTraceBecauseInsideLibDl);
    // Increase the pointer by one byte since the return address from a function call
    // would not be the beginning of the function itself.
    frames_[0] = reinterpret_cast<void*>(f_ptr + 1);
    num_frames_ = 1;
    return;
  }
  const int kMaxDepth = arraysize(frames_);

#ifdef __linux__
  unw_cursor_t cursor;
  unw_context_t uc;
  unw_getcontext(&uc);
  RAW_CHECK(unw_init_local(&cursor, &uc) >= 0, "unw_init_local failed");
  skip_frames++;         // Do not include the "Collect" frame

  num_frames_ = 0;
  while (num_frames_ < kMaxDepth) {
    void *ip;
    int ret = unw_get_reg(&cursor, UNW_REG_IP, reinterpret_cast<unw_word_t *>(&ip));
    if (ret < 0) {
      break;
    }
    if (skip_frames > 0) {
      skip_frames--;
    } else {
      frames_[num_frames_++] = ip;
    }
    ret = unw_step(&cursor);
    if (ret <= 0) {
      break;
    }
  }
#else
  // On OSX, use the unwinder from glog. However, that unwinder has an issue where
  // concurrent invocations will return no frames. See:
  // https://github.com/google/glog/issues/298
  // The worst result here is an empty result.

  // google::GetStackTrace has a data race. This is called frequently, so better
  // to ignore it with an annotation rather than use a suppression.
  debug::ScopedTSANIgnoreReadsAndWrites ignore_tsan;
  num_frames_ = google::GetStackTrace(frames_, kMaxDepth, skip_frames + 1);
#endif
}

void StackTrace::StringifyToHex(char* buf, size_t size, int flags) const {
  char* dst = buf;

  // Reserve kHexEntryLength for the first iteration of the loop, 1 byte for a
  // space (which we may not need if there's just one frame), and 1 for a nul
  // terminator.
  char* limit = dst + size - kHexEntryLength - 2;
  for (int i = 0; i < num_frames_ && dst < limit; i++) {
    if (i != 0) {
      *dst++ = ' ';
    }
    // See note in Symbolize() below about why we subtract 1 from each address here.
    uintptr_t addr = reinterpret_cast<uintptr_t>(frames_[i]);
    if (addr > 0 && !(flags & NO_FIX_CALLER_ADDRESSES)) {
      addr--;
    }
    FastHex64ToBuffer(addr, dst);
    dst += kHexEntryLength;
  }
  *dst = '\0';
}

string StackTrace::ToHexString(int flags) const {
  // Each frame requires kHexEntryLength, plus a space
  // We also need one more byte at the end for '\0'
  char buf[kMaxFrames * (kHexEntryLength + 1) + 1];
  StringifyToHex(buf, arraysize(buf), flags);
  return string(buf);
}

// Symbolization function borrowed from glog.
string StackTrace::Symbolize() const {
  string ret;
  for (int i = 0; i < num_frames_; i++) {
    void* pc = frames_[i];

    char tmp[1024];
    const char* symbol = "(unknown)";

    // The return address 'pc' on the stack is the address of the instruction
    // following the 'call' instruction. In the case of calling a function annotated
    // 'noreturn', this address may actually be the first instruction of the next
    // function, because the function we care about ends with the 'call'.
    // So, we subtract 1 from 'pc' so that we're pointing at the 'call' instead
    // of the return address.
    //
    // For example, compiling a C program with -O2 that simply calls 'abort()' yields
    // the following disassembly:
    //     Disassembly of section .text:
    //
    //     0000000000400440 <main>:
    //       400440:	48 83 ec 08          	sub    $0x8,%rsp
    //       400444:	e8 c7 ff ff ff       	callq  400410 <abort@plt>
    //
    //     0000000000400449 <_start>:
    //       400449:	31 ed                	xor    %ebp,%ebp
    //       ...
    //
    // If we were to take a stack trace while inside 'abort', the return pointer
    // on the stack would be 0x400449 (the first instruction of '_start'). By subtracting
    // 1, we end up with 0x400448, which is still within 'main'.
    //
    // This also ensures that we point at the correct line number when using addr2line
    // on logged stacks.
    if (google::Symbolize(
            reinterpret_cast<char *>(pc) - 1, tmp, sizeof(tmp))) {
      symbol = tmp;
    }
    StringAppendF(&ret, "    @ %*p  %s\n", kPrintfPointerFieldWidth, pc, symbol);
  }
  return ret;
}

string StackTrace::ToLogFormatHexString() const {
  string ret;
  for (int i = 0; i < num_frames_; i++) {
    void* pc = frames_[i];
    StringAppendF(&ret, "    @ %*p\n", kPrintfPointerFieldWidth, pc);
  }
  return ret;
}

uint64_t StackTrace::HashCode() const {
  return util_hash::CityHash64(reinterpret_cast<const char*>(frames_),
                               sizeof(frames_[0]) * num_frames_);
}

}  // namespace kudu
