/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * Constrained Execution Runtime Library
 *
 */

#include "lib.h"

extern "C" {

// For errno constants
#define _ERRNO_H
#include <bits/errno.h>

// For SYS_* constaints
#include <syscall.h>

// Fix stupid gettimeofday prototype problems
#undef __THROW
#define __THROW

// For time types
#include <time.h>
#include <sys/time.h>

// Maximum size of a calling context
// If you change this, also change the value in lib/ICS/Instrumentor.cpp
#define MAX_CONTEXT 64

// XXX: gross: remove these?
// XXX: calling these libc fns does not work without --ics-opt-ignore-uclibc-sync
extern long int syscall(long int syscallno, ...);
extern void* memset(void *ptr, int c, size_t nbytes);
extern void* memcpy(void *dst, const void *src, size_t nbytes);
extern void* memmove(void *dst, const void *src, size_t nbytes);
extern size_t strlen(const char *s);
extern void qsort(void *base, size_t nmemb, size_t size,
                  int(*compar)(const void *, const void *));

// Very gross: this works around stupid linker issues
// The problem is that we have up to 4 definitions of some functions: the actual
// function (e.g., from uclibc), the model function (in runtime/POSIX), and the
// runtime function (defined here).  To avoid the need to link via multiple stages
// (which doesn't really work -- I tried), we link everything at once.  To avoid
// name conflicts, the model functions and runtime functions get prefixes.  Here,
// we specify the prefix for runtime functions (see also DEFINE_MODEL).
#define __DEFINE_ICSFUNC(icsname, rettype, realname, ...) \
  extern rettype realname(__VA_ARGS__); \
  rettype icsname(__VA_ARGS__); \
  __attribute__((used)) __attribute__((noinline)) rettype icsname(__VA_ARGS__)

#define __DECLARE_ICSFUNC(icsname, rettype, realname, ...) \
  extern rettype icsname(__VA_ARGS__)

#define DEFINE_ICSFUNC(rettype, name, ...) \
  __DEFINE_ICSFUNC(__ics_runtime_ ## name, rettype, name, ##__VA_ARGS__)

#define DECLARE_ICSFUNC(rettype, name, ...) \
  __DECLARE_ICSFUNC(__ics_runtime_ ## name, rettype, name, ##__VA_ARGS__)

#define CALL_ICSFUNC(name, ...) \
  __ics_runtime_ ## name(__VA_ARGS__)

#define DEFINE_ICS_INPUTWRAP(rettype, name, ...) \
  extern rettype name(__VA_ARGS__); \
  extern rettype MANGLED(name)(__VA_ARGS__); \
  void* __ics_dontdelete_##name = (void*)name; \
  __DEFINE_ICSFUNC(__ics_inputwrap_ ## name, rettype, name, ##__VA_ARGS__)

// For disabled macros
#define NOP do {} while(0)

// Ugly hack to ensure linkage (see also FORCE_LINKAGE)
__attribute__((used)) void __klee_force_ics_linkage(void) {}

//-----------------------------------------------------------------------------
// ICS Logging Tables
//-----------------------------------------------------------------------------

// Actual initializers for these tables will be defined during instrumentation.
// We use snapshots to support the following sequence of execution:
//
//    RegionMarker
//    ScheduleSelector   // 1) use values at start-of-region to select a schedule
//      :   // 2) normal execution within a region
//      :   //    might log to stackvars
//    ReadInput
//    ScheduleSelector   // 3) use the new input AND values at start-of-region to update schedule
//
// The the above example, (1) and (3) will read from the old "snapshot" buffer,
// and (2) will write to the "current" buffer.  We take a snapshot at every region
// barrier (unless we know that the above scenario is not possible, in which case
// we don't take a snapshot and (1) will read from the "current" buffer directly).
//
// UGH: volatile is needed to workaround LLVM bug #14309

// Double-buffered logs
extern uint8_t  volatile* const __ics_stackvars8[2][KLEE_MAX_THREADS];
extern uint16_t volatile* const __ics_stackvars16[2][KLEE_MAX_THREADS];
extern uint32_t volatile* const __ics_stackvars32[2][KLEE_MAX_THREADS];
extern uint64_t volatile* const __ics_stackvars64[2][KLEE_MAX_THREADS];

extern const uint32_t __ics_stackvars8_sizes[KLEE_MAX_THREADS];
extern const uint32_t __ics_stackvars16_sizes[KLEE_MAX_THREADS];
extern const uint32_t __ics_stackvars32_sizes[KLEE_MAX_THREADS];
extern const uint32_t __ics_stackvars64_sizes[KLEE_MAX_THREADS];

static const unsigned LogCurrIndex = 0;      // we ALWAYS write to [0]
static const unsigned LogSnapshotIndex = 1;  // the snapshot (not always valid)
static unsigned LogReadIndex = 1;   // we can read from [0] or [1] (see RegionBarrier())

// Single-buffered logs
// We assume these do NOT change within a region (other than to add new inputs).
// HOWEVER: see the TODO in MyThreadStartRoutine() for an unhandled case of threadlocals
extern void* volatile __ics_inputptrs[];
extern void* volatile __ics_threadlocalptrs[];

//-----------------------------------------------------------------------------
// ICS Data
//-----------------------------------------------------------------------------

struct HBSchedule {
  uint16_t numThreads;
  uint16_t nextThreadLeader;
  uint16_t nextScheduleSelectorId;
  uint16_t exitsProgram;
  uint16_t* threadSchedules[0];
};

union KLEE_CACHELINE_ALIGN VectorClockEntry {
  char __cacheline_padding__[KLEE_CACHELINE_SIZE];
  struct {
    volatile uint32_t timestamp;
    volatile bool needfutex;
  } info;
};

// The schedule for program entry
extern const HBSchedule __ics_schedule_fragment_1;

// The schedule fragments for the current region
// See Instrumentor.cpp for more info on the format of a schedule
static const HBSchedule* CurrFragmentList[32];
static volatile unsigned CurrFragmentId = 0;

// Static data tables
// See Instrumentor.cpp for more info
// Actual sizes and initializers for these tables will be defined during instrumentation
typedef const HBSchedule* (*SchedSelectorFn)(void);
typedef void (*SnapshotFn)(const void*);
extern const SchedSelectorFn __ics_schedule_selectors[];  // indexed by selector id
extern const SnapshotFn      __ics_snapshot_functions[];  // indexed by snapshot id
extern const char            __ics_snapshot_buffer[];     // indexed by snapshot offsets
extern const uint64_t*       __ics_regionctx_table[];     // indexed by region id
extern const uint64_t        __ics_callctx_table[];       // indexed by callctx id

// The global vector clock
static VectorClockEntry VectorClock[KLEE_MAX_THREADS];

// The global list of thread calling contexts (updated rarely)
// This points to each thread's MyThreadContextPtr
static const uint64_t*const * ThreadContexts[KLEE_MAX_THREADS];

// Mapping from logical threads to concrete threads
// Used by selector functions when the selector is a function of "thread id".
// All "klee_ics_lookup" functions translate their given thread id using this map.
// FIXME: the selectors should NOT be a function of the ids of threads that
// are created within the region itself (ugh)
// FIXME: how do we map concrete threads to logical threads after creation?
// probably: the schedule needs to have the logical id of the newly-created thread
// for now: assuming thread creations happen up-front before this mapping gets rejiggered
static uint32_t LogicalThreadMap[KLEE_MAX_THREADS];

// Used to implement the region barrier
static volatile unsigned NumThreadsRunningInRegion = 1;
static volatile unsigned NumThreadsTotal = 1;
static volatile uint64_t RegionTicker = 0;

// Used to order wait slots
static volatile uint32_t NextWaitSlot = 1;

// Per-thread info
// N.B.: The global list of these is defined in __tsync
static __thread thread_data_t *MyThreadInfo;
static __thread bool MyThreadJustStarted;
static __thread unsigned MyThreadId;
static __thread unsigned MyThreadCurrScheduleFragment;
static __thread uint64_t MyThreadContext[MAX_CONTEXT];
static __thread uint64_t *MyThreadContextPtr;
static __thread const uint16_t *MyThreadSchedulePtr;

static inline const HBSchedule* MyThreadCurrSchedule() {
  return CurrFragmentList[MyThreadCurrScheduleFragment];
}

//-----------------------------------------------------------------------------
// ICS Builtin Constants
// Must be in sync with those in Instrumentor.cpp
//-----------------------------------------------------------------------------

static const uint16_t InputBufferFlagBit = (1 << 15);
static const uint16_t EndFragmentSentinal = 0xffff;
static const uint16_t SkipHbNodeFlag = 0xfffe;

//-----------------------------------------------------------------------------
// Debug Logging
//-----------------------------------------------------------------------------

#ifdef ENABLE_DEBUGGING

static volatile uint32_t DbgPrintLock;

// Simple recursive spinlock for synchronized debug logging
__attribute__((noinline))
static void DbgAcquirePrintLock() {
  const uint32_t encodedId = (MyThreadId+1) << 4;
  while (1) {
    const uint32_t old = __sync_val_compare_and_swap(&DbgPrintLock, 0, encodedId);
    if (old == 0)
      break;
    if ((old & (~0xf)) == (encodedId & (~0xf))) {
      DbgPrintLock = old+1;
      __sync_synchronize();
      break;
    }
  }
}
__attribute__((noinline))
static void DbgReleasePrintLock() {
  const uint32_t encodedId = (MyThreadId+1) << 4;
  while (1) {
    const uint32_t old = DbgPrintLock;
    if (old == encodedId) {
      DbgPrintLock = 0;
      __sync_synchronize();
      break;
    }
    if ((old & (~0xf)) == (encodedId & (~0xf))) {
      DbgPrintLock = old - 1;
      __sync_synchronize();
      break;
    }
  }
}

// Formatted print routines
#define DbgPrint(fmt, ...) \
  do { \
    DbgAcquirePrintLock(); \
    PrintStderr(fmt, ##__VA_ARGS__); \
    DbgReleasePrintLock(); \
  } while (0)

#define DbgVPrint(fmt, ...) \
  do { \
    DbgAcquirePrintLock(); \
    __ics_vprintf(2, fmt, ap); \
    DbgReleasePrintLock(); \
  } while (0)

// Customized print routines
__attribute__((noinline))
static void DbgPrintRegionContext(const char *name, const uint64_t *ctxlist, int nthreads) {
  char tmp[256];
  char *bufp = tmp;
  char *endp = tmp + sizeof(tmp);
  bufp = __ics_snprintf(bufp, endp-bufp, "%s: %p ::", name, ctxlist);
  for (const uint64_t *x = ctxlist; nthreads && *x && bufp!=endp; --nthreads, x+=2) {
    bufp = __ics_snprintf(bufp, endp-bufp, " (%lu,%lu)", *x, *(x+1));
  }
  DbgPrint("%s\n", tmp);
}

__attribute__((noinline))
static void DbgLogPushContext(const char *msg, uint64_t cs, uint64_t newv) {
  char tmp[64];
  char *bufp = tmp;
  char *endp = tmp + sizeof(tmp);
  bufp = __ics_snprintf(bufp, endp-bufp, "T%u: ", MyThreadId);
  for (uint64_t *p = &MyThreadContext[0]; p < MyThreadContextPtr && bufp < endp-1; ++p) {
    *(bufp++) = ' ';
  }
  *bufp = '\0';
  DbgPrint("%s %s(%lu) => %lu\n", tmp, msg, cs, newv);
}

__attribute__((noinline))
static void DbgLogInput(const char *buf, size_t nbytes, const char *name) {
  if (nbytes > 32) {
    DbgPrint("T%u: INPUT: %s = [large buffer @ %p]\n", MyThreadId, name, buf);
  } else if (nbytes == 8) {
    DbgPrint("T%u: INPUT: %s = %lx\n", MyThreadId, name, *(uint64_t*)buf);
  } else if (nbytes == 4) {
    DbgPrint("T%u: INPUT: %s = %x\n", MyThreadId, name, *(uint32_t*)buf);
  } else if (nbytes < 4) {
    for (size_t k = 0; k < nbytes; ++k)
      DbgPrint("T%u: INPUT: %s[%lub] = %x\n", MyThreadId, name, k, (uint32_t)(unsigned char)buf[k]);
  } else {
    for (size_t k = 0; k < nbytes/4; ++k)
      DbgPrint("T%u: INPUT: %s[%luw] = %x\n", MyThreadId, name, k, *(int*)(buf + k));
  }
}

__attribute__((noinline))
static void DbgPrintCurrSchedule(const uint32_t fragnum) {
  DbgAcquirePrintLock();

  // Dump the given schedule fragment
  const HBSchedule *sched = CurrFragmentList[fragnum];
  DbgPrint("ScheduleFragment[%u] @ %p\n", fragnum, sched);
  DbgPrint("  numThreads:%u nextThreadLeader:%u nextSelectorId:%u exitsProgram:%u\n",
           sched->numThreads, sched->nextThreadLeader, sched->nextScheduleSelectorId,
           sched->exitsProgram);

  // Dump schedule for each thread that appears in this fragment
  for (unsigned t = 0; t < sched->numThreads; ++t) {
    DbgPrint("  Thread[%u]: {", t);
    uint16_t *x = sched->threadSchedules[t];
    while ((*x) != 0 && (*x) != EndFragmentSentinal) {
      uint16_t ctxid = *(x++);
      // Print "SN" for a sequence of "N" skip nodes
      if (ctxid == SkipHbNodeFlag) {
        int skip;
        for (skip = 1; *x == SkipHbNodeFlag; x++)
          skip++;
        DbgPrint("S%d ", skip);
        continue;
      }
      // Print the CtxId
      if ((ctxid & InputBufferFlagBit) != 0) {
        int inputId = *(x++);
        ctxid &= ~InputBufferFlagBit;
        DbgPrint("I%u:C%u[", inputId, ctxid);
      } else {
        DbgPrint("C%u[", ctxid);
      }
      // Print the incoming HB edges
      for (int numincoming = *(x++); numincoming > 0; --numincoming) {
        const uint32_t rawtid = *(x++);
        const uint32_t tid = LogicalThreadMap[rawtid];
        const uint32_t timestamp = *(x++);
        DbgPrint("R%u/T%u/%u,", rawtid, tid, timestamp);
      }
      DbgPrint("] ");
    }
    DbgPrint("}\n");
  }

  DbgReleasePrintLock();
}

__attribute__((noinline))
static void DbgPrintGlobalState() {
  unsigned maxThreads  = 0;

  DbgAcquirePrintLock();
  DbgPrint("================= ICS GLOBAL STATE =================\n");

  // Dump the schedule fragments
  for (unsigned k = 0; k <= CurrFragmentId; ++k) {
    DbgPrintCurrSchedule(k);
    if (CurrFragmentList[k]->numThreads > maxThreads)
      maxThreads = CurrFragmentList[k]->numThreads;
  }

  // Dump the vector clock
  DbgPrint("VectorClock:\n");
  for (unsigned k = 0; k < maxThreads; k += 4) {
    DbgPrint("  T%u:%u  T%u:%u  T%u:%u  T%u:%u\n",
             k+0, VectorClock[k+0].info.timestamp,
             k+1, VectorClock[k+1].info.timestamp,
             k+2, VectorClock[k+2].info.timestamp,
             k+3, VectorClock[k+3].info.timestamp);
  }

  // Dump info about the current thread
  DbgPrint("CurrentThread:\n");
  DbgPrint("  T%u, CurrFrag:%u Region:%lu\n", MyThreadId, MyThreadCurrScheduleFragment, RegionTicker);
  DbgPrint("================= END GLOBAL STATE =================\n");
  DbgReleasePrintLock();
}

#else

// Stubs for disabled debugging
#define DbgPrint(fmt, ...) NOP
#define DbgVPrint(fmt, ...) NOP
#define DbgPrintRegionContext(name, ctx, nthreads) NOP
#define DbgLogPushContext(msg, cs, newv) NOP
#define DbgLogInput(buf, nbytes, name) NOP
#define DbgPrintCurrSchedule(fragnum) NOP
#define DbgPrintGlobalState() NOP

#endif

//-----------------------------------------------------------------------------
// Statistics Logging
//-----------------------------------------------------------------------------

#ifdef ENABLE_INSTRUMENTATION

// Per-thread stats (these are reset at each region barrier)
static volatile uint64_t CurrInstructionCounts[KLEE_MAX_THREADS];
static volatile uint64_t AccumInstructionCounts;
static __thread uint64_t MyInstructionCount;
static __thread uint64_t MyInstructionCountMax;
static __thread uint64_t MyInstructionCountAccum;
static __thread uint64_t MyRegionCount;

// Stats updating
__attribute__((noinline))
void UpdateLocalInstructionCountStats() {
  MyRegionCount++;
  MyInstructionCountAccum += MyInstructionCount;
  if (MyInstructionCount > MyInstructionCountMax) {
    MyInstructionCountMax = MyInstructionCount;
  }
  CurrInstructionCounts[MyThreadId] = MyInstructionCount;
  PrintStderr("T%u: %lu %lu %lu\n", MyThreadId,
              MyInstructionCountMax, MyInstructionCountAccum/MyRegionCount, MyInstructionCount);
}

__attribute__((noinline))
void UpdateGlobalInstructionCountStats() {
  uint64_t accum = 0;
  uint64_t total = 0;
  uint64_t min = (1ULL << 63);
  uint64_t max = 0;
  // get the mean
  for (int k = 0; k < KLEE_MAX_THREADS; ++k) {
    uint64_t c = CurrInstructionCounts[k];
    if (c) {
      accum += c;
      total++;
      if (c < min) min = c;
      if (c > max) max = c;
    }
  }
  if (!total)
    return;

  // sum of squares
  uint64_t mean = accum / total;
  accum = 0;
  for (int k = 0; k < KLEE_MAX_THREADS; ++k) {
    uint64_t c = CurrInstructionCounts[k];
    if (c) {
      accum += (c - mean) * (c - mean);
    }
  }

  uint64_t r = max - min;
  uint64_t rp = ((double)r / (double)max) * 100.0;
  AccumInstructionCounts += mean;
  PrintStderr("Imbalance: r=%lu rp=%lu m=%lu s2=%lu\n", r, rp, mean, accum/total);
  PrintStderr("Avg: %lu\n", AccumInstructionCounts/(RegionTicker+1));
}

void ResetInstructionCountStatsForRegion() {
  MyInstructionCount = 0;
  CurrInstructionCounts[MyThreadId] = 0;
}

#else

#define UpdateLocalInstructionCountStats() NOP
#define UpdateGlobalInstructionCountStats() NOP
#define ResetInstructionCountStatsForRegion() NOP

#endif

//-----------------------------------------------------------------------------
// ICS Scheduling
//-----------------------------------------------------------------------------

static void AdvanceHbNode();
static void AdvanceToNextFragment();
static void RegionBarrier();

// Spin-wait until the condition holds, but with yielding

#define SPIN_WAIT_UNTIL(cond) \
  do { \
    for (int __spins = 0; ; ++__spins) { \
      if (cond) break; \
      if (__spins >= 128) { \
        __ics_call_real_sched_yield(); \
        __spins = 0; \
      } \
    } \
  } while (0)

// Wait via futex

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
static void futex_wait(volatile uint32_t *uaddr, uint32_t oldval) {
  DbgPrint("T%u: FUTEX.Wait\n", MyThreadId);
  do {} while(syscall(SYS_futex, uaddr, FUTEX_WAIT, oldval, NULL, NULL, 0) == EINTR);
}
static void futex_signal(volatile uint32_t *uaddr) {
  DbgPrint("T%u: FUTEX.Signal\n", MyThreadId);
  syscall(SYS_futex, uaddr, FUTEX_WAKE, KLEE_MAX_THREADS, NULL, NULL, 0);
}

// Contexts

static bool IsMyCurrentContextShortId(uint32_t ctxid) {
  return __ics_callctx_table[ctxid] == *MyThreadContextPtr;
}

static bool IsMyCurrentContextLongId(uint64_t ctxid) {
  return ctxid == *MyThreadContextPtr;
}

// Schedule flags

static bool IsFragmentComplete() {
  if (*MyThreadSchedulePtr == EndFragmentSentinal) {
    DbgPrint("T%u: FragmentComplete: ptr=%p\n", MyThreadId, MyThreadSchedulePtr);
    return true;
  }
  return false;
}

static bool IsScheduleComplete() {
  if (*MyThreadSchedulePtr == 0) {
    DbgPrint("T%u: ScheduleComplete: ptr=%p\n", MyThreadId, MyThreadSchedulePtr);
    return true;
  }
  return false;
}

static void SkipNopScheduleNodes() {
  // Invariant: MyThreadSchedulePtr always points at a non-skip node, so
  // this function must be call each time MyThreadSchedulePtr is updated.
  // (Updates happen via InitializeMyThreadSchedule and WaitForHbNode.)
  while (*MyThreadSchedulePtr == SkipHbNodeFlag) {
    DbgPrint("T%u: SkipNopScheduleNode @ NodeId=%d\n", MyThreadId,
             VectorClock[MyThreadId].info.timestamp+1);
    AdvanceHbNode();
    MyThreadSchedulePtr++;
  }
}

static void InitializeMyThreadSchedulePtr() {
  // Ensure that this thread appears in the current schedule fragment.
  // If not, we may need to wait for the *next* fragment.
  while (MyThreadId >= MyThreadCurrSchedule()->numThreads) {
    if (MyThreadCurrSchedule()->exitsProgram) {
      DbgPrint("WARNING: Thread T%u does not appear in program-exiting schedule fragment"
               " (currFragNum=%u currSched=%p numThreads=%u)\n",
               MyThreadId, MyThreadCurrScheduleFragment,
               MyThreadCurrSchedule(),
               MyThreadCurrSchedule()->numThreads);
      DbgPrintGlobalState();
    }
    if (MyThreadCurrSchedule()->nextScheduleSelectorId) {
      AdvanceToNextFragment();
    } else {
      // N.B.: This case can happen when the current thread's
      // end-of-region HB node appears in a *prior* schedule fragment
      if (MyThreadCurrScheduleFragment == 0) {
        DbgPrint("WARNING: Thread T%u has no schedule in current region"
                 " (currFragNum=%u currSched=%p numThreads=%u)\n",
                 MyThreadId, MyThreadCurrScheduleFragment,
                 MyThreadCurrSchedule(),
                 MyThreadCurrSchedule()->numThreads);
        DbgPrintGlobalState();
        assert(0);
      }
      RegionBarrier();
    }
  }

  // Now we can initialize
  MyThreadSchedulePtr = MyThreadCurrSchedule()->threadSchedules[MyThreadId];
  SkipNopScheduleNodes();
}

// Regions

struct PerThreadContext {
  uint64_t ctxid;
  uint64_t tid;
};
static inline int PerThreadContextCmp(const void *pa, const void *pb) {
  const uint64_t a = ((const PerThreadContext*)pa)->ctxid;
  const uint64_t b = ((const PerThreadContext*)pb)->ctxid;
  return (a == b) ? 0 : ((a < b) ? -1 : 1);
}

static size_t GetNextRegionId() {
  // Gather the current thread contexts
  PerThreadContext sorted[KLEE_MAX_THREADS];
  int nthreads = 0;
  for (int k = 0; k < KLEE_MAX_THREADS; ++k) {
    const uint64_t*const* ptr = ThreadContexts[k];
    if (!ptr)
      continue;
    sorted[nthreads].ctxid = **ptr;
    sorted[nthreads].tid = k;
    ++nthreads;
  }

  // Sort the current thread contexts
  qsort(sorted, nthreads, sizeof sorted[0], PerThreadContextCmp);
  DbgPrintRegionContext("TestingCurrentCtx", (uint64_t*)sorted, nthreads);

  // Assumed by printing functions
  assert(sizeof(sorted) == (KLEE_MAX_THREADS * 2 * sizeof(uint64_t)));

  // Dumb iteration over the regionctx table to find a match
  for (int rid = 1; __ics_regionctx_table[rid]; ++rid) {
    const uint64_t *ctxlist = __ics_regionctx_table[rid];
    int n;
    DbgPrint("TEST REGION: %d @ %p [0]=%lu\n", rid, ctxlist, *ctxlist);
    for (n = 0; n < nthreads && *ctxlist; ++n) {
      const uint64_t ctxid = *(ctxlist++);
      const uint64_t tid = *(ctxlist++);
      if (ctxid != sorted[n].ctxid) {
        DbgPrint("CHECKED REGION CTX, DOESN'T MATCH (rid=%d, nthr=%d)\n", rid, nthreads);
        DbgPrintRegionContext("TableCtx", __ics_regionctx_table[rid], nthreads);
        DbgPrintRegionContext("CurrentCtx", (uint64_t*)sorted, nthreads);
        goto nextregion;
      }
      LogicalThreadMap[tid] = sorted[n].tid;
    }
    // Found a match?
    if (n == nthreads && !*ctxlist)
      return rid;
  nextregion:;
  }

  assert(0);
  return 0;
}

extern "C++" {
template<class LogT>
static void SnapshotStackVarsLog(LogT *log, const unsigned S, const unsigned C, const unsigned k,
                                 const unsigned *nelem) {
  memcpy((void*)log[S][k], (const void*)log[C][k],  nelem[k] * sizeof(log[S][k][0]));
}
}

static void SnapshotStackVars() {
  // Copy current --> snapshot
  const unsigned S = LogSnapshotIndex;
  const unsigned C = LogCurrIndex;
  const unsigned NumT = NumThreadsTotal;
  for (unsigned k = 0; k < NumT; ++k) {
    SnapshotStackVarsLog(__ics_stackvars8,  S, C, k,  __ics_stackvars8_sizes);
    SnapshotStackVarsLog(__ics_stackvars16, S, C, k,  __ics_stackvars16_sizes);
    SnapshotStackVarsLog(__ics_stackvars32, S, C, k,  __ics_stackvars32_sizes);
    SnapshotStackVarsLog(__ics_stackvars64, S, C, k,  __ics_stackvars64_sizes);
  }
}

static void WaitAtRegionBarrier(const uint64_t oldticker) {
  SPIN_WAIT_UNTIL(RegionTicker != oldticker);
  __sync_synchronize();
}

static void ResetVectorClocks() {
  for (int i = 0; i < KLEE_MAX_THREADS; ++i) {
    VectorClock[i].info.timestamp = 0;
    VectorClock[i].info.needfutex = false;
  }
}

static void ReleaseRegionBarrier() {
  NumThreadsRunningInRegion = NumThreadsTotal;
  ResetVectorClocks();
  __sync_synchronize();
  __sync_add_and_fetch(&RegionTicker, 1);
}

static void RegionBarrier() {
  // Should not attempt a region barrier while the program is exiting!
  assert(!MyThreadCurrSchedule()->exitsProgram
         && "RegionBarrier() called when program should exit: possible deadlock?");

  DbgPrint("T%u: RegionBarrier!\n", MyThreadId);

  // Do some stats first
  UpdateLocalInstructionCountStats();

  // Region barrier
  const uint64_t oldticker = RegionTicker;
  const unsigned left = __sync_sub_and_fetch(&NumThreadsRunningInRegion, 1);

  if (left == 0) {
    DbgPrint("Region Ended @ T%u :: Advancing\n", MyThreadId);
    // Read from the "current" log to pick the next schedule
    LogReadIndex = LogCurrIndex;
    // The region ended, so advance to the next region
    const size_t rid = GetNextRegionId();
    DbgPrint("=== STARTING NEW REGION ===\n");
    DbgPrint("Region: %lu\n", rid);
    DbgPrint("Snapshot[%lu]: %p\n", rid, __ics_snapshot_functions[rid]);
    DbgPrint("Selector[%lu]: %p\n", rid, __ics_schedule_selectors[rid]);
    __ics_snapshot_functions[rid](NULL);
    CurrFragmentId = 0;
    CurrFragmentList[0] = __ics_schedule_selectors[rid]();
    DbgPrint("Sched: %p\n", CurrFragmentList[0]);
    DbgPrintCurrSchedule(0);
    DbgPrint("LogicalThreadMap:\n");
    for (unsigned k = 0; k < CurrFragmentList[0]->numThreads; k += 4) {
      DbgPrint("  T%u:%u  T%u:%u  T%u:%u  T%u:%u\n",
               k+0, LogicalThreadMap[k+0],
               k+1, LogicalThreadMap[k+1],
               k+2, LogicalThreadMap[k+2],
               k+3, LogicalThreadMap[k+3]);
    }
    // Stats
    UpdateGlobalInstructionCountStats();
    // Need to snapshot if the next fragment reads dynamic input
    if (CurrFragmentList[0]->nextScheduleSelectorId) {
      SnapshotStackVars();
      LogReadIndex = LogSnapshotIndex;
    }
    // Done
    DbgPrint("T%u: RegionBarrier RELEASING\n", MyThreadId);
    ReleaseRegionBarrier();
  } else {
    DbgPrint("T%u: RegionBarrier WAIT\n", MyThreadId);
    DbgPrint("T%u: Waiting for End Of Region\n", MyThreadId);
    WaitAtRegionBarrier(oldticker);
    DbgPrint("T%u: RegionBarrier RELEASED\n", MyThreadId);
  }

  // Reset stats
  ResetInstructionCountStatsForRegion();

  // Update my thread schedule
  MyThreadCurrScheduleFragment = 0;
  InitializeMyThreadSchedulePtr();
}

// Schedule fragments

static void WaitForNextFragment() {
  SPIN_WAIT_UNTIL(CurrFragmentId >= MyThreadCurrScheduleFragment);
  __sync_synchronize();
}

static void UpdateCurrFragmentId(unsigned oldId, unsigned newId) {
  // Should ALWAYS success, since there is just one writer
  // N.B.: This could simplify to {fence; CurrFragmentId = newId;},
  // but we keep the CAS as it is more likely to detect bugs.
  if (!__sync_bool_compare_and_swap(&CurrFragmentId, oldId, newId)) {
    DbgPrint("UpdateCurrFragmentId(*p=%u, old=%u, new=%u) FAILED!\n",
             CurrFragmentId, oldId, newId);
    assert(0);
  }
}

static void AdvanceToNextFragment() {
  const HBSchedule *oldSchedule = MyThreadCurrSchedule();
  const unsigned oldFragment = MyThreadCurrScheduleFragment;
  MyThreadCurrScheduleFragment++;

  // Am I selecting the next schedule?
  if (oldSchedule->nextThreadLeader == MyThreadId) {
    const uint32_t sid = oldSchedule->nextScheduleSelectorId;
    SchedSelectorFn fn = __ics_schedule_selectors[sid];
    CurrFragmentList[MyThreadCurrScheduleFragment] = fn();
    DbgPrint("=== STARTING NEXT FRAGMENT (%u) ===\n", MyThreadCurrScheduleFragment);
    DbgPrint("Selector[%u]: %p\n", sid, fn);
    DbgPrint("Sched: %p\n", CurrFragmentList[MyThreadCurrScheduleFragment]);
    DbgPrintCurrSchedule(MyThreadCurrScheduleFragment);
    UpdateCurrFragmentId(oldFragment, MyThreadCurrScheduleFragment);
  } else {
    DbgPrint("T%u: Waiting for End Of Fragment #%d\n", MyThreadId, oldFragment);
    WaitForNextFragment();
  }

  // Update my thread schedule
  InitializeMyThreadSchedulePtr();
}

static void AdvanceToNextFragmentIfComplete() {
  while (IsFragmentComplete())
    AdvanceToNextFragment();
}

// Wait Slots

static void TakeNextWaitSlot() {
  // This should be invoked before each call to a CanSleep() function, unless
  // it is known that the thread will always be notified via notifyAll.
  MyThreadInfo->wait_slot = __sync_fetch_and_add(&NextWaitSlot, 1);
}

// HB Sync

static bool WaitForHbNode(int *inputId = NULL, const bool usefutex = false) {
  //
  // Waits until all incoming HB edges have been satisfied for the current program point.
  // In some cases, we may call this function even if there is no HB node at the current
  // program point.  In these cases, we return false -- otherise, we wait for the incoming
  // HB edges and return true.
  //
  // If the HB node contains dynamic input, we assert "inputId!=NULL" and assign
  // "*inputId" to the ID of the input being read.  In the HB node does not have input,
  // then we assign "*inputId = 0" (as "0" is an invalid id).  If caller does not
  // expect input at the current node, then NULL can be passed for "inputId".
  //

  // End of fragment => wait for the next fragment
  AdvanceToNextFragmentIfComplete();

  // End of schedule => no HB node here
  if (IsScheduleComplete())
    return false;

  // Check if the next HB node has a matching ctx
  const int raw = *MyThreadSchedulePtr;
  const int ctxid = raw & ~InputBufferFlagBit;
  DbgPrint("T%u: WaitForHbNode: Checking raw=%x ctx=%d @ NodeId=%d\n", MyThreadId, raw, ctxid,
           VectorClock[MyThreadId].info.timestamp+1);
  if (!IsMyCurrentContextShortId(ctxid)) {
    DbgPrint("T%u: TableCtx :: %lu\n", MyThreadId, __ics_callctx_table[ctxid]);
    DbgPrint("T%u: MyThreadCtx :: %lu\n", MyThreadId, *MyThreadContextPtr);
    return false;
  }

  // If we are expecting input, this HB node must have input.
  // N.B.: Due to translation of model functions to runtime functions,
  // we can have a sequence of HB nodes that all have the same ctx, e.g.,
  // 3 inputs followed by 1 region marker can happen in ftw().  Suppose
  // the 3 inputs are skipped: in this case, we do not want to "eat" the
  // region marker HB node when processing one of the inputs.  Hence, we
  const bool expectingInput = (inputId != NULL);
  const bool nodeHasInput = ((raw & InputBufferFlagBit) != 0);
  if (expectingInput != nodeHasInput) {
    DbgPrint("T%u: WaitForHbNode: Mismatched input bit (%d vs %d)\n", MyThreadId, expectingInput,
             nodeHasInput);
    return false;
  }

  // Found an HB node
  MyThreadSchedulePtr++;

  // Capture input if requested
  if (inputId) {
    assert((raw & InputBufferFlagBit) != 0);
    *inputId = *(MyThreadSchedulePtr++);
    DbgPrint("T%u: WaitForHbNode: Captured input (%d)\n", MyThreadId, *inputId);
  }

  // Wait for all incoming edges
  const int numincoming = *(MyThreadSchedulePtr++);
  for (int i = 0; i < numincoming; ++i) {
    const uint32_t tid = LogicalThreadMap[*(MyThreadSchedulePtr++)];
    const uint32_t timestamp = *(MyThreadSchedulePtr++);
    volatile uint32_t *clock = &VectorClock[tid].info.timestamp;
    DbgPrint("T%u: WaitForHbNode: Wait for thread T%u (%u vs %u)\n", MyThreadId, tid,
             timestamp, *clock);
    if (0 && usefutex) {
      VectorClock[tid].info.needfutex = true;
      __sync_synchronize();
      for (;;) {
        const uint32_t c = *clock;
        if (c >= timestamp)
          break;
        futex_wait(clock, c);
      }
    } else {
      SPIN_WAIT_UNTIL((*clock) >= timestamp);
    }
  }

  DbgPrint("T%u: WaitForHbNode: Finish\n", MyThreadId);
  __sync_synchronize();

  SkipNopScheduleNodes();
  return true;
}

static void AdvanceHbNode() {
  // Advances our logical clock by one time step.
  // This should be called exactly once for each time WaitForSchedule() returns true.
  VectorClock[MyThreadId].info.timestamp++;
  if (VectorClock[MyThreadId].info.needfutex)
    futex_signal(&VectorClock[MyThreadId].info.timestamp);
}

#ifdef ENABLE_DEBUGGING
__attribute__((noinline))
#endif
static void WaitForHbNodeMustExist(bool canSleep = false, int *inputId = NULL, bool usefutex = false) {
  if (canSleep) {
    while (IsScheduleComplete()) {
      DbgPrint("T%u: Sleeping\n", MyThreadId);
      RegionBarrier();
    }
  }
  bool ok = WaitForHbNode(inputId, usefutex);
  assert(ok);
}

static void WaitForHbNodeMustExistCanSleep(int *inputId = NULL, bool usefutex = false) {
  WaitForHbNodeMustExist(true, inputId, usefutex);
}

static void DoOneSyncOpMustExist(int *inputId = NULL) {
  WaitForHbNodeMustExist(false, inputId, false);
  AdvanceHbNode();
}

static void DoOneSyncOpMustExistCanSleep(int *inputId = NULL, bool usefutex = false) {
  WaitForHbNodeMustExist(true, inputId, usefutex);
  AdvanceHbNode();
}

static bool SkipRegionBarriersOrExit(void) {
  // This is called from contexts where one of three things is possible:
  //   (1) The schedule completed, leading to an end-of-region, or
  //   (2) The schedule completed, leading to (eventual) program exit, or
  //   (3) The schedule did not yet complete
  // We return "true" in case (2) only (meaning: the program is about to exit)
  while (IsScheduleComplete()) {
    if (MyThreadCurrSchedule()->exitsProgram)
      return true;
    RegionBarrier();
  }
  return false;
}

//-----------------------------------------------------------------------------
// ICS API: Basics
//-----------------------------------------------------------------------------

extern void __ics_log_threadlocaladdrs(uint32_t threadId);

static void MyThreadCommonInit() {
  // Caller must setup:
  // MyThreadInfo
  // MyThreadId
  // MyThreadCurrScheduleFragment

  // Setup local info
  MyThreadJustStarted = true;
  MyThreadContextPtr = &MyThreadContext[0];
  memset(MyThreadContext, 0, sizeof MyThreadContext);
  InitializeMyThreadSchedulePtr();

  // Setup global info
  ThreadContexts[MyThreadId] = &MyThreadContextPtr;
  __ics_log_threadlocaladdrs(MyThreadId);
}

static void MyThreadCommonTeardown() {
  // Unset global info
  ThreadContexts[MyThreadId] = NULL;
}

DEFINE_ICSFUNC(void, klee_ics_initialize) {
  DbgPrint("=== INITIALIZE ===\n");

  // Initialize the schedule
  CurrFragmentList[0] = &__ics_schedule_fragment_1;

  // Setup global thread info
  __tsync.live_threads = 1;
  for (int k = 0; k < KLEE_MAX_THREADS; ++k)
    LogicalThreadMap[k] = k;

  // Setup the default thread
  MyThreadInfo = &__tsync.threads[0].data;
  MyThreadInfo->allocated = true;
  MyThreadInfo->joinable = true;
  MyThreadId = 0;
  MyThreadCurrScheduleFragment = 0;
  MyThreadCommonInit();

  DbgPrint("Sched: %p\n", MyThreadCurrSchedule());
  DbgPrint("ThreadSched: %p\n", MyThreadSchedulePtr);
  DbgPrint("ThreadSched[0]: %d\n", *MyThreadSchedulePtr);

  // Must do srand() at the end, after the thread-locals
  // have been setup, because it calls into uclibc
  __ics_srand();

  DbgPrint("=== RUNNING ===\n");
}

DEFINE_ICSFUNC(void, klee_ics_region_marker) {
  DbgPrint("T%u: RegionMarker\n", MyThreadId);
  DoOneSyncOpMustExist();
  AdvanceToNextFragmentIfComplete();
  if (IsScheduleComplete() && !MyThreadCurrSchedule()->exitsProgram)
    RegionBarrier();
}

DEFINE_ICSFUNC(void, klee_ics_begin) {
  DbgPrint("=== DOING ICS BEGIN ===\n");
  assert(NumThreadsTotal == 1);
  assert(MyThreadId == 0);
  DoOneSyncOpMustExist();
}

DEFINE_ICSFUNC(void, klee_ics_panic_nosched, uint32_t sid) {
  PrintStderr("Couldn't compute schedule in selector %u!\n", sid);
  __ics_call_real_abort();
}

DEFINE_ICSFUNC(void, klee_ics_do_initial_syncop) {
  if (MyThreadJustStarted) {
    DoOneSyncOpMustExist();
    MyThreadJustStarted = false;
  }
}

DEFINE_ICSFUNC(void, klee_ics_count_instructions, uint32_t count) {
#ifdef ENABLE_INSTRUMENTATION
  MyInstructionCount += count;
#endif
}

//-----------------------------------------------------------------------------
// ICS API: Stack Logging
//-----------------------------------------------------------------------------

DEFINE_ICSFUNC(void, klee_ics_callstack_push, uint64_t cs) {
  const uint64_t oldv = *MyThreadContextPtr;
  const uint64_t newv = 3*oldv + cs;
  DbgLogPushContext("PushCtx", cs, newv);
  MyThreadContextPtr++;
  *MyThreadContextPtr = newv;
}

DEFINE_ICSFUNC(void, klee_ics_callstack_pop) {
  *MyThreadContextPtr = 0;
  MyThreadContextPtr--;
  DbgLogPushContext("PopCtxx", 0, *MyThreadContextPtr);
}

DEFINE_ICSFUNC(uint64_t, klee_ics_lookupthreadid, uint32_t logical_tid) {
  return LogicalThreadMap[logical_tid];
}

DEFINE_ICSFUNC(uint32_t, klee_ics_lookupwaitslot, uint32_t logical_tid) {
  return __tsync.threads[LogicalThreadMap[logical_tid]].data.wait_slot;
}

// N.B.: If any logging function is passed 0 for ctxid, this means that the value
// should *always* be logged, so we don't need to check the current ctx (we skip
// ctx checking to optimizes cases where we must ALWAYS have the correct ctx).

extern "C++" {
template<class T>
static inline T lookupvar(uint32_t logical_tid, uint32_t oid, T volatile*const* log) {
  uint32_t tid = LogicalThreadMap[logical_tid];
  DbgPrint("XXX: Lookup var[%u,%u,%u] via %p @ %p\n", LogReadIndex, tid, oid, log, &log[tid][oid]);
  DbgPrint("XXX: Lookup var[%u,%u,%u] = %x\n", LogReadIndex, tid, oid, (uint32_t)log[tid][oid]);
  return log[tid][oid];
}

template<class T>
static inline void logvar(uint32_t oid, uint64_t longctxid, T val, T volatile*const* log) {
  if (!longctxid || IsMyCurrentContextLongId(longctxid)) {
    DbgPrint("XXX: Log var[%u,%u,%u] via %p @ %p\n", LogCurrIndex, MyThreadId, oid, log, &log[MyThreadId][oid]);
    log[MyThreadId][oid] = val;
    DbgPrint("XXX: Log var[%u,%u,%u] = %x\n", LogCurrIndex, MyThreadId, oid, (uint32_t)log[MyThreadId][oid]);
  }
}
}

#define STACKVAR_FUNCS(bits) \
  DEFINE_ICSFUNC(uint##bits##_t, klee_ics_lookup_stackvar_int##bits, uint32_t tid, uint32_t oid) { \
    return lookupvar(tid, oid, __ics_stackvars##bits[LogReadIndex]); \
  } \
  DEFINE_ICSFUNC(void, klee_ics_log_stackvar_int##bits, uint32_t oid, uint64_t ctxid, uint##bits##_t val) { \
    logvar(oid, ctxid, val, __ics_stackvars##bits[LogCurrIndex]); \
  }

STACKVAR_FUNCS(8)
STACKVAR_FUNCS(16)
STACKVAR_FUNCS(32)
STACKVAR_FUNCS(64)

#undef STACKVAR_FUNCS

#define SNAPSHOT_FUNCS(bits) \
  DEFINE_ICSFUNC(uint##bits##_t, klee_ics_snapshot_lookup_int##bits, uint32_t offset) { \
    DbgPrint("XXX: Lookup input[%u,%db]\n", offset, bits); \
    uint##bits##_t *ptr = (uint##bits##_t*)(__ics_snapshot_buffer + offset); \
    DbgPrint("XXX: Lookup input[%u,%db] = %lx\n", offset, bits, (uint64_t)*ptr); \
    return *ptr; \
  } \
  DEFINE_ICSFUNC(void, klee_ics_snapshot_log_int##bits, uint32_t offset, uint##bits##_t val) { \
    DbgPrint("XXX: Log input[%u,%db] = %lx\n", offset, bits, (uint64_t)val); \
    uint##bits##_t *ptr = (uint##bits##_t*)(__ics_snapshot_buffer + offset); \
    *ptr = val; \
  }

SNAPSHOT_FUNCS(8)
SNAPSHOT_FUNCS(16)
SNAPSHOT_FUNCS(32)
SNAPSHOT_FUNCS(64)

#undef SNAPSHOT_FUNCS

DEFINE_ICSFUNC(void*, klee_ics_lookup_threadlocaladdr, uint32_t oid) {
  return __ics_threadlocalptrs[oid];
}

DEFINE_ICSFUNC(void, klee_ics_log_threadlocaladdr, uint32_t oid, char *ptr) {
  __ics_threadlocalptrs[oid] = ptr;
}

//-----------------------------------------------------------------------------
// Klee API Stubs
//-----------------------------------------------------------------------------

// XXX: ugh: is there a better way?
static __thread bool DisableInput;

static int BiasedRandInt32(void) {
  // In test code, we commonly compare against the following numbers,
  // so bias the random number generator towards these cases so we can
  // test them more frequently.
  static const int biased[20] =
    { -1, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 10, 12, 20, 22, 30, 40, 50, 100, 101 };
  const int r = __ics_rand32();
  const int k = r % 30;
  return (k >= 20) ? r : biased[k];
}

static void InitRandomBuffer(char *buf, size_t nbytes) {
  int *intbuf = (int*)buf;
  for (; nbytes >= 4; nbytes -= 4, intbuf++)
    *intbuf = BiasedRandInt32();
  if (nbytes) {
    buf = (char*)intbuf;
    for (; nbytes; nbytes--, buf++)
      *buf = (char)__ics_rand32();
  }
}

static void DoNewInput(const void *buf, size_t nbytes, const char *name) {
  DbgLogInput((const char*)buf, nbytes, name);

  // Advance through the HBSchedule
  int inputId = 0;
  const bool hasNode = WaitForHbNode(&inputId);

  // Ignore?
  if (!hasNode) {
    DbgPrint("T%u: INPUT was ignored\n", MyThreadId);
    return;
  }

  AdvanceHbNode();

  if (!inputId) {
    PrintStderr("ERROR: T%u: No input ID for INPUT!\n", MyThreadId);
    DbgPrintGlobalState();
    assert(0);
  }

  // There was an input node here: log the input and then
  // preemptively check if the HBSchedule fragment has ended
  DbgPrint("XXX: Log function is %p (for %d)\n", __ics_snapshot_functions[inputId], inputId);
  __ics_snapshot_functions[inputId](buf);
  AdvanceToNextFragmentIfComplete();
}

static uint64_t AddIntInput(uint64_t x, const char *name) {
  InitRandomBuffer((char*)&x, sizeof x);
  DoNewInput((char*)&x, sizeof x, name);
  return x;
}

static uint64_t AddIntRangeInput(uint64_t rnd, uint64_t start, uint64_t end, const char *name) {
  if (start+1 == end) {
    return AddIntInput(start, name);
  } else {
    assert(start < end);
    return AddIntInput(rnd % (end - 1 - start) + start, name);
  }
}

DEFINE_ICSFUNC(void, klee_make_symbolic, void *buf, size_t nbytes, const char *name) {
  InitRandomBuffer((char*)buf, nbytes);
  DoNewInput(buf, nbytes, name);
}

DEFINE_ICSFUNC(void, klee_read_symbolic_input, const void *buf, size_t nbytes, const char *name) {
  if (DisableInput)
    return;
  DoNewInput(buf, nbytes, name);
}

#define INT_FUNCS(bits, type) \
  DEFINE_ICSFUNC(u##type, klee_int##bits, const char *name) { \
    return AddIntInput(__ics_rand##bits(), name); \
  } \
  DEFINE_ICSFUNC(type, klee_range##bits, type start, type end, const char *name) { \
    return AddIntRangeInput(__ics_rand##bits(), start, end, name); \
  }

INT_FUNCS(8,  int8_t)
INT_FUNCS(32, int32_t)
INT_FUNCS(64, int64_t)

#undef INT_FUNCS

// FIXME: is this what we want?
DEFINE_ICSFUNC(void, klee_assume, uintptr_t cond) {
  assert(cond);
}

DEFINE_ICSFUNC(void, klee_warning, const char *msg) {
  DbgPrint("WARNING(T%u): %s\n", MyThreadId, msg);
}

static void __do_klee_debug(const char *type, bool hasexpr, const char *msg, va_list ap) {
#ifdef ENABLE_DEBUGGING
  char tmp[32+__ics_strlen(msg)];
  if (hasexpr) {
    __ics_snprintf(tmp, sizeof tmp, "%s(T%u): %s: %%d\n", type, MyThreadId, msg);
  } else {
    __ics_snprintf(tmp, sizeof tmp, "%s(T%u): %s", type, MyThreadId, msg);
  }
  DbgVPrint(tmp, ap);
#endif
}

DEFINE_ICSFUNC(void, klee_debug, const char *msg, ...) {
  va_list ap;
  va_start(ap, msg);
  __do_klee_debug("DEBUG", false, msg, ap);
  va_end(ap);
}

DEFINE_ICSFUNC(void, klee_print_expr, const char *msg, ...) {
  va_list ap;
  va_start(ap, msg);
  __do_klee_debug("DEBUG.EXPR", true, msg, ap);
  va_end(ap);
}

DEFINE_ICSFUNC(void, klee_print_expr_only, const char *msg, ...) {
  va_list ap;
  va_start(ap, msg);
  __do_klee_debug("DEBUG.EXPR", true, msg, ap);
  va_end(ap);
}

DEFINE_ICSFUNC(void, klee_print_expr_concretizations, const char *msg, ...) {
  va_list ap;
  va_start(ap, msg);
  __do_klee_debug("DEBUG.EXPR", true, msg, ap);
  va_end(ap);
}

DEFINE_ICSFUNC(void, klee_report_error, const char *file, int line, const char *message, const char *suffix) {
  PrintStderr("ERROR.FATAL(T%u) at %s:%u: %s\n", MyThreadId, file, line, message);
  __ics_call_real_abort();
}

DEFINE_ICSFUNC(void, klee_assert_fail, const char *expr, const char *file, unsigned int line, const char *func) {
  PrintStderr("ASSERTION,FAILED(T%u) at %s:%u in %s: %s\n", MyThreadId, file, line, func, expr);
  DbgPrintGlobalState();
  __ics_call_real_abort();
}

DEFINE_ICSFUNC(uint64_t, klee_get_wlist) {
  return 0;
}

// These are called only by tests, and shouldn't be invoked from user code

DEFINE_ICSFUNC(void, klee_process_terminate, void) {
  DbgPrint("WARNING(T%u): Invoking klee_process_terminate\n", MyThreadId);
  __ics_call_real_exit();
}

DEFINE_ICSFUNC(void, klee_thread_terminate, void) {
  DbgPrint("WARNING(T%u): Invoking klee_thread_terminate\n", MyThreadId);
  __ics_call_real_pthread_exit(NULL);
}

DEFINE_ICSFUNC(void, klee_thread_preempt, int yield) {
  if (yield)
    __ics_call_real_sched_yield();
}

DEFINE_ICSFUNC(void, klee_thread_sleep, uint64_t, void*, uint64_t) {
  DbgPrint("WARNING(T%u): Invoking klee_thread_sleep\n", MyThreadId);
  TakeNextWaitSlot();
  DoOneSyncOpMustExistCanSleep();
  while (WaitForHbNode())
    AdvanceHbNode();
}

DEFINE_ICSFUNC(void, klee_thread_notify, uint64_t, void*, uint64_t, int, uint64_t, uint64_t, int64_t, int) {
  // This operation does NOT emit an HBNode for the current thread: the only
  // HBNode emitted applies to the notified thread (see klee.h for details).
  DbgPrint("WARNING(T%u): Invoking klee_thread_notify (this is a nop)\n", MyThreadId);
}

DEFINE_ICSFUNC(uint64_t, klee_add_hbnode, void) {
  DbgPrint("WARNING(T%u): Invoking klee_add_hbnode\n", MyThreadId);
  DoOneSyncOpMustExist();
  return 1;
}

DEFINE_ICSFUNC(unsigned, klee_is_symbolic, uintptr_t) {
  return 0;
}

DEFINE_ICSFUNC(unsigned, klee_is_running, void) {
  return 0;
}

DEFINE_ICSFUNC(uint64_t, klee_get_concrete_thread_context, void) {
  return MyThreadId;
}

//-----------------------------------------------------------------------------
// Libc Stubs
//-----------------------------------------------------------------------------

// String wrappers

extern void* MANGLED(memset)(void *ptr, int c, size_t nbytes);
extern void* MANGLED(memcpy)(void *dst, const void *src, size_t nbytes);
extern void* MANGLED(memmove)(void *dst, const void *src, size_t nbytes);

void* __ics_dontdelete_memset = (void*)memset;
void* __ics_dontdelete_memcpy = (void*)memcpy;
void* __ics_dontdelete_memmove = (void*)memmove;

DEFINE_ICSFUNC(void, klee_memset, void *ptr, char c, size_t nbytes) {
  MANGLED(memset)(ptr, c, nbytes);
}

DEFINE_ICSFUNC(void, klee_memcpy, void *dst, const void *src, size_t nbytes) {
  MANGLED(memcpy)(dst, src, nbytes);
}

DEFINE_ICSFUNC(void, klee_memmove, void *dst, const void *src, size_t nbytes) {
  MANGLED(memmove)(dst, src, nbytes);
}

// Generally:
// * Return values come first
// * Then errno
// * Then buffers

#define CaptureSyscallResult() \
  do { klee_read_symbolic_input(&ret, sizeof ret, ""); \
       klee_read_symbolic_input(&errno, sizeof errno, ""); \
  } while(0)

DEFINE_ICS_INPUTWRAP(ssize_t, read, int fd, void *buf, size_t count) {
  ssize_t ret = MANGLED(read)(fd, buf, count);
  CaptureSyscallResult();
  klee_read_symbolic_input(buf, (ret >= 0 ? ret : 0), "");
  return ret;
}

DEFINE_ICS_INPUTWRAP(ssize_t, write, int fd, const void *buf, size_t count) {
  ssize_t ret = MANGLED(write)(fd, buf, count);
  CaptureSyscallResult();
  return ret;
}

DEFINE_ICS_INPUTWRAP(int, open, const char *pathname, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list arg;
    va_start(arg, flags);
    mode = va_arg(arg, mode_t);
    va_end(arg);
  }
  int ret = MANGLED(open)(pathname, flags, mode);
  CaptureSyscallResult();
  return ret;
}

DEFINE_ICS_INPUTWRAP(int, creat, const char *pathname, mode_t mode) {
  int ret = MANGLED(creat)(pathname, mode);
  CaptureSyscallResult();
  return ret;
}

DEFINE_ICS_INPUTWRAP(int, close, int fd) {
  int ret = MANGLED(close)(fd);
  CaptureSyscallResult();
  return ret;
}

DEFINE_ICS_INPUTWRAP(int, fstat, int fd, struct stat *buf) {
  int ret = MANGLED(fstat)(fd, buf);
  CaptureSyscallResult();
  klee_read_symbolic_input(buf, sizeof *buf, "");
  return ret;
}

DEFINE_ICS_INPUTWRAP(int, stat, const char *path, struct stat *buf) {
  int ret = MANGLED(stat)(path, buf);
  CaptureSyscallResult();
  klee_read_symbolic_input(buf, sizeof *buf, "");
  return ret;
}

DEFINE_ICS_INPUTWRAP(int, lstat, const char *path, struct stat *buf) {
  int ret = MANGLED(lstat)(path, buf);
  CaptureSyscallResult();
  klee_read_symbolic_input(buf, sizeof *buf, "");
  return ret;
}

// Time

DEFINE_ICS_INPUTWRAP(int, gettimeofday, struct timeval *tv, struct timezone *tz) {
  struct timeval tv_tmp;
  struct timezone tz_tmp;
  if (!tv)
    tv = &tv_tmp;
  if (!tz)
    tz = &tz_tmp;
  // FIXME: Technically this can fail (see runtime/POSIX/time.c)
  MANGLED(gettimeofday)(tv, tz);
  klee_read_symbolic_input(tv, sizeof *tv, "");
  klee_read_symbolic_input(tz, sizeof *tz, "");
  return 0;
}

DEFINE_ICS_INPUTWRAP(time_t, time, time_t *t) {
  // FIXME: Technically this can fail (see runtime/POSIX/time.c)
  time_t res = MANGLED(time)(t);
  klee_read_symbolic_input(&res, sizeof res, "");
  return res;
}

// Mmap

DEFINE_ICS_INPUTWRAP(void*, mmap, void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
  void *ret = MANGLED(mmap)(addr, length, prot, flags, fd, offset);
  CaptureSyscallResult();
  if (ret == (void*)-1) {
    klee_read_symbolic_input((void*)&ret, sizeof ret, "");  // dummy
  } else {
    klee_read_symbolic_input(ret, length, "");
  }
  return ret;
}

DEFINE_ICS_INPUTWRAP(void*, mmap2, void *addr, size_t length, int prot, int flags, int fd, off_t pgoffset) {
  void *ret = MANGLED(mmap2)(addr, length, prot, flags, fd, pgoffset);
  CaptureSyscallResult();
  if (ret == (void*)-1) {
    klee_read_symbolic_input((void*)&ret, sizeof ret, "");  // dummy
  } else {
    klee_read_symbolic_input(ret, length, "");
  }
  return ret;
}

DEFINE_ICS_INPUTWRAP(int, munmap, void *addr, size_t length) {
  int ret = MANGLED(munmap)(addr, length);
  CaptureSyscallResult();
  return ret;
}

// FTW

typedef int (*ftwfunc)(const char *fpath, const struct stat *sb, int typeflag);
static __thread ftwfunc MyCurrFtwFunc;
static __thread int MyCurrFtwFuncRetVal;

static int FtwFuncWrapper(const char *fpath, const struct stat *sb, int typeflag) {
  DbgPrint("T%u: FtwFuncWrapper(%d,%s): Start $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$\n", MyThreadId, typeflag, fpath);
  DisableInput = false;
  CALL_ICSFUNC(klee_ics_region_marker);
  DbgPrint("T%u: LogTypeFlag: %d\n", MyThreadId, typeflag);
  klee_read_symbolic_input(&typeflag, sizeof typeflag, "");
  klee_read_symbolic_input(fpath, strlen(fpath)+1, "");
  klee_read_symbolic_input(sb, sizeof *sb, "");
  MyCurrFtwFuncRetVal = MyCurrFtwFunc(fpath, sb, typeflag);
  DbgPrint("T%u: FtwFuncWrapper(%d,%s): End\n", MyThreadId, typeflag, fpath);
  DisableInput = true;
  return MyCurrFtwFuncRetVal;
}

DEFINE_ICS_INPUTWRAP(int, ftw, const char *dirpath, ftwfunc fn, int nopenfd) {
  MyCurrFtwFunc = fn;
  MyCurrFtwFuncRetVal = 0;
  // Invoke the system's ftw()
  DbgPrint("T%u: FtwStart(dir=%s)\n", MyThreadId, dirpath);
  DisableInput = true;  // ugh: disable inputs in the syscalls made internally by ftw()
  int ret = MANGLED(ftw)(dirpath, FtwFuncWrapper, nopenfd);
  DisableInput = false;
  DbgPrint("T%u: FtwExit(fnret=%d, ret=%d, errno=%d)\n", MyThreadId, MyCurrFtwFuncRetVal, ret, errno);
  // If ftw() completed via non-zero retval from fn(), then there is no
  // region marker here and we return directly.  Otherwise, we use "-1"
  // as a special flag to signal completion (see use of "ftw_retval" in
  // runtime/POSIX/io.c).
  if (MyCurrFtwFuncRetVal == 0) {
    int neg1 = -1;
    CALL_ICSFUNC(klee_ics_region_marker);
    klee_read_symbolic_input(&neg1, sizeof neg1, "");
  }
  klee_read_symbolic_input(&ret, sizeof ret, "");
  return ret;
}

//-----------------------------------------------------------------------------
// PThreads
//-----------------------------------------------------------------------------

typedef uint64_t mypthread_t;
DECLARE_ICSFUNC(void, pthread_exit, void *returnValue) __attribute__((noreturn));

DEFINE_ICSFUNC(mypthread_t, pthread_self) {
  return MyThreadId;
}

static void* MyThreadStartRoutine(void *raw) {
  // Setup this thread
  MyThreadInfo = (thread_data_t*)raw;
  MyThreadId = MyThreadInfo->id;
  MyThreadCurrScheduleFragment = MyThreadInfo->initial_fragment_id;
  assert(MyThreadId < MyThreadCurrSchedule()->numThreads);
  MyThreadCommonInit();

  // TODO: We do not currently support the following sequence of events when
  // they occur within the same region:
  //   T2: pthread_exit()
  //   T1: pthread_create()   // reuses tid=2
  //   T2: ...
  // We also do NOT check for this sequence, so if encountered, we expect the
  // program will fail, for the following reasons:
  //   * For the first thread with tid=2, we cannot assert(IsScheduleComplete())
  //     as the last step of pthread_exit()
  //   * For the second thread with tid=2, MyThreadSchedulePtr must point
  //     to the middle of a schedule, but it is always initialized to point
  //     at the beginning of a schedule (vai MyThreadCommonInit)
  //   * If the program reads dynamic input some time after pthread_create(),
  //     the resulting schedule selector can depend on state from the beginning
  //     of the region, which means it can depend on the value of a thread-local
  //     variable in the *first* T2, which has been destroyed.
  // These problems are non-trivial to fix ...
  // TODO: Should update the symbolic exec code to trigger a warning when it
  // detects pthread_create() following pthread_exit() on any path within a region

  // Execute
  DbgPrint("T%u: StartExecution f=%p\n", MyThreadId, MyThreadInfo->start_routine);
  void *returnValue = MyThreadInfo->start_routine(MyThreadInfo->start_arg);
  DbgPrint("T%u: Exiting via return\n", MyThreadId);
  CALL_ICSFUNC(pthread_exit, returnValue);

  // Shouldn't be reachable
  assert(0);
}

DEFINE_ICSFUNC(int, pthread_create, mypthread_t *thread, const pthread_attr_t *attr,
               void *(*startRoutine)(void*), void *startArg) {
  DbgPrint("T%u: PthreadCreate\n", MyThreadId);

  // This HB node serializes thread operations
  // We advance the HB node after selecting an id for the new thread
  WaitForHbNodeMustExist();

  // Look for an available slot
  // Since KLEE_MAX_THREADS is the same as in the symbolic runtime, this should
  // succeed iff the corresponding call succeeded during symbolic execution.
  thread_data_t *t = NULL;
  for (unsigned i = 0; i < KLEE_MAX_THREADS; ++i) {
    if (!__tsync.threads[i].data.allocated) {
      t = &__tsync.threads[i].data;
      klee_memset(t, 0, sizeof *t);
      t->id = i;
      t->allocated = true;
      *thread = i;
      __tsync.live_threads++;
      break;
    }
  }

  AdvanceHbNode();

  if (!t) {
    DbgPrint("T%u: PthreadCreate: EAGAIN\n", MyThreadId);
    return EAGAIN;
  }

  t->start_routine = startRoutine;
  t->start_arg = startArg;
  t->initial_fragment_id = MyThreadCurrScheduleFragment;
  t->joinable = true;
  __sync_add_and_fetch(&NumThreadsRunningInRegion, 1);
  __sync_add_and_fetch(&NumThreadsTotal, 1);

  DbgPrint("T%u: PthreadCreate: Spawning T%u\n", MyThreadId, t->id);

  // This HB node corresponds to the one generated by klee_thread_create()
  DoOneSyncOpMustExist();
  int ret = __ics_call_real_pthread_create(attr, MyThreadStartRoutine, t);
  if (ret != 0) {
    PrintStderr("T%u: PthreadCreate: Failed to spawn T%u: errno=%u\n", MyThreadId, t->id, ret);
    assert(0 && "pthread_create failed");
  }

  return 0;
}

DEFINE_ICSFUNC(void, pthread_exit, void *returnValue) {
  DbgPrint("T%u: PthreadExit\n", MyThreadId);

  while (MyThreadInfo->cleanup_stack) {
    _pthread_cleanup_buffer *b = MyThreadInfo->cleanup_stack;
    b->__routine(b->__arg);
    MyThreadInfo->cleanup_stack = b->__prev;
  }

  // This HB node serializes thread operations
  // We advance the HB node after marking the thread terminated
  WaitForHbNodeMustExist();

  if (MyThreadInfo->joinable) {
    MyThreadInfo->terminated = true;
    MyThreadInfo->return_value = returnValue;
  } else {
    MyThreadInfo->allocated = false;
  }

  __tsync.live_threads--;
  __sync_sub_and_fetch(&NumThreadsRunningInRegion, 1);
  __sync_sub_and_fetch(&NumThreadsTotal, 1);

  AdvanceHbNode();
  assert(IsScheduleComplete());

  DbgPrint("T%u: Exiting...\n", MyThreadId);
  MyThreadCommonTeardown();
  __ics_call_real_pthread_exit(returnValue);
}

DEFINE_ICSFUNC(int, pthread_join, mypthread_t thread, void **returnValue) {
  DbgPrint("T%u: PthreadJoin on T%lu\n", MyThreadId, thread);

  // This HB node serializes thread operations
  // We advance the HB node after selecting the thread to join
  WaitForHbNodeMustExist();

  if (thread >= KLEE_MAX_THREADS) {
    DbgPrint("T%u: PthreadJoin: OOB\n", MyThreadId);
    AdvanceHbNode();
    return ESRCH;
  }
  if (thread == MyThreadId) {
    DbgPrint("T%u: PthreadJoin: DEADLK\n", MyThreadId);
    AdvanceHbNode();
    return EDEADLK;
  }

  thread_data_t *t = &__tsync.threads[thread].data;
  if (!t->allocated) {
    DbgPrint("T%u: PthreadJoin: SRCH\n", MyThreadId);
    AdvanceHbNode();
    return ESRCH;
  }
  if (!t->joinable) {
    DbgPrint("T%u: PthreadJoin: INVAL (not joinable)\n", MyThreadId);
    AdvanceHbNode();
    return EINVAL;
  }
  if (t->hasjoiner) {
    DbgPrint("T%u: PthreadJoin: INVAL (has joiner)\n", MyThreadId);
    AdvanceHbNode();
    return EINVAL;
  }

  DbgPrint("T%u: PthreadJoin: joining with T%lu\n", MyThreadId, thread);
  t->hasjoiner = true;
  AdvanceHbNode();

  // Do the join
  // Don't need to update wait_slot since we will be the only waiter
  DoOneSyncOpMustExistCanSleep(NULL, true);
  DbgPrint("T%u: PthreadJoin: joined with T%lu\n", MyThreadId, thread);
  assert(t->terminated);

  if (returnValue) {
    *returnValue = t->return_value;
  }

  t->allocated = false;
  return 0;
}

DEFINE_ICSFUNC(int, pthread_joinall, void) {
  DbgPrint("T%u: PthreadJoinAll\n", MyThreadId);

  // Wait for all threads to exit
  // CAREFUL: pthread_joinall/pthread_exit may interleave in different
  // ways than during symbolic execution, so "live_threads" may have a
  // different value!  There must be at least ONE HbNode, however.

  // First loop: gobble-up all HbNodes
  if (WaitForHbNode(NULL, true)) {
    AdvanceHbNode();
    while (__tsync.live_threads > 1) {
      // CAREFUL: If the schedule completed, we could be exiting the
      // program here, in which case we're done, but otherwise, there
      // must be a region barrier.
      if (SkipRegionBarriersOrExit())
        break;
      // Schedule isn't complete: there MIGHT be an HbNode (but the
      // next HbNode could be for something else ...)
      if (WaitForHbNode(NULL, true))
        AdvanceHbNode();
    }
  }

  // Second loop: wait for any remaining threads
  while (__tsync.live_threads > 1)
    __ics_call_real_sched_yield();

  // Done
  return 0;
}

DEFINE_ICSFUNC(int, pthread_detach, mypthread_t thread) {
  DbgPrint("T%u: PthreadDetach on T%lu\n", MyThreadId, thread);

  if (thread >= KLEE_MAX_THREADS) {
    DbgPrint("T%u: PthreadDetach: OOB\n", MyThreadId);
    return ESRCH;
  }

  // This HB node serializes thread operations
  // We advance the HB node after detaching
  WaitForHbNodeMustExist();

  thread_data_t *t = &__tsync.threads[thread].data;
  if (!t->allocated) {
    DbgPrint("T%u: PthreadDetach: SRCH\n", MyThreadId);
    AdvanceHbNode();
    return ESRCH;
  }
  if (!t->joinable) {
    DbgPrint("T%u: PthreadDetach: INVAL (not joinable)\n", MyThreadId);
    AdvanceHbNode();
    return EINVAL;
  }

  DbgPrint("T%u: PthreadDetach: detaching\n", MyThreadId);
  t->joinable = false;
  if (t->terminated) {
    t->allocated = false;
  }
  AdvanceHbNode();

  return 0;
}

DEFINE_ICSFUNC(int, pthread_once, pthread_once_t *onceControl, void (*initRoutine)(void)) {
  // This HB node serializes the onceControl
  WaitForHbNodeMustExist();
  if (*onceControl == 0) {
    initRoutine();
    *onceControl = 1;
  }
  AdvanceHbNode();

  return 0;
}

DEFINE_ICSFUNC(int, pthread_equal, mypthread_t thread1, mypthread_t thread2) {
  return thread1 == thread2;
}

// TODO: should these be nops?
DEFINE_ICSFUNC(int, pthread_yield, void) {
  __ics_call_real_sched_yield();
  return 0;
}

DEFINE_ICSFUNC(int, sched_yield, void) {
  __ics_call_real_sched_yield();
  return 0;
}

// Nops
DEFINE_ICSFUNC(int, pthread_attr_init, pthread_attr_t *attr) {
  return 0;
}

DEFINE_ICSFUNC(int, pthread_attr_destroy, pthread_attr_t *attr) {
  return 0;
}

DEFINE_ICSFUNC(void, pthread_initialize, void) {}
DEFINE_ICSFUNC(void, __pthread_initialize, void) {}
DEFINE_ICSFUNC(void, __pthread_initialize_minimal, void) {}

// Cleanup push/pop
DEFINE_ICSFUNC(void, _pthread_cleanup_push, _pthread_cleanup_buffer *buffer, void (*routine)(void*), void *arg) {
  buffer->__routine = routine;
  buffer->__arg = arg;
  buffer->__prev = MyThreadInfo->cleanup_stack;
  MyThreadInfo->cleanup_stack = buffer;
}

DEFINE_ICSFUNC(void, _pthread_cleanup_pop, _pthread_cleanup_buffer *buffer, int execute) {
  if (execute) {
    buffer->__routine(buffer->__arg);
  }
  MyThreadInfo->cleanup_stack = buffer->__prev;
}

DEFINE_ICSFUNC(void, _pthread_cleanup_push_defer, _pthread_cleanup_buffer *buffer, void (*routine)(void*), void *arg) {
  CALL_ICSFUNC(_pthread_cleanup_push, buffer, routine, arg);
}

DEFINE_ICSFUNC(void, _pthread_cleanup_pop_restore, _pthread_cleanup_buffer *buffer, int execute) {
  CALL_ICSFUNC(_pthread_cleanup_pop, buffer, execute);
}

// Unsupported
DEFINE_ICSFUNC(int, pthread_setcancelstate, int state, int *oldstate) {
  return 0;
}

DEFINE_ICSFUNC(int, pthread_setcanceltype, int type, int *oldtype) {
  return 0;
}

DEFINE_ICSFUNC(int, pthread_cancel, mypthread_t thread) {
  assert(0 && "not implemented");
}

DEFINE_ICSFUNC(int, pthread_testcancel, void) {
  assert(0 && "not implemented");
}

DEFINE_ICSFUNC(int, pthread_key_create, pthread_key_t *key, void (*dtor)(void*)) {
  assert(0 && "not implemented");
  return -1;
}

DEFINE_ICSFUNC(int, pthread_key_delete, pthread_key_t key) {
  assert(0 && "not implemented");
  return -1;
}

DEFINE_ICSFUNC(void*, pthread_getspecific, pthread_key_t key) {
  assert(0 && "not implemented");
  return NULL;
}

DEFINE_ICSFUNC(int, pthread_setspecific, pthread_key_t key, const void *ptr) {
  assert(0 && "not implemented");
  return -1;
}

//-----------------------------------------------------------------------------
// Pthreads Mutexes: Uclibc wrappers
// N.B.: There is an assumption here that mutexes are either always used via
// these wrappers or never used via these wrappers, so we don't need to worry
// about interactions with the standard HBNode-based implementation.
//-----------------------------------------------------------------------------

static int spinlock_lock(pthread_mutex_t *mutex, bool istry) {
  bool success = __sync_lock_test_and_set(&mutex->taken, 1);
  if (!success) {
    // Recursive?
    // N.B.: uclibc initializes locks to be recursive, which we currently
    // do not support in the symbolic models.  I'm not sure they actually
    // *need* recursion support, but just in case, we use the "queued" field
    // as a "count" to support recursion in this spinlock.
    if (mutex->owner == MyThreadId) {
      mutex->queued++;
      return 0;
    }

    // Somebody else holds it
    if (istry)
      return EBUSY;

    SPIN_WAIT_UNTIL(__sync_lock_test_and_set(&mutex->taken, 1));
  }

  // Success
  mutex->owner = MyThreadId;
  mutex->queued = 1;

  return 0;
}

static int spinlock_unlock(pthread_mutex_t *mutex) {
  // N.B.: Technically the first condition here would imply a data race,
  // but, since we assume data race freedom, we'll ignore that possibility.
  if (!mutex->taken || mutex->owner != MyThreadId)
    return EPERM;

  // Recursive?
  // See comments in spinlock_lock() about recursion
  if (mutex->queued > 0) {
    mutex->queued--;
    if (mutex->queued != 0)
      return 0;
  }

  // Done with the lock
  __sync_lock_release(&mutex->taken);
  return 0;
}

DEFINE_ICSFUNC(int, __uClibc_wrap_pthread_mutex_lock, pthread_mutex_t *mutex) {
  DbgPrint("T%u: UclibcWrapPthreadMutexLock on %p\n", MyThreadId, mutex);
  return spinlock_lock(mutex, false);
}
DEFINE_ICSFUNC(int, __uClibc_wrap_pthread_mutex_trylock, pthread_mutex_t *mutex) {
  DbgPrint("T%u: UclibcWrapPthreadMutexTryock on %p\n", MyThreadId, mutex);
  return spinlock_lock(mutex, true);
}
DEFINE_ICSFUNC(int, __uClibc_wrap_pthread_mutex_unlock, pthread_mutex_t *mutex) {
  DbgPrint("T%u: UclibcWrapPthreadMutexUnlock on %p\n", MyThreadId, mutex);
  return spinlock_unlock(mutex);
}

//-----------------------------------------------------------------------------
// Pthreads Mutexes
//-----------------------------------------------------------------------------

// Attrs
DEFINE_ICSFUNC(int, pthread_mutexattr_init, pthread_mutexattr_t *attr) {
  return 0;
}

DEFINE_ICSFUNC(int, pthread_mutexattr_destroy, pthread_mutexattr_t *attr) {
  return 0;
}

DEFINE_ICSFUNC(int, pthread_mutexattr_setpshared, pthread_mutexattr_t *attr, int pshared) {
  return 0;
}

DEFINE_ICSFUNC(int, pthread_mutexattr_settype, pthread_mutexattr_t *attr, int type) {
  return 0;
}

DEFINE_ICSFUNC(int, pthread_mutexattr_getpshared, pthread_mutexattr_t *attr, int *pshared) {
  *pshared = PTHREAD_PROCESS_PRIVATE;
  return 0;
}

DEFINE_ICSFUNC(int, pthread_mutexattr_gettype, pthread_mutexattr_t *attr, int *type) {
  *type = PTHREAD_MUTEX_NORMAL;
  return 0;
}

// Sync
DEFINE_ICSFUNC(int, pthread_mutex_init, pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
  klee_memset(mutex, 0, sizeof *mutex);
  return 0;
}

DEFINE_ICSFUNC(int, pthread_mutex_destroy, pthread_mutex_t *mutex) {
  return 0;
}

static inline int dolock(pthread_mutex_t *mutex, bool istry) {
  // Each lock() call has at least one HB node
  WaitForHbNodeMustExist();

  // Already acquired?
  if (mutex->taken || mutex->queued > 0) {
    if (istry) {
      AdvanceHbNode();
      return EBUSY;
    }
    // Sleep: when we wake, the lock is ours
    mutex->queued++;
    AdvanceHbNode();
    TakeNextWaitSlot();
    DoOneSyncOpMustExistCanSleep();
  } else {
    AdvanceHbNode();
  }

  // Now it's our turn
  mutex->taken = true;
  mutex->owner = MyThreadId;

  return 0;
}

static inline int dounlock(pthread_mutex_t *mutex) {
  // N.B.: Technically the first condition here would imply a data race,
  // but, since we assume data race freedom, we'll ignore that possibility.
  if (!mutex->taken || mutex->owner != MyThreadId)
    return EPERM;

  // Done with the lock
  mutex->taken = false;
  if (mutex->queued > 0)
    mutex->queued--;

  DoOneSyncOpMustExist();
  return 0;
}

DEFINE_ICSFUNC(int, pthread_mutex_lock, pthread_mutex_t *mutex) {
  DbgPrint("T%u: PthreadMutexLock on %p\n", MyThreadId, mutex);
  return dolock(mutex, false);
}

DEFINE_ICSFUNC(int, pthread_mutex_trylock, pthread_mutex_t *mutex) {
  DbgPrint("T%u: PthreadMutexLock on %p\n", MyThreadId, mutex);
  return dolock(mutex, true);
}

DEFINE_ICSFUNC(int, pthread_mutex_unlock, pthread_mutex_t *mutex) {
  DbgPrint("T%u: PthreadMutexUnlock on %p\n", MyThreadId, mutex);
  return dounlock(mutex);
}

//-----------------------------------------------------------------------------
// Pthreads Condition Variables
//-----------------------------------------------------------------------------

DEFINE_ICSFUNC(int, pthread_cond_init, pthread_cond_t *cond, const pthread_condattr_t *attr) {
  klee_memset(cond, 0, sizeof *cond);
  return 0;
}

DEFINE_ICSFUNC(int, pthread_cond_destroy, pthread_cond_t *cond) {
  return 0;
}

DEFINE_ICSFUNC(int, pthread_cond_timedwait, pthread_cond_t *cond, pthread_mutex_t *mutex,
                                 const struct timespec *abstime) {
  assert(0 && "not implemented");
  return -1;
}

DEFINE_ICSFUNC(int, pthread_cond_wait, pthread_cond_t *cond, pthread_mutex_t *mutex) {
  // N.B.: Since we assume data race freedom, we can use the given mutex
  // to guard accesses to fields of *cond -- if the caller does not supply
  // a properly locked mutex, or if multiple mutexes are used with the
  // same cond, there are potential data races in the code below (and, again,
  // we assume no data races so we ignore this possibility).
  DbgPrint("T%u: PthreadCondWait on %p,%p\n", MyThreadId, cond, mutex);

  // Queue this thread
  if (cond->queued > 0) {
    assert(cond->mutex == mutex);
  } else {
    cond->mutex = mutex;
  }

  cond->queued++;

  if (dounlock(mutex) != 0) {
    DbgPrint("T%u: PthreadCondWait: EPERM on unlock\n", MyThreadId);
    cond->queued--;
    return EPERM;
  }

  // Wait to be woken
  TakeNextWaitSlot();
  DoOneSyncOpMustExistCanSleep();

  // Re-lock
  if (dolock(mutex, false) != 0) {
    DbgPrint("T%u: PthreadCondWait: EPERM on relock\n", MyThreadId);
    return EPERM;
  }

  DbgPrint("T%u: PthreadCondWait DONE\n", MyThreadId);
  return 0;
}

DEFINE_ICSFUNC(int, pthread_cond_broadcast, pthread_cond_t *cond) {
  DbgPrint("T%u: PthreadCondBroadcast @ %p\n", MyThreadId, cond);
  WaitForHbNodeMustExist();
  cond->queued = 0;
  AdvanceHbNode();
  return 0;
}

DEFINE_ICSFUNC(int, pthread_cond_signal, pthread_cond_t *cond) {
  DbgPrint("T%u: PthreadCondSignal @ %p\n", MyThreadId, cond);
  WaitForHbNodeMustExist();
  if (cond->queued > 0)
    cond->queued--;
  AdvanceHbNode();
  return 0;
}

//-----------------------------------------------------------------------------
// Pthreads Barriers
//-----------------------------------------------------------------------------

DEFINE_ICSFUNC(int, pthread_barrier_init, pthread_barrier_t *barrier, const pthread_barrierattr_t *attr,
                               unsigned int count) {
  barrier->init_count = count;
  barrier->left = count;
  return 0;
}

DEFINE_ICSFUNC(int, pthread_barrier_destroy, pthread_barrier_t *barrier) {
  return 0;
}

DEFINE_ICSFUNC(int, pthread_barrier_wait, pthread_barrier_t *barrier) {
  if (barrier->init_count == 0)
    return EINVAL;

  // Need to update this since the symbolic model code branches on this value
  // TODO: do we *actually* need to update this?  the model code does branch
  // on this value, but we implement that branch differently, below
  __sync_fetch_and_sub(&barrier->left, 1);

  // The HB graph at a barrier looks like this:
  //   {otherthreads} -> serialthread -> {otherthreads}
  // That is, the next HB node we see should either:
  //   (a) have no incoming edges (this is the "other" thread case), or
  //   (b) have (barrier->init_count-1) incoming edges (this is the "serial" thread case)
  const uint16_t *numIncoming = MyThreadSchedulePtr + 1;
  int result = 0;

  // Wait for the barrer
  // Don't need to update wait_slot since barrier threads are woken via notifyAll
  WaitForHbNodeMustExistCanSleep();
  if (*numIncoming == barrier->init_count-1) {
    DbgPrint("T%u: PthreadBarrierWait on %p: serial thread (%d)\n", MyThreadId, barrier, *numIncoming);
    result = PTHREAD_BARRIER_SERIAL_THREAD;
    barrier->left = barrier->init_count;
    AdvanceHbNode();
  } else {
    DbgPrint("T%u: PthreadBarrierWait on %p: waiter thread (%d)\n", MyThreadId, barrier, *numIncoming);
    assert(*numIncoming == 0);
    AdvanceHbNode();
    DoOneSyncOpMustExistCanSleep();
  }

  // There might be a region barrier here
  SkipRegionBarriersOrExit();

  return result;
}

//-----------------------------------------------------------------------------
// Pthreads Read Write Locks
// TODO: need to reimplement
//-----------------------------------------------------------------------------

// XXX XXX XXX
#define DoAllSyncOps()

// Nops
DEFINE_ICSFUNC(int, pthread_rwlock_init, pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr) {
  return 0;
}

DEFINE_ICSFUNC(int, pthread_rwlock_destroy, pthread_rwlock_t *rwlock) {
  return 0;
}

// Sync
// TODO: actually need to model the rwlock enough to determine the error codes?
DEFINE_ICSFUNC(int, pthread_rwlock_rdlock, pthread_rwlock_t *rwlock) {
  DoAllSyncOps();
  return 0;
}

DEFINE_ICSFUNC(int, pthread_rwlock_tryrdlock, pthread_rwlock_t *rwlock) {
  DoAllSyncOps();
  return 0;
}

DEFINE_ICSFUNC(int, pthread_rwlock_wrlock, pthread_rwlock_t *rwlock) {
  DoAllSyncOps();
  return 0;
}

DEFINE_ICSFUNC(int, pthread_rwlock_trywrlock, pthread_rwlock_t *rwlock) {
  DoAllSyncOps();
  return 0;
}

DEFINE_ICSFUNC(int, pthread_rwlock_unlock, pthread_rwlock_t *rwlock) {
  DoAllSyncOps();
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
// POSIX Semaphores
////////////////////////////////////////////////////////////////////////////////

__attribute__((always_inline))
static int dodown(sem_t *sem, char istry) {
  // Each dec() call has at least one HB node
  WaitForHbNodeMustExist();

  // Resource exhausted?
  while (sem->count == 0) {
    if (istry) {
      AdvanceHbNode();
      errno = EAGAIN;
      return -1;
    }

    // Sleep: when we wake, we must RECHECK since
    // semaphores are (currently) not FIFO
    AdvanceHbNode();
    TakeNextWaitSlot();
    WaitForHbNodeMustExistCanSleep();
  }

  sem->count--;
  AdvanceHbNode();

  return 0;
}

__attribute__((always_inline))
static int doup(sem_t *sem) {
  WaitForHbNodeMustExist();
  sem->count++;
  AdvanceHbNode();
  return 0;
}

DEFINE_ICSFUNC(int, sem_init, sem_t *sem, int pshared, unsigned int value) {
  if(value > SEM_VALUE_MAX) {
    errno = EINVAL;
    return -1;
  }

  sem->wlist = klee_get_wlist();
  sem->last_hbnode = 0;
  sem->count = value;
  return 0;
}

DEFINE_ICSFUNC(int, sem_destroy, sem_t *sem) {
  return 0;
}

DEFINE_ICSFUNC(int, sem_wait, sem_t *sem) {
  DbgPrint("T%u: SemWait on %p\n", MyThreadId, sem);
  return dodown(sem, 0);
}

DEFINE_ICSFUNC(int, sem_trywait, sem_t *sem) {
  DbgPrint("T%u: SemTryWait on %p\n", MyThreadId, sem);
  return dodown(sem, 1);
}

DEFINE_ICSFUNC(int, sem_post, sem_t *sem) {
  DbgPrint("T%u: SemPost on %p\n", MyThreadId, sem);
  return doup(sem);
}

}  // extern "C"
