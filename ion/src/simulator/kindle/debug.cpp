#include "debug.h"

#include <cxxabi.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

#if KINDLE_DIAGNOSTICS
extern char ** environ;
#endif

namespace {

#if KINDLE_DIAGNOSTICS
using Ion::Kindle::Debug::Counters;

constexpr uint64_t k_stallMs = 4000;          // No keyboard poll for this long: stalled
constexpr uint64_t k_startupStallMs = 10000;  // Same, before the first poll
constexpr uint64_t k_repeatReportMs = 15000;  // Report again while still stalled
constexpr uint64_t k_rescueHoldMs = 3000;     // Finger held this long while stalled: exit
constexpr uint64_t k_statusEveryMs = 10000;   // Verbose status line
constexpr int k_maxFrames = 48;

#endif

uint64_t sStartMs = 0;
#if KINDLE_DIAGNOSTICS
bool sVerbose = false;
const char * volatile sPhase = "starting";
uint64_t sLastBeat = 0;  // Accessed with __atomic builtins
Counters sCounters;
pthread_t sMainThread;
pid_t sMainTid = 0;
volatile int sTouchFd = -1;
volatile sig_atomic_t sDumpRequested = 0;
#endif

uint64_t nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- Async-signal-safe output ---- */

void writeAll(const char * s, size_t n) {
  while (n > 0) {
    ssize_t w = write(STDERR_FILENO, s, n);
    if (w < 0 && errno == EINTR) {
      continue;
    }
    if (w <= 0) {
      return;
    }
    s += w;
    n -= w;
  }
}

#if KINDLE_DIAGNOSTICS

void writeStr(const char * s) {
  writeAll(s, strlen(s));
}

void writeHex(uintptr_t v) {
  char buffer[2 + 2 * sizeof(uintptr_t) + 1];
  int i = sizeof(buffer) - 1;
  buffer[i] = 0;
  do {
    buffer[--i] = "0123456789abcdef"[v & 0xF];
    v >>= 4;
  } while (v != 0 && i > 2);
  buffer[--i] = 'x';
  buffer[--i] = '0';
  writeStr(buffer + i);
}

void writeDec(uint64_t v, int minDigits = 1) {
  char buffer[24];
  int i = sizeof(buffer) - 1;
  buffer[i] = 0;
  do {
    buffer[--i] = '0' + (v % 10);
    v /= 10;
    minDigits--;
  } while ((v != 0 || minDigits > 0) && i > 0);
  writeStr(buffer + i);
}

void writeTimestamp() {
  uint64_t t = nowMs() - sStartMs;
  writeStr("[");
  uint64_t seconds = t / 1000;
  for (uint64_t limit = 100000; limit > 1 && seconds < limit; limit /= 10) {
    writeStr(" ");
  }
  writeDec(seconds);
  writeStr(".");
  writeDec(t % 1000, 3);
  writeStr("] ");
}

/* ---- Symbols, read from our own ELF symbol table ---- */

struct Symbol {
  uint32_t address;
  uint32_t size;
  uint32_t name;
};

Symbol * sSymbols = nullptr;
int sSymbolCount = 0;
char * sStrings = nullptr;
volatile bool sSymbolsReady = false;

int compareSymbols(const void * a, const void * b) {
  uint32_t x = static_cast<const Symbol *>(a)->address;
  uint32_t y = static_cast<const Symbol *>(b)->address;
  return x < y ? -1 : (x > y ? 1 : 0);
}

void loadSymbols() {
  int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  Elf32_Ehdr header;
  Elf32_Shdr * sections = nullptr;
  Elf32_Sym * symbols = nullptr;
  if (pread(fd, &header, sizeof(header), 0) != (ssize_t)sizeof(header)
      || memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS32
      || header.e_shentsize != sizeof(Elf32_Shdr) || header.e_shnum == 0) {
    close(fd);
    return;
  }
  size_t sectionsSize = header.e_shnum * sizeof(Elf32_Shdr);
  sections = static_cast<Elf32_Shdr *>(malloc(sectionsSize));
  if (sections == nullptr || pread(fd, sections, sectionsSize, header.e_shoff) != (ssize_t)sectionsSize) {
    free(sections);
    close(fd);
    return;
  }
  for (int i = 0; i < header.e_shnum; i++) {
    if (sections[i].sh_type != SHT_SYMTAB || sections[i].sh_link >= header.e_shnum) {
      continue;
    }
    const Elf32_Shdr & table = sections[i];
    const Elf32_Shdr & strings = sections[table.sh_link];
    symbols = static_cast<Elf32_Sym *>(malloc(table.sh_size));
    sStrings = static_cast<char *>(malloc(strings.sh_size + 1));
    if (symbols == nullptr || sStrings == nullptr
        || pread(fd, symbols, table.sh_size, table.sh_offset) != (ssize_t)table.sh_size
        || pread(fd, sStrings, strings.sh_size, strings.sh_offset) != (ssize_t)strings.sh_size) {
      free(symbols);
      free(sStrings);
      sStrings = nullptr;
      symbols = nullptr;
      break;
    }
    sStrings[strings.sh_size] = 0;
    int count = table.sh_size / sizeof(Elf32_Sym);
    sSymbols = static_cast<Symbol *>(malloc(sizeof(Symbol) * count));
    if (sSymbols == nullptr) {
      break;
    }
    for (int j = 0; j < count; j++) {
      const Elf32_Sym & s = symbols[j];
      if (ELF32_ST_TYPE(s.st_info) == STT_FUNC && s.st_value != 0 && s.st_name < strings.sh_size) {
        sSymbols[sSymbolCount++] = Symbol{s.st_value & ~1u, s.st_size, s.st_name};
      }
    }
    qsort(sSymbols, sSymbolCount, sizeof(Symbol), compareSymbols);
    break;
  }
  free(symbols);
  free(sections);
  close(fd);
  __atomic_store_n(&sSymbolsReady, sSymbolCount > 0, __ATOMIC_RELEASE);
}

const Symbol * findSymbol(uintptr_t address) {
  if (!__atomic_load_n(&sSymbolsReady, __ATOMIC_ACQUIRE)) {
    return nullptr;
  }
  int lo = 0, hi = sSymbolCount - 1, found = -1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (sSymbols[mid].address <= address) {
      found = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  if (found < 0) {
    return nullptr;
  }
  const Symbol & s = sSymbols[found];
  uint32_t limit = s.size > 0 ? s.size : 0x10000;
  return address - s.address < limit ? &s : nullptr;
}

/* ---- Backtraces ---- */

struct UnwindState {
  uintptr_t * frames;
  int max;
  int count;
};

_Unwind_Reason_Code unwindCallback(struct _Unwind_Context * context, void * argument) {
  UnwindState * state = static_cast<UnwindState *>(argument);
  uintptr_t ip = _Unwind_GetIP(context);
  if (ip == 0 || state->count >= state->max) {
    return _URC_END_OF_STACK;
  }
  state->frames[state->count++] = ip;
  return _URC_NO_REASON;
}

int captureBacktrace(uintptr_t * frames, int max) {
  UnwindState state{frames, max, 0};
  _Unwind_Backtrace(unwindCallback, &state);
  return state.count;
}

void contextRegisters(void * context, uintptr_t * pc, uintptr_t * lr, uintptr_t * sp) {
  *pc = *lr = *sp = 0;
#if defined(__arm__)
  if (context != nullptr) {
    const ucontext_t * uc = static_cast<const ucontext_t *>(context);
    *pc = uc->uc_mcontext.arm_pc;
    *lr = uc->uc_mcontext.arm_lr;
    *sp = uc->uc_mcontext.arm_sp;
  }
#else
  (void)context;
#endif
}

// Async-signal-safe: mangled names only
void writeFrame(int index, const char * label, uintptr_t address) {
  writeStr("    #");
  writeDec(index);
  writeStr(" ");
  writeStr(label);
  writeHex(address);
  // Return addresses point after the call: look up the call itself
  const Symbol * s = findSymbol(address > 0 ? address - 1 : address);
  if (s != nullptr) {
    writeStr(" ");
    writeStr(sStrings + s->name);
    writeStr("+");
    writeHex(address - s->address);
  }
  writeStr("\n");
}

// Main thread backtrace, captured on request by the watchdog
volatile sig_atomic_t sBacktraceReady = 0;
uintptr_t sBacktrace[k_maxFrames];
int sBacktraceCount = 0;
uintptr_t sBacktracePc = 0, sBacktraceLr = 0, sBacktraceSp = 0;

void onBacktraceRequest(int, siginfo_t *, void * context) {
  int savedErrno = errno;
  contextRegisters(context, &sBacktracePc, &sBacktraceLr, &sBacktraceSp);
  sBacktraceCount = captureBacktrace(sBacktrace, k_maxFrames);
  __atomic_store_n(&sBacktraceReady, 1, __ATOMIC_RELEASE);
  errno = savedErrno;
}

const char * signalName(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV (invalid memory access)";
    case SIGBUS: return "SIGBUS (bus error)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGFPE: return "SIGFPE (arithmetic error)";
    case SIGABRT: return "SIGABRT (abort)";
    default: return "signal";
  }
}

void onCrash(int sig, siginfo_t * info, void * context) {
  uintptr_t pc, lr, sp;
  contextRegisters(context, &pc, &lr, &sp);
  writeTimestamp();
  writeStr("CRASH: ");
  writeStr(signalName(sig));
  writeStr(" in thread ");
  writeDec((uint64_t)syscall(SYS_gettid));
  writeStr(", fault address ");
  writeHex(info ? (uintptr_t)info->si_addr : 0);
  writeStr(", phase: ");
  writeStr(sPhase);
  writeStr("\n");
  writeFrame(0, "pc ", pc);
  writeFrame(1, "lr ", lr);
  uintptr_t frames[k_maxFrames];
  int count = captureBacktrace(frames, k_maxFrames);
  writeStr("  backtrace:\n");
  for (int i = 0; i < count; i++) {
    writeFrame(i, "", frames[i]);
  }
  writeTimestamp();
  writeStr("upsilon is exiting because of this crash\n");
  // SA_RESETHAND restored the default action: re-raise once we return
  raise(sig);
}

void onDumpRequest(int) {
  sDumpRequested = 1;
}

/* ---- Watchdog ---- */

bool readSmallFile(const char * path, char * buffer, size_t size) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    buffer[0] = 0;
    return false;
  }
  ssize_t n = read(fd, buffer, size - 1);
  close(fd);
  if (n < 0) {
    buffer[0] = 0;
    return false;
  }
  buffer[n] = 0;
  while (n > 0 && (buffer[n - 1] == '\n' || buffer[n - 1] == ' ')) {
    buffer[--n] = 0;
  }
  return true;
}

const char * describeState(char state) {
  switch (state) {
    case 'R': return "running (busy computing)";
    case 'S': return "sleeping (waiting, interruptible)";
    case 'D': return "uninterruptible wait in the kernel (usually a driver)";
    case 'T': case 't': return "stopped";
    case 'Z': return "zombie";
    default: return "?";
  }
}

const char * syscallName(long nr) {
  // ARM EABI numbers
  switch (nr) {
    case 3: return "read";
    case 4: return "write";
    case 5: return "open";
    case 6: return "close";
    case 54: return "ioctl";
    case 118: return "fsync";
    case 142: return "select";
    case 162: return "nanosleep";
    case 168: return "poll";
    case 240: return "futex";
    case 263: return "clock_gettime";
    case 265: return "clock_nanosleep";
    case 322: return "openat";
    case 336: return "ppoll";
    case 345: return "getcpu";
    case 403: return "clock_gettime64";
    case 407: return "clock_nanosleep_time64";
    case 414: return "ppoll_time64";
    default: return nullptr;
  }
}

const char * ioctlName(unsigned long request) {
  switch (request) {
    case 0x4048462e: return "MXCFB_SEND_UPDATE (Rex layout)";
    case 0x4050462e: return "MXCFB_SEND_UPDATE (Zelda layout)";
    case 0x4044462e: return "MXCFB_SEND_UPDATE (legacy layout)";
    case 0xc008462f: return "MXCFB_WAIT_FOR_UPDATE_COMPLETE";
    case 0x40044590: return "EVIOCGRAB";
    case 0x4600: return "FBIOGET_VSCREENINFO";
    case 0x4602: return "FBIOGET_FSCREENINFO";
    default: return nullptr;
  }
}

void logMainThreadKernelState() {
  char path[96];
  char buffer[2048];

  snprintf(path, sizeof(path), "/proc/self/task/%d/stat", sMainTid);
  if (readSmallFile(path, buffer, sizeof(buffer))) {
    const char * close = strrchr(buffer, ')');
    char state = (close != nullptr && close[1] == ' ') ? close[2] : '?';
    Ion::Kindle::Debug::log("  main thread: %c, %s", state, describeState(state));
  }
  snprintf(path, sizeof(path), "/proc/self/task/%d/wchan", sMainTid);
  if (readSmallFile(path, buffer, sizeof(buffer)) && buffer[0] != 0) {
    Ion::Kindle::Debug::log("  kernel wait channel: %s", buffer);
  }
  snprintf(path, sizeof(path), "/proc/self/task/%d/syscall", sMainTid);
  if (readSmallFile(path, buffer, sizeof(buffer)) && buffer[0] != 0) {
    long nr = -1;
    unsigned long a0 = 0, a1 = 0;
    const char * name = nullptr;
    const char * detail = nullptr;
    if (sscanf(buffer, "%ld %lx %lx", &nr, &a0, &a1) >= 1 && nr >= 0) {
      name = syscallName(nr);
      if (nr == 54) {
        detail = ioctlName(a1);
      }
    }
    char what[96];
    if (nr < 0) {
      snprintf(what, sizeof(what), "none (running user code)");
    } else if (name != nullptr) {
      snprintf(what, sizeof(what), "%s%s%s", name, detail ? " " : "", detail ? detail : "");
    } else {
      snprintf(what, sizeof(what), "#%ld", nr);
    }
    Ion::Kindle::Debug::log("  system call: %s [%s]", what, buffer);
  }
  snprintf(path, sizeof(path), "/proc/self/task/%d/stack", sMainTid);
  if (readSmallFile(path, buffer, sizeof(buffer)) && buffer[0] != 0) {
    Ion::Kindle::Debug::log("  kernel stack:");
    char * save = nullptr;
    for (char * line = strtok_r(buffer, "\n", &save); line != nullptr; line = strtok_r(nullptr, "\n", &save)) {
      Ion::Kindle::Debug::log("    %s", line);
    }
  }
}

void logFrame(int index, const char * label, uintptr_t address) {
  const Symbol * s = findSymbol(address > 0 ? address - 1 : address);
  if (s == nullptr) {
    Ion::Kindle::Debug::log("    #%d %s0x%08lx", index, label, (unsigned long)address);
    return;
  }
  const char * mangled = sStrings + s->name;
  int status = -1;
  char * demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
  Ion::Kindle::Debug::log("    #%d %s0x%08lx %s+0x%lx", index, label, (unsigned long)address,
      status == 0 && demangled ? demangled : mangled, (unsigned long)(address - s->address));
  free(demangled);
}

bool sampleMainThread(uintptr_t * frames, int * count, uintptr_t * pc, uintptr_t * lr) {
  __atomic_store_n(&sBacktraceReady, 0, __ATOMIC_RELEASE);
  if (pthread_kill(sMainThread, SIGUSR2) != 0) {
    return false;
  }
  for (int i = 0; i < 30 && !__atomic_load_n(&sBacktraceReady, __ATOMIC_ACQUIRE); i++) {
    usleep(50000);
  }
  if (!__atomic_load_n(&sBacktraceReady, __ATOMIC_ACQUIRE)) {
    return false;
  }
  *count = sBacktraceCount;
  memcpy(frames, sBacktrace, sizeof(uintptr_t) * sBacktraceCount);
  *pc = sBacktracePc;
  *lr = sBacktraceLr;
  return true;
}

void logMainThreadBacktrace() {
  /* When the main thread is interrupted in code without unwind tables (the
   * kernel's vDSO, e.g. while reading the clock in a busy loop), the trace
   * stops early. Sample a few times and keep the deepest one. */
  uintptr_t best[k_maxFrames], frames[k_maxFrames];
  int bestCount = -1, count = 0;
  uintptr_t bestPc = 0, bestLr = 0, pc = 0, lr = 0;
  for (int attempt = 0; attempt < 6; attempt++) {
    if (!sampleMainThread(frames, &count, &pc, &lr)) {
      break;
    }
    if (count > bestCount) {
      bestCount = count;
      memcpy(best, frames, sizeof(uintptr_t) * count);
      bestPc = pc;
      bestLr = lr;
    }
    if (count >= 5) {
      break;
    }
    usleep(30000);
  }
  if (bestCount < 0) {
    Ion::Kindle::Debug::log("  user-space backtrace: main thread did not answer (it cannot be interrupted, see kernel state above)");
    return;
  }
  Ion::Kindle::Debug::log("  user-space backtrace:");
  logFrame(0, "pc ", bestPc);
  logFrame(1, "lr ", bestLr);
  for (int i = 0; i < bestCount; i++) {
    logFrame(i, "", best[i]);
  }
}

void logKindleUiState() {
  DIR * proc = opendir("/proc");
  if (proc == nullptr) {
    return;
  }
  struct dirent * entry;
  while ((entry = readdir(proc)) != nullptr) {
    if (entry->d_name[0] < '1' || entry->d_name[0] > '9') {
      continue;
    }
    char path[300], comm[64], status[1024];
    snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
    if (!readSmallFile(path, comm, sizeof(comm)) || strcmp(comm, "cvm") != 0) {
      continue;
    }
    snprintf(path, sizeof(path), "/proc/%s/status", entry->d_name);
    if (readSmallFile(path, status, sizeof(status))) {
      const char * state = strstr(status, "State:");
      if (state != nullptr) {
        const char * end = strchr(state, '\n');
        Ion::Kindle::Debug::log("  Kindle UI (cvm, pid %s): %.*s", entry->d_name,
            (int)(end ? end - state - 6 : (long)strlen(state) - 6), state + 6);
      }
    }
  }
  closedir(proc);
}

void report(const char * reason, uint64_t sinceBeat) {
  using Ion::Kindle::Debug::log;
  log("==== %s ====", reason);
  if (sinceBeat > 0) {
    log("  main loop has not polled input for %.1f s, phase: %s", sinceBeat / 1000.0, sPhase);
  } else {
    log("  phase: %s", sPhase);
  }
  logMainThreadKernelState();
  logMainThreadBacktrace();
  log("  touch events so far: %u, taps: %u, e-ink updates: %u (%u failed, slowest %u ms)",
      sCounters.touchEvents, sCounters.taps, sCounters.einkUpdates, sCounters.einkFailures, sCounters.einkMaxMs);
  logKindleUiState();
  log("==== end of report ====");
}

/* Rescue gesture: only looked at while the main loop is stalled, so we are
 * not competing with it for touch events. */
struct Rescue {
  bool down = false;
  uint64_t downSince = 0;
  uint64_t lastActivity = 0;
  bool announced = false;

  void reset() {
    down = false;
    downSince = 0;
    lastActivity = 0;
  }

  // Returns true when the finger has been held long enough
  bool poll(int timeoutMs) {
    if (sTouchFd < 0) {
      usleep(timeoutMs * 1000);
      return false;
    }
    struct pollfd p{sTouchFd, POLLIN, 0};
    if (::poll(&p, 1, timeoutMs) > 0 && (p.revents & POLLIN)) {
      input_event events[32];
      // A single read: never block here, even on a blocking descriptor
      ssize_t n = read(sTouchFd, events, sizeof(events));
      if (n > 0) {
        for (int i = 0; i < (int)(n / sizeof(input_event)); i++) {
          const input_event & e = events[i];
          if (e.type == EV_KEY && e.code == BTN_TOUCH) {
            down = e.value != 0;
          } else if (e.type == EV_ABS && e.code == ABS_MT_TRACKING_ID) {
            down = e.value >= 0;
          } else if (e.type == EV_ABS) {
            lastActivity = nowMs();
          }
        }
      }
    }
    uint64_t now = nowMs();
    bool touching = down || (lastActivity != 0 && now - lastActivity < 300);
    if (!touching) {
      downSince = 0;
      return false;
    }
    if (downSince == 0) {
      downSince = now;
    }
    return now - downSince >= k_rescueHoldMs;
  }
};

void * watchdogMain(void *) {
  sigset_t set;
  sigfillset(&set);
  sigdelset(&set, SIGSEGV);
  sigdelset(&set, SIGBUS);
  sigdelset(&set, SIGILL);
  sigdelset(&set, SIGFPE);
  sigdelset(&set, SIGABRT);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  loadSymbols();
  if (!sSymbolsReady) {
    Ion::Kindle::Debug::log("debug: no symbol table, backtraces will show raw addresses");
  }

  bool stalled = false;
  uint64_t stallStart = 0;
  uint64_t lastReport = 0;
  uint64_t lastStatus = nowMs();
  uint32_t lastScans = 0;
  Rescue rescue;

  while (true) {
    uint64_t now = nowMs();
    uint64_t beat = __atomic_load_n(&sLastBeat, __ATOMIC_RELAXED);
    uint64_t since = now - (beat != 0 ? beat : sStartMs);
    uint64_t threshold = beat != 0 ? k_stallMs : k_startupStallMs;

    if (sDumpRequested) {
      sDumpRequested = 0;
      report("STATE REPORT (requested with SIGQUIT)", 0);
    }

    if (since >= threshold) {
      if (!stalled) {
        stalled = true;
        stallStart = beat != 0 ? beat : sStartMs;
        rescue.reset();
        report("WATCHDOG: main loop is stuck", since);
        Ion::Kindle::Debug::log("  hold a finger anywhere on the screen for %llu s to quit Upsilon",
            (unsigned long long)(k_rescueHoldMs / 1000));
        lastReport = now;
      } else if (now - lastReport >= k_repeatReportMs) {
        report("WATCHDOG: main loop is still stuck", since);
        lastReport = now;
      }
      if (rescue.poll(200)) {
        Ion::Kindle::Debug::log("RESCUE: screen held for %llu s while Upsilon was stuck, exiting (state since last autosave is lost)",
            (unsigned long long)(k_rescueHoldMs / 1000));
        report("STATE AT RESCUE EXIT", nowMs() - stallStart);
        _exit(70);
      }
      continue;
    }
    if (stalled) {
      stalled = false;
      Ion::Kindle::Debug::log("watchdog: main loop resumed after %.1f s", (now - stallStart) / 1000.0);
    }

    if (sVerbose && now - lastStatus >= k_statusEveryMs) {
      Ion::Kindle::Debug::log("status: %.0f polls/s, touch events %u, taps %u, e-ink updates %u (%u failed, slowest %u ms)",
          (sCounters.scans - lastScans) * 1000.0 / (now - lastStatus), sCounters.touchEvents, sCounters.taps,
          sCounters.einkUpdates, sCounters.einkFailures, sCounters.einkMaxMs);
      lastScans = sCounters.scans;
      lastStatus = now;
    }
    usleep(250000);
  }
  return nullptr;
}

#endif // KINDLE_DIAGNOSTICS

}

namespace Ion {
namespace Kindle {
namespace Debug {

#if KINDLE_DIAGNOSTICS
void init() {
  sStartMs = nowMs();
  sMainThread = pthread_self();
  sMainTid = (pid_t)syscall(SYS_gettid);
  const char * v = getenv("UPSILON_DEBUG");
  sVerbose = v != nullptr && *v != 0 && strcmp(v, "0") != 0;

  // Crash handlers run on an alternate stack so stack overflows get logged
  static char altStack[64 * 1024];
  stack_t ss;
  ss.ss_sp = altStack;
  ss.ss_size = sizeof(altStack);
  ss.ss_flags = 0;
  sigaltstack(&ss, nullptr);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = onCrash;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  const int crashSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
  for (int s : crashSignals) {
    sigaction(s, &sa, nullptr);
  }

  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = onBacktraceRequest;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR2, &sa, nullptr);

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = onDumpRequest;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGQUIT, &sa, nullptr);

  signal(SIGPIPE, SIG_IGN);

  char uptime[64] = "?";
  FILE * f = fopen("/proc/uptime", "r");
  if (f != nullptr) {
    if (fscanf(f, "%63s", uptime) != 1) {
      strcpy(uptime, "?");
    }
    fclose(f);
  }
  log("upsilon for Kindle, built " __DATE__ " " __TIME__ ", pid %d, system uptime %s s%s", (int)getpid(), uptime,
      sVerbose ? ", verbose log" : "");
  for (char ** e = environ; *e != nullptr; e++) {
    if (strncmp(*e, "UPSILON_", 8) == 0) {
      log("  %s", *e);
    }
  }
}

bool verbose() {
  return sVerbose;
}

#else // Fast build: diagnostics compiled out

void init() {
  sStartMs = nowMs();
  signal(SIGPIPE, SIG_IGN);
  // No report to write: don't let debug.sh's SIGQUIT terminate us
  signal(SIGQUIT, SIG_IGN);
  log("upsilon for Kindle (fast build, diagnostics off), built " __DATE__ " " __TIME__ ", pid %d", (int)getpid());
}

#endif

void log(const char * format, ...) {
  char buffer[1024];
  uint64_t t = nowMs() - sStartMs;
  int n = snprintf(buffer, sizeof(buffer), "[%6llu.%03llu] ", (unsigned long long)(t / 1000), (unsigned long long)(t % 1000));
  va_list args;
  va_start(args, format);
  int m = vsnprintf(buffer + n, sizeof(buffer) - n - 1, format, args);
  va_end(args);
  if (m < 0) {
    m = 0;
  }
  n += m;
  if (n > (int)sizeof(buffer) - 2) {
    n = sizeof(buffer) - 2;
  }
  buffer[n++] = '\n';
  writeAll(buffer, n);
}

#if KINDLE_DIAGNOSTICS

void setPhase(const char * phase) {
  sPhase = phase;
  log("phase: %s", phase);
}

void heartbeat() {
  __atomic_store_n(&sLastBeat, nowMs(), __ATOMIC_RELAXED);
  sCounters.scans++;
}

void setTouchFd(int fd) {
  sTouchFd = fd;
}

void startWatchdog() {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  pthread_t thread;
  if (pthread_create(&thread, &attr, watchdogMain, nullptr) != 0) {
    log("debug: could not start the watchdog");
  } else {
    pthread_detach(thread);
  }
  pthread_attr_destroy(&attr);
}

Counters & counters() {
  return sCounters;
}

#endif

}
}
}
