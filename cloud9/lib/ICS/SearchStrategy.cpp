/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * A search strategy for computing input-covering schedules.
 *
 */

#include "ics/SearchStrategy.h"
#include "ics/Instrumentor.h"
#include "AARunner.h"
#include "AACache.h"

#include "../Core/Common.h"
#include "../Core/Memory.h"
#include "../Core/TimingSolver.h"

#include "klee/Context.h"
#include "klee/Executor.h"
#include "klee/ExecutionState.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Support/Timer.h"
#include "klee/util/StateUtil.h"
#include "cloud9/worker/JobManager.h"
#include "cloud9/worker/TreeObjects.h"

#include "dsa/DataStructureAA.h"

#include "llvm/Argument.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/PassManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/DebugInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/raw_ostream.h"

#include "LocalLlvmUtils.h"

#include <queue>
#include <memory>
#include <sstream>
#include <tr1/unordered_map>

char ics::AARunner::DoNothing::ID;

using cloud9::worker::ControlFlowEvent;
using cloud9::worker::ExecutionJob;
using cloud9::worker::JobManager;
using cloud9::worker::SymbolicState;
using cloud9::worker::WorkerTree;
using cloud9::worker::WorkerNodeInfo;
using klee::ref;
using klee::Expr;

using namespace llvm;

// XXX Need to assert that these implied-value-concretization opts are not set:
// XXX SimplifyAfterLoad
// XXX SimplifyBeforeStore

namespace ics {

//-----------------------------------------------------------------------------
// Command-line options
//-----------------------------------------------------------------------------

enum IcsExecModeTy {
  // In normal execution, we execute all paths in a region until full coverage
  // is achieved, then execute the next region, and so on, up to the "limits"
  // defined below (MaxRegions, MaxPathsPerRegion, etc.).
  IcsNormal = 0,
  // These are experiments to evaluate execution from the middle of a program.
  // In each case, we start by executing a single path from program entry up to
  // the second region, wherever it happens to fall.  We then execute all paths
  // in the second region until full coverage is acheived, and then halt.  The
  // difference in each mode is the way the initial state for the second region
  // is computed:
  //
  // * ViaConcretePath
  //     - we use the concrete state reached by the initial path from program entry
  // * ViaDataflowAnalysis
  //     - we compute an initial state via the dataflow analysis, as done in IcsNormal
  // * Unconstrained
  //     - the initial state is an unconstrained over-approximation
  //
  // The MaxRegions limit does not apply in any of these modes (it is effectively
  // 2).  The MaxPathsPerRegion and Max*DepthPerPath limits apply to the second region
  // only (the first region has one path of an unlimited depth).
  IcsExecFromMiddleViaConcretePath,
  IcsExecFromMiddleViaDataflowAnalysis,
  IcsExecFromMiddleUnconstrained
};

cl::opt<IcsExecModeTy>
IcsExecMode("ics-exec-mode",
            cl::values(clEnumValN(IcsNormal, "Normal", ""),
                       clEnumValN(IcsExecFromMiddleViaConcretePath, "MiddleViaConcretePath", ""),
                       clEnumValN(IcsExecFromMiddleViaDataflowAnalysis, "MiddleViaDataflow", ""),
                       clEnumValN(IcsExecFromMiddleUnconstrained, "MiddleUnconstrained", ""),
                       clEnumValEnd),
            cl::init(IcsNormal));

static bool IsIcsModeExecFromMiddle() {
  switch (IcsExecMode) {
  case IcsExecFromMiddleViaConcretePath:
  case IcsExecFromMiddleViaDataflowAnalysis:
  case IcsExecFromMiddleUnconstrained:
    return true;
  default:
    return false;
  }
}

// If set, then we will perform a context-switch after each branch.
// This attempts to avoiding switching within synchronization functions
// to preserve atomicity requirements of those functions.
cl::opt<bool> EnableCtxSwitchOnBranches("enable-ctx-switch-on-branches", cl::init(false));

//
// Features
//

// If set, we perform automatic region formation both statically, by injecting region
// markers, and dynamically, by halting the region at certain functions (e.g., at
// certain calls to klee_thread_notify).
cl::opt<bool> DoRegionFormation("ics-do-region-formation", cl::init(true));

// If set, we use precondition slicing to compute a synchronization-preserving slice.
// This slice defines the set of branches to next explore.  If not set, we explore *all*
// branches, including those that do not affect synchronization.
cl::opt<bool> DoPrecondSlice("ics-do-precond-slice", cl::init(true));

// If set, we use a dataflow analysis to construct the initial symbolic state for each
// region.  If this is not set, we construct just as for IcsExecFromMiddleUnconstrained,
// but this option works for the IcsNormal exec mode.
cl::opt<bool> DoRegionStateViaDataflow("ics-do-region-state-via-dataflow", cl::init(true));

// If set, we instrument the code for constrained execution.
// This is implicitly false when IcsExecMode != IcsNormal.
cl::opt<bool> DoInstrumentor("ics-do-instrumentor", cl::init(true));

//
// Search Limits
// A value of "0" means "unlimited"
// The precise meaning of each limit depends on the above execution mode
//

cl::opt<int> MaxRegions("ics-max-regions", cl::init(0));
cl::opt<int> MaxPathsPerRegion("ics-max-paths-per-region", cl::init(0));
cl::opt<int> MaxConcreteDepthPerPath("ics-max-concrete-depth-per-path", cl::init(0));
cl::opt<int> MaxSymbolicDepthPerPath("ics-max-symbolic-depth-per-path", cl::init(0));

static size_t GetEffectiveMaxRegions(void) {
  if (IsIcsModeExecFromMiddle()) {
    return 2;
  } else {
    return MaxRegions;
  }
}

static size_t GetMaxPathsForCurrentRegion(const size_t numPriorCompletedRegions) {
  if (IsIcsModeExecFromMiddle() && numPriorCompletedRegions == 0) {
    return 1;  // first region has 1 path
  } else {
    return MaxPathsPerRegion;
  }
}

static size_t GetMaxConcretePathDepthForCurrentRegion(const size_t numPriorCompletedRegions) {
  if (IsIcsModeExecFromMiddle() && numPriorCompletedRegions == 0) {
    return 0;  // first region has unlimited depth
  } else {
    return MaxConcreteDepthPerPath;
  }
}

static size_t GetMaxSymbolicPathDepthForCurrentRegion(const size_t numPriorCompletedRegions) {
  if (IsIcsModeExecFromMiddle() && numPriorCompletedRegions == 0) {
    return 0;  // first region has unlimited depth
  } else {
    return MaxSymbolicDepthPerPath;
  }
}

//
// Region formation settings
// These are meaningless unless "DoRegionFormation"=true
//

cl::opt<unsigned> MinTimeBetweenRegionMarkers("ics-min-time-between-region-markers", cl::init(0));
cl::opt<bool> DoLazyRegionCovering("ics-do-lazy-region-covering", cl::init(false));
cl::opt<bool> OptRegionsCoverLoopOnce("ics-opt-regions-cover-loop-once", cl::init(false));
cl::opt<bool> OptRegionsIgnoreTrivialLoops("ics-opt-regions-ignore-trivial-loops", cl::init(false));
cl::opt<bool> OptAssumeRegionsEndInBarriers("ics-opt-assume-regions-end-in-barriers", cl::init(false));
cl::opt<bool> OptRunRegionsWhileAcyclic("ics-opt-run-regions-while-acyclic", cl::init(false));

//
// Optimizations
//

cl::opt<bool> OptSkipEarlyExitBranches("ics-opt-skip-early-exit-branches", cl::init(false));
cl::opt<bool> OptShortestPathFirst("ics-opt-shortest-path-first", cl::init(false));
cl::opt<bool> OptNoSingleThreadedScheduling("ics-opt-no-single-threaded-scheduling", cl::init(false));
cl::opt<bool> OptNoConcretizationConstraintsInSlice("ics-opt-no-concretization-constraints-in-slice", cl::init(false));
cl::opt<bool> OptIgnoreUclibcSync("ics-opt-ignore-uclibc-sync", cl::init(false));

//
// Debugging
//

// Options to write more output
cl::opt<bool> DumpExecutionTraces("ics-dump-execution-traces", cl::init(false));
cl::opt<bool> DumpSliceTraces("ics-dump-slice-traces", cl::init(false));
cl::opt<bool> DumpSliceConstraints("ics-dump-slice-constraints", cl::init(false));
cl::opt<bool> DumpHbGraphs("ics-dump-hbgraphs", cl::init(false));
cl::opt<bool> DumpSymbolicTrees("ics-dump-symbolic-trees", cl::init(false));
cl::opt<bool> DumpPrecondSlicingStats("ics-dump-precond-slicing-stats", cl::init(false));
cl::opt<bool> DumpRegionMarkerStats("ics-dump-region-marker-stats", cl::init(false));
cl::opt<bool> DumpRegionMarkerCfgs("ics-dump-region-marker-cfgs", cl::init(false));
cl::opt<bool> DumpRegionSymbolicObjects("ics-dump-region-symbolic-objects", cl::init(false));
cl::opt<bool> DumpSymbolicPtrGraphs("ics-dump-symbolic-ptr-graphs", cl::init(false));
cl::opt<bool> DumpOptimizationStats("ics-dump-optimization-stats", cl::init(false));

// Options to print more verbose messages
cl::opt<bool> DbgSymbolicPaths("ics-debug-symbolic-paths", cl::init(false));
cl::opt<bool> DbgSymbolicPathsVerbose("ics-debug-symbolic-paths-verbose", cl::init(false));
cl::opt<bool> DbgSyncOpCallers("ics-debug-sync-op-callers", cl::init(false));
cl::opt<bool> DbgMayHappenBAThreadCreate("ics-debug-may-happen-ba-thread-create", cl::init(false));
cl::opt<bool> DbgConstraints("ics-debug-constraints", cl::init(false));
cl::opt<bool> DbgCtxSwitchEvents("ics-debug-ctxswitch-events", cl::init(false));
cl::opt<bool> DbgPrecondSlicing("ics-debug-precond-slicing", cl::init(false));
cl::opt<bool> DbgPrecondSlicingVerbose("ics-debug-precond-slicing-verbose", cl::init(false));
cl::opt<bool> DbgSymbolicPtrSlicing("ics-debug-symbolic-ptr-slicing", cl::init(false));
cl::opt<bool> DbgRegionMarkers("ics-debug-region-markers", cl::init(false));
cl::opt<bool> DbgSchedulingOptimizations("ics-debug-scheduling-optimizations", cl::init(false));

// Options to take certain code paths meant for debugging
cl::opt<bool> DbgIgnoreUncoveredLoops("ics-debug-ignore-uncovered-loops", cl::init(false));

//-----------------------------------------------------------------------------
// Globals
//-----------------------------------------------------------------------------

namespace {

//
// Defined in this file
//

void ComputePrecondSlice(klee::Executor *executor,
                         klee::ExecutionState *finalState,
                         DepthFirstICS *strat,
                         PrecondSliceTrace *slice);

bool TerminateStateIfSingleThreaded(klee::ExecutionState *kstate,
                                    klee::KInstruction *targetKinst);

void InitializeStateForParallelRegion(klee::ExecutionState *kstate);

//
// Special functions
//

llvm::Function* KleeAssumeFn;
llvm::Function* KleeThreadCreateFn;
llvm::Function* KleeThreadNotifyFn;
llvm::Function* KleeThreadSleepFn;
llvm::Function* KleeThreadTerminateFn;
llvm::Function* KleeProcessTerminateFn;
llvm::Function* KleeAssertFailFn;
llvm::Function* KleeIcsRegionMarkerFn;

llvm::Function* PosixPthreadBarrierWait;
llvm::Function* PosixPthreadCondWait;
llvm::Function* PosixPthreadCreate;
llvm::Function* PosixPthreadJoin;
llvm::Function* PosixPthreadExit;

// Reflexive-transitive closure of the bottom-up callgraph over SyncOpFunctions
llvm::DenseSet<const llvm::Function*> SyncOpCallers;

// These are not included in SyncOpCallers if OptIgnoreUclibcSync
llvm::DenseSet<const llvm::Function*> UclibcSyncOpWrappers;
const char* UclibcSyncOpWrapperNames[] = {
  "__uClibc_wrap_pthread_mutex_lock",
  "__uClibc_wrap_pthread_mutex_trylock",
  "__uClibc_wrap_pthread_mutex_unlock",
  NULL
};

// Functions that are sync ops
// These are any of the klee-builtins that may update the HBGraph
llvm::DenseSet<const llvm::Function*> SyncOpFunctions;
const char* SyncOpFunctionNames[] = {
  "klee_add_hbnode",
  "klee_add_hbedge",
  "klee_add_hbnode_to_hbgroup",
  "klee_add_hbedges_from_hbgroup",
  "klee_add_hbedges_to_hbgroup",
  "klee_remove_hbgroup",
  "klee_thread_create",
  "klee_thread_sleep",
  "klee_thread_notify",
  // N.B.: Intentionally NOT including klee_thread_terminate due to the 
  // prefix schedules optimization (we don't want to explore early exits).
  NULL
};

}  // namespace

bool IsSyncOpFunction(const llvm::Function *f) {
  return f && SyncOpFunctions.count(f) > 0;
}

bool IsSyncOpCaller(const llvm::Function *f) {
  return f && SyncOpCallers.count(f) > 0;
}

bool IsIgnorableSyncOpCaller(const llvm::Function *f) {
  return f && OptIgnoreUclibcSync && UclibcSyncOpWrappers.count(f) > 0;
}

namespace {

// A cache mapping llvm::Instruction -> klee::KInstruction.
// SUPER UGLY, but what else are we gonna do ...
typedef llvm::DenseMap<const llvm::Instruction*, klee::KInstruction*> InstToKInstMapTy;
InstToKInstMapTy InstToKInstMap;

//
// Dataflow analyses
//
// These analyses compute facts that are true over ranges of instructions.
// For example, we may ask the question: which instructions may be eventually
// followed by a call to KleeThreadCreateFn?  We use a dataflow analysis to
// compute a set of instructions which satisfy that query.  This information
// is cached in a InstructionRangeCacheTy, which works as follows:
//
//   .. If the query is true for instruction I, then I->parent is in the
//      cache, and cache[I->parent] points a range of instructions that
//      includes I.  This range of instructions [start,end] is inclusive,
//      meaning it includes both start and end, and it is block-local,
//      meaning start and end are in the same BasicBlock.
//   .. If the query is false for I, then either (a) I->parent is not in
//      the cache, or (b) I lies outside the range defined by cache[I->parent].
//
// To improve accuracy, this cache is context-sensitive and thread-local.
// For each function, we compute cache entries under the assumption that,
// first, the function is on the *bottom* of the callstack, and second, that
// there are no other threads.  So, to answer any question using these caches,
// we must consult the cache for *every* thread and for *every* stack frame
// in each thread's callstack.  This is done by CheckInstructionRangeCache().
//

struct InstructionRange {
  const llvm::Instruction *start;
  const llvm::Instruction *end;
  InstructionRange() : start(NULL), end(NULL) {}
  InstructionRange(const llvm::Instruction *s, const llvm::Instruction *e)
    : start(s), end(e) {}
};

typedef llvm::DenseMap<const llvm::BasicBlock*, InstructionRange> InstructionRangeCacheTy;

// Answers the following query: starting from given callstacks, might
// the program eventually make a call to either KleeThreadCreateFn or
// PosixPthreadJoin?  This is used for OptNoSingleThreadedScheduling.
// N.B.: We include PosixPthreadJoin to ensure that we've generated all
// exit->join HBedges before halting execution.
InstructionRangeCacheTy CacheForMayLeadToThreadCreation;

// Answers the following query: may a given callstack occur after a
// program has make a call to KleeThreadCreateFn?  This is used for
// load/store execution optimizations in partial eval.
InstructionRangeCacheTy CacheForMayHappenAfterThreadCreation;

//
// AliasAnalysis
//

AARunner TheAARunner;
AACache TheAACache;
llvm::AliasAnalysis *AA;
llvm::DSAA *DSAA;

//
// PostDominators
// LoopInfo
//

typedef llvm::DominatorTreeBase<llvm::BasicBlock> DominatorTree;
std::tr1::unordered_map<llvm::Function*, DominatorTree*> TheDominators;
std::tr1::unordered_map<llvm::Function*, DominatorTree*> ThePostdominators;

typedef llvm::LoopInfoBase<llvm::BasicBlock, llvm::Loop> LoopInfoTy;
std::tr1::unordered_map<llvm::Function*, LoopInfoTy*> TheLoopInfo;

}  // namespace

DominatorTree* GetDominatorTree(llvm::Function *f) {
  assert(f);

  DominatorTreeBase<BasicBlock>** dt = &TheDominators[f];
  if ((*dt) == NULL) {
    (*dt) = new DominatorTreeBase<BasicBlock>(false /* dominators */);
    (*dt)->recalculate(*f);
  }

  return (*dt);
}

DominatorTree* GetPostdominatorTree(llvm::Function *f) {
  assert(f);

  DominatorTreeBase<BasicBlock>** dt = &ThePostdominators[f];
  if ((*dt) == NULL) {
    (*dt) = new DominatorTreeBase<BasicBlock>(true  /* post-dominators */);
    (*dt)->recalculate(*f);
  }

  return (*dt);
}

LoopInfoTy* GetLoopInfo(llvm::Function *f) {
  assert(f);

  LoopInfoTy **li = &TheLoopInfo[f];
  if ((*li) == NULL) {
    (*li) = new LoopInfoTy;
    (*li)->Calculate(*GetDominatorTree(f));
  }

  return (*li);
}

//-----------------------------------------------------------------------------
// Initialization
//-----------------------------------------------------------------------------

llvm::Function* GetFunctionWithWarning(llvm::Module *module, const char *name, const char *type) {
  llvm::Function *f = module->getFunction(name);
  if (!f)
    std::cerr << "WARNING: couldn't find " << type << " function " << name << std::endl;
  return f;
}

void GetFunctionsWithWarning(llvm::Module *module, const char **names, const char *type,
                             llvm::DenseSet<const llvm::Function*> *set) {
  for (const char **name = names; *name; ++name) {
    llvm::Function *f = GetFunctionWithWarning(module, *name, "sync op");
    if (f)
      set->insert(f);
  }
}

klee::KInstruction* LookupKInstruction(const klee::ExecutionState *kstate,
                                       const llvm::Instruction *inst) {
  assert(inst);
  InstToKInstMapTy::iterator it = InstToKInstMap.find(inst);
  if (it != InstToKInstMap.end())
    return it->second;

  llvm::Function *f = const_cast<llvm::Function*>(inst->getParent()->getParent());
  const klee::KModule *kmodule = kstate->executor->getKModule();
  std::map<const llvm::Function*,klee::KFunction*>::const_iterator kit;
  kit = kmodule->functionMap.find(f);
  assert(kit != kmodule->functionMap.end());

  klee::KFunction *kf = kit->second;
  for (unsigned i = 0; i < kf->numInstructions; ++i) {
    if (kf->instructions[i]->inst == inst) {
      klee::KInstruction *ki = kf->instructions[i];
      InstToKInstMap[inst] = ki;
      return ki;
    }
  }

  assert(0);
  return NULL;
}

//
// Debugging
//

template<class SetTy>
void PrintSortedFuncSet(const SetTy &unsorted, const char *name, const char *sep = "\n") {
  std::set<std::string> funcs;
  for (typename SetTy::const_iterator
       it = unsorted.begin(); it != unsorted.end(); ++it) {
    funcs.insert((*it) ? (*it)->getNameStr() : "<null>");
  }
  std::cerr << name << ": [" << sep;
  for (std::set<std::string>::iterator it = funcs.begin(); it != funcs.end(); ++it)
    std::cerr << "  " << (*it) << sep;
  std::cerr << "]\n";
}

void DumpInstruction(const char *prefix, const llvm::Instruction *inst) {
  if (!inst) {
    llvm::errs() << prefix << " <null inst>";
  } else {
    llvm::errs() << prefix << " " << inst << " " << (*inst);
  }
}

void DumpValue(const char *prefix, const llvm::Value *val) {
  if (!val) {
    llvm::errs() << prefix << " <null value>";
  } else {
    llvm::errs() << prefix << " " << val << " " << (*val);
  }
}

void DumpKInstruction(const char *prefix, klee::KInstruction *kinst) {
  DumpKInstruction(prefix, kinst, llvm::errs());
}

void DumpKInstruction(const char *prefix, klee::KInstruction *kinst,
                      llvm::raw_ostream &out, bool lineNoOnly) {
  if (!kinst) {
    out << prefix << " <null kinst>";
    return;
  }
  if (kinst->info) {
    out << prefix << " " << kinst->info->file.substr(kinst->info->file.find_last_of('/') + 1)
                         << ":" << kinst->info->line
                         << ":" << kinst->inst->getParent()->getParent()->getName()
                  << "  (" << kinst->inst << ")";
  } else {
    out << prefix << " " << "???:" << kinst->inst->getParent()->getParent()->getName()
                  << "  (" << kinst->inst << ")";
  }
  if (!lineNoOnly) {
    out << prefix << " " << (*kinst->inst);
  }
}

void DumpTuid(const char *prefix, klee::ThreadUid tuid) {
  llvm::errs() << prefix << " " << tuid;
}

std::string KleeForkClassToString(klee::ForkClass fc) {
#define ENUM_TO_STRING(X) case klee::X: return #X;
  switch (fc) {
    ENUM_TO_STRING(KLEE_FORK_BRANCH)
    ENUM_TO_STRING(KLEE_FORK_FAULTINJ)
    ENUM_TO_STRING(KLEE_FORK_RESOLVE_FN_POINTER)
    ENUM_TO_STRING(KLEE_FORK_SCHEDULE)
    ENUM_TO_STRING(KLEE_FORK_SCHEDULE_MULTI)
    ENUM_TO_STRING(KLEE_FORK_INTERNAL)
    ENUM_TO_STRING(KLEE_FORK_ASSUME)
    ENUM_TO_STRING(KLEE_FORK_CONVERTED_BRANCH)
    ENUM_TO_STRING(KLEE_FORK_INVARIANT_INTERNAL)
    ENUM_TO_STRING(KLEE_FORK_INVARIANT_INTERNAL_HIDDEN)
    ENUM_TO_STRING(KLEE_FORK_INVARIANT_INPUT)
    ENUM_TO_STRING(KLEE_FORK_CONCRETIZE_FP)
    ENUM_TO_STRING(KLEE_FORK_CONCRETIZE_ADDR_ON_SOLVER_FAILURE)
    ENUM_TO_STRING(KLEE_FORK_CONCRETIZE_ADDR_ON_BIG_SYMBOLIC_ARRAY)
    ENUM_TO_STRING(KLEE_FORK_POINTER_ALIASING)
    ENUM_TO_STRING(KLEE_FORK_BOUNDS_CHECK)
    default: {
      std::ostringstream str;
      str << "???:" << fc;
      return str.str();
    }
  }
#undef ENUM_TO_STRING
}

//
// Misc static analysis
//

bool LooksLikeMemsetLoop(Instruction *inst) {
  // Return true if this instruction looks like part of a memset loop
  llvm::StoreInst *store = dyn_cast<StoreInst>(inst);
  if (!store)
    return false;

  // Right now, only looking for memset(dst, 0, size)
  llvm::ConstantInt *v = dyn_cast<ConstantInt>(store->getValueOperand());
  if (!v || v->getType()->getBitWidth() != 8 || !v->isZero())
    return false;

  // Should have no subloops
  LoopInfoTy* LI = GetLoopInfo(inst->getParent()->getParent());
  llvm::Loop *loop = LI->getLoopFor(inst->getParent());
  if (!loop || !loop->empty())
    return false;

  // Should have just one store
  // Should have no loads or calls
  for (llvm::Loop::block_iterator bb = loop->block_begin(); bb != loop->block_end(); ++bb) {
    for (llvm::BasicBlock::iterator i = (*bb)->begin(); i != (*bb)->end(); ++i) {
      if (isa<StoreInst>(i) && (&*i) != store)
        return false;
      if (isa<LoadInst>(i) || isa<CallInst>(i))
        return false;
    }
  }

  return true;
}

bool IsLoopLatch(Loop *loop, BasicBlock *bb) {
  for (succ_iterator succ = succ_begin(bb); succ != succ_end(bb); ++succ) {
    if (*succ == loop->getHeader())
      return true;
  }
  return false;
}

//
// Call graph analysis
//

void BottomUpCallGraphClosure(const llvm::DSAA::CallerMapTy &BottomUpCG,
                              const llvm::DenseSet<const llvm::Function*> &seeds,
                              const llvm::DenseSet<const llvm::Function*> &ignore,
                              llvm::DenseSet<const llvm::Function*> *closure) {
  typedef llvm::DSAA::CallerMapTy CallerMapTy;
  typedef llvm::DenseSet<const llvm::Function*> FuncSetTy;

  std::stack<const llvm::Function*> stack;
  assert(closure);
  closure->clear();

  // Seed the stack
  for (FuncSetTy::const_iterator F = seeds.begin(); F != seeds.end(); ++F) {
    if (*F) stack.push(*F);
  }

  // DFS up the callgraph
  while (!stack.empty()) {
    const llvm::Function *F = stack.top();
    stack.pop();
    if (ignore.count(F))
      continue;

    assert(F);
    closure->insert(F);

    // Go up the callgraph
    CallerMapTy::const_iterator CI = BottomUpCG.find(F);
    if (CI == BottomUpCG.end())
      continue;
    for (CallerMapTy::mapped_type::const_iterator
         Caller = CI->second.begin(); Caller != CI->second.end(); ++Caller) {
      if (!closure->count(*Caller))
        stack.push(*Caller);
    }
  }
}

void PrecomputeSyncOpCallers(llvm::Module *module, const llvm::DSAA::CallerMapTy &BottomUpCG) {
  llvm::DenseSet<const llvm::Function*> seeds;
  llvm::DenseSet<const llvm::Function*> ignore;

  seeds = SyncOpFunctions;

  // Ignore all sync ops in uclibc functions, if requested.
  if (OptIgnoreUclibcSync) {
    ignore.insert(UclibcSyncOpWrappers.begin(), UclibcSyncOpWrappers.end());
  }

  // Compute
  BottomUpCallGraphClosure(BottomUpCG, seeds, ignore, &SyncOpCallers);
}

// N.B.: PrecomputeCacheForMayLeadToCall and PrecomputeCacheForHappenAfterCall
// are excatly the same, except the former does a backwards CFG traversal, while
// the later does a forwards CFG traversal.  At the moment, I can't think of a
// clean way to share more code.

void PrecomputeCacheForMayLeadToCall(const DSCallGraph &CG,
                                     const llvm::DSAA::CallerMapTy &BottomUpCG,
                                     const llvm::DenseSet<const llvm::Function*> &seeds,
                                     InstructionRangeCacheTy *cache) {
  typedef llvm::DenseSet<const llvm::Function*> FuncSetTy;
  FuncSetTy empty;
  FuncSetTy seedCallers;

  assert(cache);
  cache->clear();

  // Compute set of functions which call one of the seed functions
  BottomUpCallGraphClosure(BottomUpCG, seeds, empty, &seedCallers);

  // For each such function, walk backwards over the CFG to compute the closure of
  // basic blocks which may-lead-to a CallInst that calls a "seedCaller".
  for (FuncSetTy::iterator f = seedCallers.begin(); f != seedCallers.end(); ++f) {
    std::stack<const llvm::BasicBlock*> stack;
    llvm::DenseSet<const llvm::BasicBlock*> pushed;

    // Special case: f is one of the seeds.
    // We include all blocks of f in the cache (as the cache is inclusive).
    if (seeds.count(*f)) {
      for (llvm::Function::const_iterator bb = (*f)->begin(); bb != (*f)->end(); ++bb)
        (*cache)[bb] = InstructionRange(bb->begin(), bb->getTerminator());
      continue;
    }

    // Enumerate exit nodes
    for (llvm::Function::const_iterator bb = (*f)->begin(); bb != (*f)->end(); ++bb) {
      const llvm::Instruction *i = bb->getTerminator();
      if (isa<UnwindInst>(i) || isa<ReturnInst>(i) || isa<UnreachableInst>(i)) {
        stack.push(bb);
        pushed.insert(bb);
      }
    }

    // DFS backwards
    while (!stack.empty()) {
      const llvm::BasicBlock *bb = stack.top();
      stack.pop();

      // Already in the cache?
      InstructionRange r;
      InstructionRangeCacheTy::iterator c = cache->find(bb);
      if (c != cache->end()) {
        r = c->second;
      } else {
        // Not cached
        r.start = bb->begin();
        r.end = NULL;
        // Search backwards for the first call to a seedCaller
        for (llvm::BasicBlock::InstListType::const_reverse_iterator
             i = bb->getInstList().rbegin(); i != bb->getInstList().rend(); ++i) {
          if (!isa<CallInst>(&*i) && !isa<InvokeInst>(&*i))
            continue;
          CallSite cs(const_cast<llvm::Instruction*>(&*i));
          for (DSCallGraph::call_target_iterator t = CG.call_target_begin(cs),
                                                te = CG.call_target_end(cs); t != te; ++t) {
            if (seedCallers.count(*t)) {
              r.end = &*i;
              (*cache)[bb] = r;
              break;
            }
          }
          if (r.end)
            break;
        }
      }

      // Flood to predecessors
      for (const_pred_iterator pred = pred_begin(bb); pred != pred_end(bb); ++pred) {
        bool done = true;
        if (r.end) {
          InstructionRangeCacheTy::iterator c = cache->find(*pred);
          if (c == cache->end() || c->second.end != (*pred)->getTerminator()) {
            done = false;
            (*cache)[*pred] = InstructionRange((*pred)->begin(), (*pred)->getTerminator());
          }
        }
        if (!done || !pushed.count(*pred)) {
          stack.push(*pred);
          pushed.insert(*pred);
        }
      }
    }
  }
}

void PrecomputeCacheForMayHappenAfterCall(const DSCallGraph &CG,
                                          const llvm::DSAA::CallerMapTy &BottomUpCG,
                                          const llvm::DenseSet<const llvm::Function*> &seeds,
                                          InstructionRangeCacheTy *cache) {
  typedef llvm::DenseSet<const llvm::Function*> FuncSetTy;
  FuncSetTy empty;
  FuncSetTy seedCallers;

  assert(cache);
  cache->clear();

  // Compute set of functions which call one of the seed functions
  BottomUpCallGraphClosure(BottomUpCG, seeds, empty, &seedCallers);

  // For each such function, walk forwards over the CFG to compute the closure of
  // basic blocks which may-happen-after a CallInst that calls a "seedCaller".
  for (FuncSetTy::iterator f = seedCallers.begin(); f != seedCallers.end(); ++f) {
    std::stack<const llvm::BasicBlock*> stack;
    llvm::DenseSet<const llvm::BasicBlock*> pushed;

    // Special case: f is one of the seeds.
    // We include all blocks of f in the cache (as the cache is inclusive).
    if (seeds.count(*f)) {
      for (llvm::Function::const_iterator bb = (*f)->begin(); bb != (*f)->end(); ++bb)
        (*cache)[bb] = InstructionRange(bb->begin(), bb->getTerminator());
      continue;
    }

    // DFS forwards from the entry node
    const llvm::BasicBlock *entry = &(*f)->getEntryBlock();
    stack.push(entry);
    pushed.insert(entry);

    while (!stack.empty()) {
      const llvm::BasicBlock *bb = stack.top();
      stack.pop();

      bool found = false;

      // Already in the cache?
      InstructionRange r;
      InstructionRangeCacheTy::iterator c = cache->find(bb);
      if (c != cache->end()) {
        r = c->second;
        found = true;
      } else {
        // Not cached
        r.start = NULL;
        r.end = bb->getTerminator();
        // Search forwards for the first call to a seedCaller
        for (llvm::BasicBlock::const_iterator i = bb->begin(); i != bb->end(); ++i) {
          if (!isa<CallInst>(&*i) && !isa<InvokeInst>(&*i))
            continue;
          CallSite cs(const_cast<llvm::Instruction*>(&*i));
          for (DSCallGraph::call_target_iterator t = CG.call_target_begin(cs),
                                                te = CG.call_target_end(cs); t != te; ++t) {
            if (seedCallers.count(*t)) {
              found = true;
              ++i;  // becomes true *after* this call
              if (i != bb->end()) {
                r.start = &*i;
                (*cache)[bb] = r;
              }
              break;
            }
          }
          if (found)
            break;
        }
      }

      // Flood to successors
      for (succ_const_iterator succ = succ_begin(bb); succ != succ_end(bb); ++succ) {
        bool done = true;
        if (found) {
          InstructionRangeCacheTy::iterator c = cache->find(*succ);
          if (c == cache->end() || c->second.start != (*succ)->begin()) {
            done = false;
            (*cache)[*succ] = InstructionRange((*succ)->begin(), (*succ)->getTerminator());
          }
        }
        if (!done || !pushed.count(*succ)) {
          stack.push(*succ);
          pushed.insert(*succ);
        }
      }
    }
  }
}

void DumpInstructionRangeCache(const DSCallGraph &CG,
                               const llvm::DSAA::CallerMapTy &BottomUpCG,
                               const llvm::DenseSet<const llvm::Function*> &seeds,
                               const InstructionRangeCacheTy &cache) {
  typedef llvm::DenseSet<const llvm::Function*> FuncSetTy;
  FuncSetTy empty;
  FuncSetTy callers;

  // DEBUG
  BottomUpCallGraphClosure(BottomUpCG, seeds, empty, &callers);
  PrintSortedFuncSet(callers, "Callers");

  for (FuncSetTy::iterator f = callers.begin(); f != callers.end(); ++f) {
    llvm::errs() << (*f)->getNameStr() << ":\n";
    for (llvm::Function::const_iterator bb = (*f)->begin(); bb != (*f)->end(); ++bb) {
       InstructionRangeCacheTy::const_iterator c = cache.find(bb);
       if (c != cache.end()) {
         const llvm::BasicBlock *bb = c->second.start->getParent();
         llvm::errs() << "  bb.start: [" << bb->getName() << "]" << (*c->second.start) << "\n";
         llvm::errs() << "  bb.end:   [" << bb->getName() << "]" << (*c->second.end) << "\n";
       }
    }
  }
}

void PrecomputeDataflowCaches(const DSCallGraph &CG,
                              const llvm::DSAA::CallerMapTy &BottomUpCG) {
  llvm::DenseSet<const llvm::Function*> seeds;

  // Cache for may-lead-to thread creation
  // N.B.: This is used for OptNoSingleThreadedScheduling.  We include
  // PosixPthreadJoin to ensure that we've generated all HBEdges for
  // exit->join before halting execution.
  seeds.clear();
  seeds.insert(KleeThreadCreateFn);
  seeds.insert(PosixPthreadJoin);
  PrecomputeCacheForMayLeadToCall(CG, BottomUpCG, seeds,
                                  &CacheForMayLeadToThreadCreation);

  if (DbgMayHappenBAThreadCreate) {
    llvm::errs() << "=== CacheForMayLeadToThreadCreation ===" << "\n";
    DumpInstructionRangeCache(CG, BottomUpCG, seeds, CacheForMayLeadToThreadCreation);
  }

  // Cache for may-happen-after thread creation
  // N.B.: We need to seed from klee_thread_create and all thread-spawn functions
  // N.B.: pthread_exit also happens-after thread creation, but it is not always
  // called explicitly!
  if (KleeThreadCreateFn) {
    seeds.clear();
    seeds.insert(KleeThreadCreateFn);
    seeds.insert(PosixPthreadExit);

    for (llvm::Value::use_iterator
         use = KleeThreadCreateFn->use_begin(); use != KleeThreadCreateFn->use_end(); ++use) {
      llvm::CallInst *call = dyn_cast<CallInst>(*use);
      if (!call || call->getCalledValue() != KleeThreadCreateFn)
        continue;
      std::vector<const llvm::Function*> targets;
      DSAA->getFunctionsForValue(call->getArgOperand(1), &targets);
      seeds.insert(targets.begin(), targets.end());
      if (DbgMayHappenBAThreadCreate) {
        for (size_t k = 0; k < targets.size(); ++k)
          std::cerr << "TARGET: " << targets[k]->getNameStr() << "\n";
      }
    }

    PrecomputeCacheForMayHappenAfterCall(CG, BottomUpCG, seeds,
                                         &CacheForMayHappenAfterThreadCreation);
  }

  if (DbgMayHappenBAThreadCreate) {
    llvm::errs() << "=== CacheForMayHappenAfterThreadCreation ===" << "\n";
    DumpInstructionRangeCache(CG, BottomUpCG, seeds, CacheForMayHappenAfterThreadCreation);
  }
}

bool CheckInstructionRangeCache(const InstructionRangeCacheTy &cache,
                                const llvm::Instruction *inst) {
  assert(inst);

  // Return true only if the instruction lies in a cached range.
  const llvm::BasicBlock *bb = inst->getParent();
  InstructionRangeCacheTy::const_iterator c = cache.find(bb);
  if (c == cache.end())
    return false;

  assert(c->second.start);
  assert(c->second.start != bb->end());
  assert(c->second.start->getParent() == bb);

  assert(c->second.end);
  assert(c->second.end != bb->end());
  assert(c->second.end->getParent() == bb);

  // Walk from start to end
  llvm::BasicBlock::const_iterator i = c->second.start;
  for (;;) {
    if (&*i == inst)
      return true;
    if (&*i == c->second.end)
      return false;
    ++i;
    assert(i != bb->end());
  }
}

bool CheckInstructionRangeCache(const InstructionRangeCacheTy &cache,
                                const klee::Thread &thread,
                                const llvm::Instruction *pc,
                                const bool incCallers) {
  assert(!thread.getStack().empty());

  // Check PC
  // Allow caller to specify the top-of-stack PC (it not specified, read from thread)
  if (!pc)
    pc = thread.getPC()->inst;

  if (CheckInstructionRangeCache(cache, pc))
    return true;

  // Check callers up the stack
  // N.B.: Bottom frame has no callers
  // N.B.: If "incCallers" is true, then we actually don't care about the call
  // instructions in each caller because we'll resolve the query for those call
  // instruction more precisely by checking callee stack frames.  Thus, we skip
  // the call instructions and instead check the *following* instructions.
  for (size_t k = 1; k < thread.getStack().size(); ++k) {
    const klee::StackFrame &sf = thread.getStack()[k];
    if (!incCallers) {
      if (CheckInstructionRangeCache(cache, sf.caller->inst))
        return true;
      continue;
    }
    if (llvm::InvokeInst *ii = dyn_cast<InvokeInst>(sf.caller->inst)) {
      if (CheckInstructionRangeCache(cache, ii->getNormalDest()->begin()))
        return true;
      if (CheckInstructionRangeCache(cache, ii->getUnwindDest()->begin()))
        return true;
    } else {
      klee::KInstIterator ktarget = sf.caller;
      ++ktarget;
      if (CheckInstructionRangeCache(cache, ktarget->inst))
        return true;
    }
  }

  return false;
}

bool MayLeadToThreadCreation(const klee::Thread &thread) {
  // IncCallers: say we have the following code
  //   foo() {    bar() {
  //     ...        ...
  //     bar()      @@@
  //   }          }
  // Where some code in the "..." may include thread create, but none of the
  // code in "@@@" does.  Assume foo() and bar() each have one basic block.
  // The cache will have two range entires: one for foo(), covering the entire
  // function, and one for bar(), covering the "..." only.  Say our thread's
  // top-of-stack is in "@@@".  The cache search has two steps: (1) check the
  // thread's PC, and (2) check the callers on the stack.  Step (1) notices
  // that "@@@" is outside the cached range.  Step (2), however, notices that
  // the call to bar() may include thread creation, so it returns true.  This
  // is too imprecise!  So, instead, in step (2), we check the instruction
  // *after* the call to bar(), rather than checking the call to bar() itself.
  return CheckInstructionRangeCache(CacheForMayLeadToThreadCreation, thread, NULL, true);
}

bool MayHappenAfterThreadCreation(const klee::Thread &thread,
                                  const llvm::Instruction *pc /* = NULL */) {
  return CheckInstructionRangeCache(CacheForMayHappenAfterThreadCreation, thread, pc, false);
}

bool DoesInstructionCallFunction(const DSCallGraph &CG,
                                 const llvm::Instruction *I,
                                 const llvm::DenseSet<const llvm::Function*> &Funcs) {
  // Return true if the specificied instruction calls a function in Funcs
  if (!isa<CallInst>(I) && !isa<InvokeInst>(I))
    return false;

  CallSite CS(const_cast<Instruction*>(cast<Instruction>(I)));

  // Direct call?
  const Function* Callee = CS.getCalledFunction();
  if (Callee) {
    return Funcs.count(Callee) > 0;
  }

  // Process all potential call targets
  // Need to check the entire SCC
  for (DSCallGraph::callee_iterator Target = CG.callee_begin(CS),
                                   TargetE = CG.callee_end(CS); Target != TargetE; ++Target) {
    for (DSCallGraph::scc_iterator F = CG.scc_begin(*Target),
                                  FE = CG.scc_end(*Target); F != FE; ++F) {
      if (Funcs.count(*F))
        return true;
    }
  }

  return false;
}

bool DoesBasicBlockLeadToReturn(const llvm::BasicBlock *bb,
                                const llvm::BasicBlock *ignored = NULL) {
  assert(bb);

  //
  // DoesBasicBlockLeadToReturn(bb, NULL)
  //   * Returns true if following any path from bb might lead to
  //     a ReturnInst.
  //
  // DoesBasicBlockLeadToReturn(bb, ignored)
  //   * Returns true as above, but ignores paths that go through
  //     "ignored".  The typical use case it to make "ignored" one
  //     of the successors of "bb" to check what can happen if you
  //     "go the other way" on a branch.
  //
  llvm::SmallPtrSet<const llvm::BasicBlock*,16> visited;
  std::stack<const llvm::BasicBlock*> stack;
  // return, unwind
  // unreachable
  // branch, switch, indirectbr, invoke,

  stack.push(bb);
  visited.insert(bb);

  // DFS
  while (!stack.empty()) {
    bb = stack.top();
    stack.pop();

    if (bb == ignored)
      continue;

    const llvm::TerminatorInst *inst = bb->getTerminator();
    if (isa<ReturnInst>(inst) || isa<UnwindInst>(inst))
      return true;

    for (succ_const_iterator succ = succ_begin(bb); succ != succ_end(bb); ++succ) {
      if (!visited.count(*succ)) {
        stack.push(*succ);
        visited.insert(*succ);
      }
    }
  }

  return false;
}

bool DoesBlockCallSync(const llvm::BasicBlock *bb) {
  for (BasicBlock::const_iterator it = bb->begin(); it != bb->end(); ++it) {
    if (!isa<CallInst>(it) && !isa<InvokeInst>(it))
      continue;
    if (IsSyncOpCaller(ImmutableCallSite(it).getCalledFunction()))
      return true;
  }
  return false;
}

bool DoesBlockEndWithProcessTerminate(const llvm::BasicBlock *bb) {
  BasicBlock::const_iterator it = bb->getTerminator();
  if (it == bb->begin())
    return false;
  --it;
  if (const llvm::CallInst *call = dyn_cast<CallInst>(it)) {
    Function *f = call->getCalledFunction();
    if (!f)
      return false;
    if (f == KleeProcessTerminateFn || f == KleeAssertFailFn)
      return true;
    if (f->isDeclaration())
      return false;
    return DoesBlockEndWithProcessTerminate(&f->getEntryBlock());
  }
  return false;
}

bool IsRecursiveFunction(const llvm::Function *F, const DSCallGraph &CG) {
  // Yes if sizeof(SCC) > 1
  const llvm::Function *LeaderF = CG.sccLeaderIfExists(F);
  if (!LeaderF)
    return false;
  if (LeaderF != F || (++CG.scc_begin(LeaderF) != CG.scc_end(LeaderF)))
    return true;

  // Yes if it calls self
  for (DSCallGraph::flat_iterator T = CG.flat_callee_begin(F),
                                 TE = CG.flat_callee_end(F); T != TE; ++T) {
    if (*T == F)
      return true;
  }

  return false;
}

void CallGraphPostOrder(const llvm::DSAA::CallerMapTy &TopDownCG,
                        const llvm::Function *F,
                        const llvm::DenseSet<const llvm::Function*> &shouldVisit,
                        llvm::DenseSet<const llvm::Function*> *visited,
                        std::vector<const llvm::Function*> *postorder) {
  typedef llvm::DSAA::CallerMapTy CallerMapTy;

  // Already visited?
  if (!visited->insert(F).second)
    return;

  // Ignore?
  if (!shouldVisit.empty() && !shouldVisit.count(F))
    return;

  // Go down the callgraph
  CallerMapTy::const_iterator CI = TopDownCG.find(F);
  if (CI != TopDownCG.end()) {
    for (CallerMapTy::mapped_type::const_iterator
         Callee = CI->second.begin(); Callee != CI->second.end(); ++Callee) {
      CallGraphPostOrder(TopDownCG, *Callee, shouldVisit, visited, postorder);
    }
  }

  // Post-visit
  postorder->push_back(F);
}

void SyncOpCallersPostOrder(const llvm::DSAA::CallerMapTy &BottomUpCG,
                            const llvm::DSAA::CallerMapTy &TopDownCG,
                            std::vector<const llvm::Function*> *postorder) {
  typedef llvm::DSAA::CallerMapTy CallerMapTy;
  llvm::DenseSet<const llvm::Function*> visited;

  // Enumerate the "root" nodes of the "sync op callers" subset of the call graph.
  // These are the fns \in SyncOpCallers that are never used as a callee.  Then,
  // for each root, we perform a post-order traversal.
  for (llvm::DenseSet<const llvm::Function*>::iterator
       F = SyncOpCallers.begin(); F != SyncOpCallers.end(); ++F) {
    CallerMapTy::const_iterator CI = BottomUpCG.find(*F);
    if (CI == BottomUpCG.end() || CI->second.begin() == CI->second.end())
      CallGraphPostOrder(TopDownCG, *F, SyncOpCallers, &visited, postorder);
  }
}

bool MustExecuteInFunction(const llvm::BasicBlock *bb, llvm::Function *F) {
  // Returns true iff "bb" must execute at least once on each call to "F"
  DominatorTree *PDT = GetPostdominatorTree(F);
  return PDT->dominates(bb, &F->getEntryBlock());
}

bool MustExecuteInAllNonExitingIterations(const llvm::BasicBlock *bb, llvm::Loop *loop) {
  // Returns true iff "bb" must execute at least once on each non-exiting
  // iteration of "loop".  The requires that (1) "bb" is contained in the
  // loop, and (2) "bb" dominates all loop backedges.  We check (2) by
  // enumerating all "latch" blocks.
  if (!loop->contains(bb))
    return false;

  llvm::BasicBlock *header = loop->getHeader();
  DominatorTree *DT = GetDominatorTree(header->getParent());

  for (pred_iterator pred = pred_begin(header); pred != pred_end(header); ++pred) {
    if (loop->contains(*pred) && !DT->dominates(bb, *pred))
      return false;
  }

  return true;
}

//
// Injecting region markers
//

namespace {

// XXX: Loops that have synchronization
// XXX: These have been covered with region markers
typedef std::set<const llvm::Loop*> LoopSetTy;
typedef std::set<llvm::Instruction*> InstSetTy;
typedef std::map<const llvm::Loop*, llvm::Instruction*> UncoveredMapTy;
static LoopSetTy CoveredLoops;         // loops that are covered
static LoopSetTy TrivialCoveredLoops;  // loops in CoveredLoops that we consider "trivial"
static UncoveredMapTy UncoveredLoops;  // foreach loop, map to the dormant region marker
static InstSetTy DormantRegionMarkers; // region markers to ignore

}  // namespace

struct RegionMarkersInjector {
private:
  typedef llvm::DSAA::CallerMapTy CallerMapTy;
  typedef std::set<const llvm::Function*> FuncSetTy;
  typedef llvm::DenseMap<const llvm::Function*, FuncSetTy> FuncUncoveredMapTy;
  typedef llvm::DenseSet<const llvm::Function*> FuncDenseSetTy;
  typedef llvm::DenseMap<llvm::Instruction*, FuncSetTy> RegionHeadersMapTy;

  FuncUncoveredMapTy _functionsWithUncoveredSyncOps;
  FuncDenseSetTy _functionsWithRegionMarkers;
  FuncDenseSetTy _functionsMustCallRegionMarkers;

  // Region markers go just before these instructions
  // The mapped FuncSetTy describes the set of sync ops covered by each region marker
  RegionHeadersMapTy _regionHeaders;

  struct Stats {
    uint64_t syncCoveredByHeader;
    uint64_t syncCoveredByNested;
    uint64_t syncUncovered;
    uint64_t syncUncoveredLoops;
    uint64_t syncSkippedNoRet;
    uint64_t syncSkippedHeuristic;
    uint64_t markersAtBlockEntry;
    uint64_t dormantMarkers;
    uint64_t redoDuringSearch;
  } _stats;

private:
  llvm::Instruction* GetRegionMarkerInsertionPoint(llvm::BasicBlock *BB);
  void AddRegionMarkerForBasicBlock(llvm::BasicBlock *BB, const FuncSetTy &covered);

  void EnumerateCalls(const DSCallGraph &CG,
                      const llvm::Instruction *I,
                      FuncSetTy *syncCallees,
                      bool *callsSync,
                      bool *callsRegionMarker);

  void MarkForSyncOps(const DSCallGraph &CG,
                      llvm::Function *F,
                      bool *marked,
                      FuncSetTy *uncovered);

  void Find(const DSCallGraph &CG,
            const llvm::DSAA::CallerMapTy &BottomUpCG,
            const llvm::DSAA::CallerMapTy &TopDownCG);

  void EnumerateUncoveredLoops(llvm::Module *module);

public:
  void FindAndInject(llvm::Module *module,
                     const DSCallGraph &CG,
                     const llvm::DSAA::CallerMapTy &BottomUpCG,
                     const llvm::DSAA::CallerMapTy &TopDownCG);
};

// TODO: put the region marker after all side-effect-free instructions.
//  this can (maybe?) improve loop invariant constraints emitted at the
//  end of partial eval
llvm::Instruction*
RegionMarkersInjector::GetRegionMarkerInsertionPoint(llvm::BasicBlock *BB) {
  // Markers go at the beginning of a block, right afters PHIs, dbgs, and allocas
  BasicBlock::iterator insertBefore = BB->getFirstNonPHIOrDbg();
  while (isa<AllocaInst>(insertBefore))
    ++insertBefore;
  return insertBefore;
}

void RegionMarkersInjector::AddRegionMarkerForBasicBlock(llvm::BasicBlock *BB,
                                                         const FuncSetTy &covered) {
  // Don't bother placing markers right before an unreachable
  llvm::Instruction *insertBefore = GetRegionMarkerInsertionPoint(BB);
  if (!isa<UnreachableInst>(insertBefore)) {
    if (_regionHeaders.count(insertBefore))
      _stats.markersAtBlockEntry++;
    _regionHeaders[insertBefore].insert(covered.begin(), covered.end());
  }
}

void RegionMarkersInjector::EnumerateCalls(const DSCallGraph &CG,
                                           const llvm::Instruction *I,
                                           FuncSetTy *syncCallees,
                                           bool *callsSync,
                                           bool *callsRegionMarker) {
  // Enumerate calls to uncovered sync ops
  if (!isa<CallInst>(I) && !isa<InvokeInst>(I))
    return;

  CallSite CS(const_cast<Instruction*>(cast<Instruction>(I)));

  // Process all potential call targets
  for (DSCallGraph::call_target_iterator F = CG.call_target_begin(CS),
                                        FE = CG.call_target_end(CS); F != FE; ++F) {
    // Calls sync?
    FuncUncoveredMapTy::iterator it = _functionsWithUncoveredSyncOps.find(*F);
    if (it != _functionsWithUncoveredSyncOps.end()) {
      syncCallees->insert(it->second.begin(), it->second.end());
      *callsSync = true;
    } else if (IsSyncOpCaller(*F)) {
      *callsSync = true;
    }
    // Calls a region marker?
    if (_functionsMustCallRegionMarkers.count(*F)) {
      *callsRegionMarker = true;
    }
  }
}

// TODO: Also skip loops with a fixed trip count?

void RegionMarkersInjector::MarkForSyncOps(const DSCallGraph &CG,
                                           llvm::Function *F,
                                           bool *marked          /* was anything marked? */,
                                           FuncSetTy *uncovered  /* what was left uncovered? */) {
  assert(marked);
  assert(uncovered);
  assert(uncovered->empty());

  if (DbgRegionMarkers) {
    llvm::errs() << "RegionMarkersInjector::MarkForSyncOps(" << F->getName() << ")\n";
  }

  // Treat pthread model functions as primitive sync ops
  // This: (a) allows us to ignore simple thread creation loops and thread
  // joining loops; (b) prevents us from inserting region markers into pthread
  // models; and (c) makes the debugging output nicer.
  if (IsSyncOpCaller(F) && F->getName().startswith("__klee_pthread_model_")) {
    *marked = false;
    assert(uncovered->empty());
    uncovered->insert(F);
    return;
  }

  // First: enumerate all uncovered sync ops
  typedef std::set<llvm::Loop*> LoopSetTy;
  typedef std::set<llvm::BasicBlock*> BlockSetTy;
  typedef llvm::DenseMap<llvm::Loop*, FuncSetTy> LoopFuncMapTy;
  typedef llvm::DenseMap<llvm::Loop*, BlockSetTy> LoopBlockMapTy;
  LoopFuncMapTy loopsUncoveredSync;
  LoopBlockMapTy loopsBlocksWithSync;
  LoopBlockMapTy loopsCoveredBlocks;
  LoopInfoTy* LI = GetLoopInfo(F);

  for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
    llvm::Loop *loop = LI->getLoopFor(BB);

    // Not in a loop: ignore this block unless it can reach a return
    // statement (and thus, possibly be contained in another loop).
    if (!loop && !DoesBasicBlockLeadToReturn(BB)) {
      _stats.syncSkippedNoRet++;
      continue;
    }

    // We use the "NULL" loop to refer to blocks outside of all loops
    FuncSetTy *funcs = &loopsUncoveredSync[loop];
    bool callsRegionMarker = false;
    bool callsSync = false;
    for (BasicBlock::const_iterator I = BB->begin(); I != BB->end(); ++I) {
      EnumerateCalls(CG, I, funcs, &callsSync, &callsRegionMarker);
    }
    if (callsSync) {
      loopsBlocksWithSync[loop].insert(BB);
    }
    if (callsRegionMarker) {
      for (llvm::Loop *l = loop; l; l = l->getParentLoop())
        loopsCoveredBlocks[l].insert(BB);
    }
  }

  // Construct a post-order (bottom-up) traversal of the loop nesting tree,
  // ending with "NULL" (outside of loops).
  std::vector<llvm::Loop*> postorder;
  for (LoopInfoTy::iterator outer = LI->begin(); outer != LI->end(); ++outer) {
    for (llvm::po_iterator<llvm::Loop*> loop = llvm::po_begin(*outer),
                                     loopEnd = llvm::po_end(*outer); loop != loopEnd; ++loop) {
      postorder.push_back(*loop);
    }
  }
  postorder.push_back(NULL);

  // Second: enumerate the set of loops we must cover
  // We do this with a post-order (bottom-up) traversal of the loop nesting tree
  LoopSetTy needscover;
  for (size_t k = 0; k < postorder.size(); ++k) {
    llvm::Loop *loop = postorder[k];

    // Default: cover all loops that contain synchronization
    const FuncSetTy &currUncoveredSync = loopsUncoveredSync[loop];
    bool coverThis = !loopsBlocksWithSync[loop].empty();
    bool hasCoveredBlocks = !loopsCoveredBlocks[loop].empty();

    // Is this loop already covered?
    // Yes if there exists a block with a region marker where the block
    // must execute on every iteration of the loop.
    // For example:
    //    for (...) {
    //       if (i < 0) { coveredblock }
    //    }
    //
    // The outer loop could iterate an unbounded number of times without
    // ever jumping to the inner loop, which would lead to an endless region,
    // so we consider this loop covered ONLY IF the regionmarker must-execute
    // in every iteration of the loop, i.e., if it post-dominates the loop's
    // header.
    //
    // This could be made weaker: e.g., if the loop executes every N iterations
    // for a small constant N, as in the example below, then we do not need to
    // mark the outer loop:
    //    for (i = 0 ... X) {
    //       if (i % 2 == 0) { coveredblock }
    //    }
    //
    // We perform this check if either:
    //   (a) the loop has synchronization that might not be covered, or
    //   (b) the loop calls a function that already includes a region marker;
    //       this case ensures that such explicit markers will be added to CoveredLoops
    if (OptRegionsCoverLoopOnce && (loop && (coverThis || hasCoveredBlocks))) {
      for (BlockSetTy::iterator
           bb = loopsCoveredBlocks[loop].begin(); bb != loopsCoveredBlocks[loop].end(); ++bb) {
        if (MustExecuteInAllNonExitingIterations(*bb, loop)) {
          if (DbgRegionMarkers) {
            llvm::errs() << "Skipping loop (already covered) :: " << F->getName()
                         << ":" << loop->getHeader()->getName()
                         << " covered by " << (*bb)->getName()
                         << "\n";
          }
          _stats.syncCoveredByNested++;
          CoveredLoops.insert(loop);  // XXX "covered" via optimization
          coverThis = false;
          break;
        }
      }
    }

    // If the loop is the "whole function" loop, we check if the function
    // can be added to "_functionsMustCallRegionMarkers"
    if (OptRegionsCoverLoopOnce && !loop) {
      for (BlockSetTy::iterator
           bb = loopsCoveredBlocks[loop].begin(); bb != loopsCoveredBlocks[loop].end(); ++bb) {
        if (MustExecuteInFunction(*bb, F)) {
          if (DbgRegionMarkers) {
            llvm::errs() << "Function must call region marker :: " << F->getName()
                         << " covered by " << (*bb)->getName()
                         << "\n";
          }
          _functionsMustCallRegionMarkers.insert(F);
          coverThis = false;
          break;
        }
      }
    }

    // Do we actually need to cover this loop?
    // Heuristically, we ignore certain sync ops when they appear alone
    // N.B.: We don't just "ignore" these in Find() because we can't always
    // ignore these sync ops; e.g., we can't ignore the loop:
    //    for (..) {
    //      create();  // could spawn/join an arbitrary number of threads
    //      join();
    //    }
    // However, we can ignore this loop:
    //    for (..) {
    //      create();  // we assume the total number of live threads is bounded
    //    }
    // Similarly, we can ignore this loop under the assumption that all
    // loops that invoke "notify" must be marked:
    //    while (..) {              // for each region, this can wake only as many
    //      pthread_cond_wait()     // times as there are calls to "notify"
    //    }
    if (OptRegionsIgnoreTrivialLoops && (coverThis && loop && currUncoveredSync.size() == 1)) {
      const llvm::Function *syncop = *currUncoveredSync.begin();
      if (syncop == KleeThreadCreateFn || syncop == KleeThreadTerminateFn ||
          syncop == PosixPthreadCreate || syncop == PosixPthreadJoin ||
          syncop == PosixPthreadCondWait) {
        llvm::BasicBlock *preheader = loop->getLoopPreheader();
        llvm::BasicBlock *header = loop->getHeader();
        // Push into the parent loop
        loopsUncoveredSync[loop->getParentLoop()].insert(syncop);
        loopsBlocksWithSync[loop->getParentLoop()].insert(preheader ? preheader : header);
        _stats.syncSkippedHeuristic++;
        if (DbgRegionMarkers) {
          llvm::errs() << "Skipping uncovered loop (heuristic) :: " << F->getName()
                       << ":" << header->getName()
                       << " calls " << syncop->getName()
                       << "\n";
        }
        CoveredLoops.insert(loop);  // XXX "covered" via optimization
        TrivialCoveredLoops.insert(loop);
        coverThis = false;
      }
    }

    // Cover if needed
    if (coverThis) {
      needscover.insert(loop);
      if (loop) {
        // The parent loop may need a region marker as well
        llvm::Loop *parent = loop->getParentLoop();
        llvm::BasicBlock *header = loop->getHeader();
        loopsCoveredBlocks[parent].insert(header);
        loopsBlocksWithSync[parent].insert(header);
        CoveredLoops.insert(loop);  // XXX will be actually covered
      }
    }
  }

  // Third: cover loops
  for (LoopSetTy::iterator it = needscover.begin(); it != needscover.end(); ++it) {
    llvm::Loop *loop = *it;
    if (!loop)
      continue;

    // Cover this loop
    AddRegionMarkerForBasicBlock(loop->getHeader(), loopsUncoveredSync[loop]);
    *marked = true;
    _stats.syncCoveredByHeader++;
    if (DbgRegionMarkers) {
      llvm::errs() << "Covered by loop region :: " << F->getName()
                   << ":" << loop->getHeader()->getName() << " covers ";
      for (BlockSetTy::iterator
           bb = loopsBlocksWithSync[loop].begin(); bb != loopsBlocksWithSync[loop].end(); ++bb) {
        llvm::errs() << (*bb)->getName() << " ";
      }
      llvm::errs() << "\n";
    }
  }

  // Fourth: Does the function itself need to be covered?
  if (needscover.count(NULL)) {
    const FuncSetTy &currUncoveredSync = loopsUncoveredSync[NULL];

    // FIXME: Recursive functions with synchronization are not supported!
    if (IsRecursiveFunction(F, CG)) {
      llvm::errs() << "ERROR: Recursive function calls sync! " << F->getName() << "\n";
      llvm::errs() << "BlocksWithSync: ";
      for (BlockSetTy::iterator
           bb = loopsBlocksWithSync[NULL].begin(); bb != loopsBlocksWithSync[NULL].end(); ++bb) {
        llvm::errs() << (*bb)->getName() << " ";
      }
      llvm::errs() << "\nUncoveredSync: ";
      PrintSortedFuncSet(currUncoveredSync, "", " ");
      llvm::errs() << "\n";
      assert(0);
    }

    // Propagate to parents
    _stats.syncUncovered++;
    assert(uncovered->empty());
    uncovered->insert(currUncoveredSync.begin(), currUncoveredSync.end());
  }
}

void RegionMarkersInjector::Find(const DSCallGraph &CG,
                                 const llvm::DSAA::CallerMapTy &BottomUpCG,
                                 const llvm::DSAA::CallerMapTy &TopDownCG) {
  // Reset
  _functionsWithUncoveredSyncOps.clear();
  _functionsWithRegionMarkers.clear();
  _functionsMustCallRegionMarkers.clear();
  _regionHeaders.clear();

  // These functions always invoke region markers
  _functionsMustCallRegionMarkers.insert(KleeIcsRegionMarkerFn);
  _functionsMustCallRegionMarkers.insert(PosixPthreadBarrierWait);

  // Post-order traversal of all SyncOpCallers
  // Using a post-order ensures that we visit all callees before callers.
  // N.B.: FindAndInject() ensures that sync ops are not called from recursive
  // fns, so we don't need to worry about cycles when constructing this order.
  // N.B.: We assume we have the whole program, thus we don't worry about the
  // possibility that some unknown external function may call a sync op.
  std::vector<const llvm::Function*> postOrder;
  SyncOpCallersPostOrder(BottomUpCG, TopDownCG, &postOrder);

  for (size_t k = 0; k < postOrder.size(); ++k) {
    llvm::Function *F = const_cast<llvm::Function*>(postOrder[k]);
    FuncSetTy &uncovered = _functionsWithUncoveredSyncOps[F];
    uncovered.clear();

    if (SyncOpFunctions.count(F)) {
      uncovered.insert(F);
      continue;
    }

    // Place region markers to cover all uncovered sync op callsites
    bool marked = false;
    MarkForSyncOps(CG, F, &marked, &uncovered);

    if (uncovered.empty()) {
      _functionsWithUncoveredSyncOps.erase(F);
    }
  }
}

void RegionMarkersInjector::EnumerateUncoveredLoops(llvm::Module *module) {
  for (Module::iterator f = module->begin(); f != module->end(); ++f) {
    if (f->isDeclaration())
      continue;

    // Ignore model functions
    if (f->getName().startswith("__klee_") || f->getName().startswith("__ics_") ||
        f->getName().startswith("klee_init_"))
      continue;

    // Write UncoveredLoops[l] = NULL forall uncovered loops in this function
    LoopInfoTy* LI = GetLoopInfo(f);
    for (LoopInfoTy::iterator outer = LI->begin(); outer != LI->end(); ++outer) {
      for (llvm::po_iterator<llvm::Loop*> loop = llvm::po_begin(*outer),
                                       loopEnd = llvm::po_end(*outer); loop != loopEnd; ++loop) {
        // Ignore loops with small constant trip-counts
        const unsigned trip = loop->getSmallConstantTripCount();
        if (trip && trip <= 1024) {
          if (DbgRegionMarkers) {
            const llvm::BasicBlock *header = loop->getHeader();
            const llvm::Function *f = header->getParent();
            llvm::errs() << "Ignoring uncovered loop :: " << f->getName()
                         << ":" << header->getName()
                         << " trip count is " << trip << "\n";
          }
          continue;
        }
        if (trip && DbgRegionMarkers) {
          const llvm::BasicBlock *header = loop->getHeader();
          const llvm::Function *f = header->getParent();
          llvm::errs() << "Not ignoring uncovered loop :: " << f->getName()
                       << ":" << header->getName()
                       << " trip count is " << trip << "\n";
        }
        // Uncovered?
        if (!CoveredLoops.count(*loop))
          UncoveredLoops[*loop] = NULL;
      }
    }
  }
}

void RegionMarkersInjector::FindAndInject(llvm::Module *module,
                                          const DSCallGraph &CG,
                                          const llvm::DSAA::CallerMapTy &BottomUpCG,
                                          const llvm::DSAA::CallerMapTy &TopDownCG) {
  memset(&_stats, 0, sizeof _stats);

  // Assert-fail if there are any recursive functions that contain synchronization.
  // These are not currently supported.
  for (Module::iterator f = module->begin(); f != module->end(); ++f) {
    if (f->isDeclaration() || !IsRecursiveFunction(f, CG))
      continue;

    llvm::errs() << "WARNING: recursive functions found: " << f->getName() << "\n";
    if (IsSyncOpCaller(f)) {
      llvm::errs() << "ERROR: recursive functions with synchronization are not supported: "
                   << f->getName() << "\n";
      assert(0);
    }
  }

  // Go find places to put region markers
  Find(CG, BottomUpCG, TopDownCG);
  EnumerateUncoveredLoops(module);

  // Inject live region markers
  // Markers go just before the specified instruction
  for (RegionHeadersMapTy::iterator it = _regionHeaders.begin(); it != _regionHeaders.end(); ++it) {
    llvm::Instruction *I = it->first;
    llvm::Instruction *marker = CallInst::Create(KleeIcsRegionMarkerFn);
    marker->insertBefore(I);
    // DEBUG
    if (DbgRegionMarkers) {
      const llvm::BasicBlock *bb = I->getParent();
      llvm::errs() << "==== Placing region marker ===="
                   << "\n  in: " << bb->getParent()->getName()
                   << "\n  bb: " << bb->getName();
      struct {
        const char *label;
        const llvm::Instruction *inst;
      } *tmp, toprint[] = {
        { "\n  bb: ", bb->begin() },
        { "\n  @: ", I }
      };
      for (tmp = toprint; tmp < toprint+2; ++tmp) {
        std::string file = "???";
        unsigned line = 0;
        if (llvm::MDNode *N = tmp->inst->getMetadata("dbg")) {
          llvm::DILocation loc(N);
          file = loc.getFilename();
          line = loc.getLineNumber();
        }
        llvm::errs() << tmp->label << file.substr(file.find_last_of('/') + 1) << ":" << line
                     << tmp->label << (*tmp->inst);
      }
      llvm::errs() << "\n  covers";
      PrintSortedFuncSet(it->second, "", " ");
    }
  }

  // Inject dormant region markers to the heads of uncovered loops
  for (UncoveredMapTy::iterator it = UncoveredLoops.begin(); it != UncoveredLoops.end(); ++it) {
    const llvm::Loop *loop = it->first;
    llvm::Instruction *insertBefore = GetRegionMarkerInsertionPoint(loop->getHeader());
    llvm::Instruction *marker = CallInst::Create(KleeIcsRegionMarkerFn);
    marker->insertBefore(insertBefore);
    _stats.dormantMarkers++;

    // Remember that this marker is dormant
    it->second = marker;
    DormantRegionMarkers.insert(marker);
    if (DbgRegionMarkers) {
      const llvm::BasicBlock *header = loop->getHeader();
      const llvm::Function *f = header->getParent();
      llvm::errs() << "Inserted dormant region marker :: " << f->getName()
                   << ":" << header->getName() << "\n";
    }
  }

  // DEBUG
  if (DumpRegionMarkerCfgs) {
    FuncSetTy funcs;
    for (RegionHeadersMapTy::iterator it = _regionHeaders.begin(); it != _regionHeaders.end(); ++it) {
      funcs.insert(it->first->getParent()->getParent());
    }
    for (FuncSetTy::iterator F = funcs.begin(); F != funcs.end(); ++F) {
      llvm::WriteGraph(*F, (*F)->getNameStr() + ".cfg", true);
    }
  }

  // STATS
  if (DumpRegionMarkerStats) {
    std::cerr << "RegionMarkersInjector::FindAndInject() Stats"
              << "\n> Overall"
              << "\n    total markers: " << _regionHeaders.size()
              << "\n    total functions with markers: " << _functionsWithRegionMarkers.size()
              << "\n    total functions uncovered sync: " << _functionsWithUncoveredSyncOps.size()
              << "\n> Algorithm"
              << "\n    covered-by-hdr:    " << _stats.syncCoveredByHeader
              << "\n    covered-by-nested: " << _stats.syncCoveredByNested
              << "\n    uncovered-fns:     " << _stats.syncUncovered
              << "\n    uncovered-loops:   " << _stats.syncUncoveredLoops
              << "\n    skipped-noret:     " << _stats.syncSkippedNoRet
              << "\n    skipped-heuristic: " << _stats.syncSkippedHeuristic
              << "\n    markers-at-block:  " << _stats.markersAtBlockEntry
              << "\n    dormant-markers:   " << _stats.dormantMarkers
              << "\n    redo-during-search: " << _stats.redoDuringSearch
              << "\n";
  }
}

//
// Initialization API
//

void InitializePrecondSlice(klee::KModule *kmodule) {
  assert(kmodule);

  llvm::Module *module = kmodule->module;
  assert(module);

  // Get special functions
  KleeAssumeFn = GetFunctionWithWarning(module, "klee_assume", "klee");
  KleeThreadCreateFn = GetFunctionWithWarning(module, "klee_thread_create", "klee");
  KleeThreadNotifyFn = GetFunctionWithWarning(module, "klee_thread_notify", "klee");
  KleeThreadSleepFn = GetFunctionWithWarning(module, "klee_thread_sleep", "klee");
  KleeThreadTerminateFn = GetFunctionWithWarning(module, "klee_thread_terminate", "klee");
  KleeProcessTerminateFn = GetFunctionWithWarning(module, "klee_process_terminate", "klee");
  KleeAssertFailFn = GetFunctionWithWarning(module, "klee_assert_fail", "klee");

  PosixPthreadBarrierWait =
    GetFunctionWithWarning(module, "__klee_pthread_model_pthread_barrier_wait", "Cloud9/POSIX");
  PosixPthreadCondWait =
    GetFunctionWithWarning(module, "__klee_pthread_model_pthread_cond_wait", "Cloud9/POSIX");
  PosixPthreadCreate =
    GetFunctionWithWarning(module, "__klee_pthread_model_pthread_create", "Cloud9/POSIX");
  PosixPthreadJoin =
    GetFunctionWithWarning(module, "__klee_pthread_model_pthread_join", "Cloud9/POSIX");
  PosixPthreadExit =
    GetFunctionWithWarning(module, "__klee_pthread_model_pthread_exit", "Cloud9/POSIX");

  GetFunctionsWithWarning(module, SyncOpFunctionNames, "sync op", &SyncOpFunctions);
  GetFunctionsWithWarning(module, UclibcSyncOpWrapperNames, "uclibc sync wrapper", &UclibcSyncOpWrappers);

  // void klee_ics_region_marker(void)
  module->getOrInsertFunction("klee_ics_region_marker",
                              llvm::FunctionType::get(
                                  llvm::Type::getVoidTy(llvm::getGlobalContext()),
                                  false));
  KleeIcsRegionMarkerFn = GetFunctionWithWarning(module, "klee_ics_region_marker", "klee");

  // Run alias analysis
  CLOUD9_DEBUG("Running alias analysis");
  TheAARunner.RunAliasAnalysis(module);
  AA = TheAARunner.getAA();
  DSAA = TheAARunner.getDSAA();
  TheAACache.setAA(&TheAARunner);

  // Pre-processing
  PrecomputeSyncOpCallers(module, TheAARunner.getDSAA()->getBottomUpCallGraph());

  if (DoRegionFormation) {
    RegionMarkersInjector().FindAndInject(module,
                                          TheAARunner.getDSAA()->getCallGraph(),
                                          TheAARunner.getDSAA()->getBottomUpCallGraph(),
                                          TheAARunner.getDSAA()->getTopDownCallGraph());
  } else {
    // unset options
    DoLazyRegionCovering = false;
    MinTimeBetweenRegionMarkers = 0;
  }

  // Post-processing
  PrecomputeDataflowCaches(TheAARunner.getDSAA()->getCallGraph(),
                           TheAARunner.getDSAA()->getBottomUpCallGraph());

  // Export analyses
  kmodule->AA = TheAARunner.getAA();
  kmodule->DSAA = TheAARunner.getDSAA();

  // DEBUG
  if (DbgSyncOpCallers)
    PrintSortedFuncSet(SyncOpCallers, "SyncOpCallers");
}

//
// Finalization API
//

void FinalizePrecondSlice() {
  TheAARunner.DoneWithAliasAnalysis();
}

void RemoveDormantRegionMarkers(const bool verbose) {
  // Erase the instructions
  for (InstSetTy::iterator
       it = DormantRegionMarkers.begin(); it != DormantRegionMarkers.end(); ++it) {
    (*it)->eraseFromParent();
  }
  // These are now invalid
  DormantRegionMarkers.clear();
  UncoveredLoops.clear();
}

//-----------------------------------------------------------------------------
// An event handler that logs a trace to ExecutionState::precondSliceTrace
//
// NB: The PrecondSliceTrace::Entry constructor assumes kstate->PC() has already
// been incremented (and thus represents the currently executing instruction;
// kstate->prevPC) represents the next instruction to execute).  This is true
// for all events below, except onControlFlowEvent(STEP), which we ignore.
//-----------------------------------------------------------------------------

void PrecondSliceTraceLogger::onConstraintAdded(klee::ExecutionState *kstate,
                                                ref<Expr> condition,
                                                klee::ForkClass forkclass) {
  kstate->precondSliceTrace.pushAssertion(kstate, condition, forkclass);

  if (DbgConstraints) {
    klee::KInstruction *kinst = kstate->precondSliceTrace.back().kinst();

    llvm::errs() << "ADD CONSTRAINT"
                 << "\n  node: " << kstate->getCloud9State()->getNode().get()
                 << "\n  instr: " << (*kinst->inst)
                 << "\n  file: " << kinst->info->file.substr(kinst->info->file.find_last_of('/') + 1)
                                 << ":" << kinst->info->line
                 << "\n  class: " << KleeForkClassToString(forkclass);
    std::cerr    << "\n  expr: " << condition << std::endl;
  }
}

void PrecondSliceTraceLogger::onControlFlowEvent(klee::ExecutionState *kstate,
                                                 ControlFlowEvent event,
                                                 klee::ThreadUid targetTuid,
                                                 klee::KInstruction *targetKinst,
                                                 llvm::Function *exCall) {
  if (event == cloud9::worker::STEP)
    return;

  // Skip external calls since we don't have the code for them
  // (and therefore there really isn't a "control transfer")
  if (event == cloud9::worker::CALL && exCall != NULL)
    return;

  // Convert unwinds to returns
  if (event == cloud9::worker::UNWIND)
    event = cloud9::worker::RETURN;

  // Convert external call returns to branches (since there is no matching CALL)
  if (event == cloud9::worker::RETURN && exCall != NULL)
    event = cloud9::worker::BRANCH;

  // Translate the event
  PrecondSliceTrace::Kind kind;
  if (event == cloud9::worker::BRANCH) {
    kind = PrecondSliceTrace::Branch;
  } else if (event == cloud9::worker::CALL) {
    kind = PrecondSliceTrace::Call;
  } else if (event == cloud9::worker::RETURN) {
    kind = PrecondSliceTrace::Ret;
  } else if (event == cloud9::worker::CONTEXT_SWITCH) {
    kind = PrecondSliceTrace::CtxSwitch;
  } else {
    std::cerr << "UNEXPECTED EVENT: " << event << std::endl;
    assert(false);
  }

  kstate->precondSliceTrace.pushControlTransfer(kstate, targetTuid, targetKinst, kind);

  // Update the "crossedIcsLoopedRegion" flag
  // This is true if we're jumping into a covered loop either directly
  // or via a backedge (i.e., or if going around again)
  if (event == cloud9::worker::BRANCH ) {
    llvm::Instruction *currI = kstate->prevPC()->inst;
    llvm::Instruction *nextI = kstate->pc()->inst;
    llvm::BasicBlock *currBB = currI->getParent();
    llvm::BasicBlock *nextBB = nextI->getParent();
    llvm::Function *f = currBB->getParent();
    // sanity check
    assert(currI == currBB->getTerminator());
    assert(nextBB->getParent() == f);
    // jumping into a new looped region?
    LoopInfoTy *LI = GetLoopInfo(f);
    llvm::Loop *currLoop = LI->getLoopFor(currBB);
    llvm::Loop *nextLoop = LI->getLoopFor(nextBB);
    if (CoveredLoops.count(nextLoop) && !TrivialCoveredLoops.count(nextLoop)) {
      const bool isBackedge = GetDominatorTree(f)->dominates(nextBB, currBB);
      const bool isNewLoopRegion = (currLoop != nextLoop && !nextLoop->contains(currLoop));
      if (isBackedge || isNewLoopRegion) {
        kstate->crossedIcsLoopedRegion[kstate->crtThreadUid()] = true;
      }
    }
  }

  // DEBUG
  if (DbgCtxSwitchEvents) {
    if (event == cloud9::worker::CONTEXT_SWITCH) {
      std::cerr << "CTX SWITCH "
                << kstate->crtThreadUid() << " "
                << kstate->prevPC()->inst->getParent()->getParent()->getNameStr()
                << ":"
                << kstate->prevPC()->info->line
                << " to "
                << targetTuid << " " << targetKinst << " "
                << (targetKinst ? targetKinst->inst->getParent()->getParent()->getNameStr() : "0x0")
                << ":"
                << (targetKinst ? targetKinst->info->line : 0)
                << "\n";
    }
  }

  // Careful: Don't do this from a branch
  // This might terminate the state, which in turn will discard *all* states
  // on the queue.  Our caller (executeInstruction) is not prepared for that.
  // TODO: this is pretty gross, there must be a better way
  if (event != cloud9::worker::BRANCH)
    TerminateStateIfSingleThreaded(kstate, targetKinst);
}

void PrecondSliceTraceLogger::onMemoryAccess(klee::ExecutionState *kstate,
                                             const klee::MemoryObject *mo,
                                             ref<Expr> offset,
                                             ref<Expr> nbytes,
                                             bool isStore,
                                             bool isAlloc) {
  if (isAlloc) {
    kstate->precondSliceTrace.pushAllocation(kstate, mo);
  } else {
    PrecondSliceTrace::Kind kind = isStore ? PrecondSliceTrace::Store : PrecondSliceTrace::Load;
    kstate->precondSliceTrace.pushMemoryAccess(kstate, mo, offset, nbytes, kind);
  }

  /*
  std::cerr << "MEMORY ACCESS !!!!! "
            << "<" << kstate->crtThreadUid().first << "," << kstate->crtThreadUid().second << "> "
            << kstate->prevPC()->inst->getParent()->getParent()->getNameStr()
            << ":"
            << kstate->prevPC()->info->line
            << " store:" << isStore
            << " alloc:" << isAlloc
            << " object:" << mo
            << " offset:" << offset
            << "\n";
  */
}

void PrecondSliceTraceLogger::onResolvedSymbolicPtr(klee::ExecutionState *kstate,
                                                    ref<klee::SymbolicPtrResolution> res) {
  kstate->precondSliceTrace.pushResolvedSymbolicPtr(kstate, res);
}

//-----------------------------------------------------------------------------
// DEBUGGING: a decorator for dumping symbolic execution tree grahs
//-----------------------------------------------------------------------------

class ICSWorkerNodeDecorator: public cloud9::worker::WorkerNodeDecorator {
public:
  ICSWorkerNodeDecorator(const DepthFirstICS *searcher, WorkerTree::Node *highlight)
    : cloud9::worker::WorkerNodeDecorator(highlight),
      _searcher(searcher),
      _highlight(highlight)
  {}

  const DepthFirstICS *_searcher;
  WorkerTree::Node *_highlight;

  void operator() (WorkerTree::Node *node, deco_t &deco, edge_deco_t &inEdges) {
    cloud9::worker::WorkerNodeDecorator::operator() (node, deco, inEdges);

    // Make a better label
    std::ostringstream label;

    if (!node->layerExists(WORKER_LAYER_STATES)) {
      label << "\"JobLayer?"
            << "\\nfork:" << (**node).getForkTag().forkClass
            << "\\njob:" << (**node).getJob()
            << "\\nnode:" << node
            << "\"";
    }

    else if (!(**node).getSymbolicState()) {
      label << "\"NoSymState?"
            << "\\nfork:" << (**node).getForkTag().forkClass
            << "\\njob:" << (**node).getJob()
            << "\\nnode:" << node
            << "\"";
    }

    else {
      SymbolicState *state = (**node).getSymbolicState();
      klee::ExecutionState *kstate = state->getKleeState();
      const klee::InstructionInfo *kinfo = kstate->crtThread().getPrevPC()->info;

      const bool root = _searcher->isRootState(state);
      const bool queued = _searcher->isQueuedState(state);
      const bool newregion = _searcher->isRegionFrontierState(state);

      std::string status;
      if (root) status += "Root";
      if (queued) status += "DfsQueued";
      if (newregion) status += "RegionFrontier";

      if (!root && !queued && !newregion)
        status = "BAD:" + status;

      label << "\""
            << kinfo->file.substr(kinfo->file.find_last_of('/') + 1) << ":" << kinfo->line
            << "\\n<"
            << kstate->crtThread().getTid() << "," << kstate->crtThread().getPid()
            << ">\\nfork:" << (**node).getForkTag().forkClass
            << "\\njob:" << (**node).getJob()
            << "\\nnode:" << node
            << "\\nstate:" << state
            << "\\nstatus:" << status
            << "\"";

      if (queued)
        deco["fillcolor"] = "aquamarine3";
      if (newregion)
        deco["fillcolor"] = "darkolivegreen3";
    }

    deco["label"] = label.str();
  }
};

//-----------------------------------------------------------------------------
// Region IDs
//-----------------------------------------------------------------------------

// The standard sorting function
bool SortByCtx(const ParallelRegionId::Context &lhs, const ParallelRegionId::Context &rhs) {
  return lhs.strippedctx < rhs.strippedctx;
}

// The unoptimized sorting function
bool SortByTidAndCtx(const ParallelRegionId::Context &lhs, const ParallelRegionId::Context &rhs) {
  if (lhs.canonicalTuid != rhs.canonicalTuid)
    return lhs.canonicalTuid < rhs.canonicalTuid;
  return lhs.strippedctx < rhs.strippedctx;
}

// For printing only
bool SortByThreadId(const ParallelRegionId::Context &lhs, const ParallelRegionId::Context &rhs) {
  return lhs.canonicalTuid < rhs.canonicalTuid;
}

ParallelRegionId::ParallelRegionId(klee::ExecutionState *kstate) {
  contexts.reserve(kstate->threads.size());

  // Copy each per-thread context, skipping threads that have exited
  // XXX FIXME: There really should be a better way to detect if a thread has
  // exited, but right now we can't remove a thread from the ExecutionState
  // before calling schedule() (and this method is called transitively from
  // schedule()), so this is the best we can do ...
  for (klee::ExecutionState::threads_ty::const_iterator
       t = kstate->threads.begin(); t != kstate->threads.end(); ++t) {
    if (CallInst *call = dyn_cast<CallInst>(t->second.getPrevPC()->inst)) {
      if (call->getCalledValue() == KleeThreadTerminateFn)
        continue;
    }
    Context c;
    c.fullctx = klee::CallingContext::FromThreadStackWithPC(t->second);
    if (klee::clvars::OptSymbolicThreadIds) {
      c.strippedctx = c.fullctx->removePthreadsFunctions();
    } else {
      c.strippedctx = c.fullctx;
    }
    c.canonicalTuid = t->first;
    contexts.push_back(c);
  }

  // Sort the context list
  if (klee::clvars::OptSymbolicThreadIds) {
    std::stable_sort(contexts.begin(), contexts.end(), SortByCtx);
  }
}

// Operators

bool operator<(const ParallelRegionId::Context &lhs, const ParallelRegionId::Context &rhs) {
  if (klee::clvars::OptSymbolicThreadIds) {
    return SortByCtx(lhs, rhs);
  } else {
    return SortByTidAndCtx(lhs, rhs);
  }
}

bool operator<(const ParallelRegionId &lhs, const ParallelRegionId &rhs) {
  return lhs.contexts < rhs.contexts;
}

// DEBUG

template<class T>
struct PrintWithPrefix {
  const T &obj;
  const char *const prefix;
  PrintWithPrefix(const T &_obj, const char *_prefix = "") : obj(_obj), prefix(_prefix) {}
};

typedef PrintWithPrefix<ParallelRegionId> ParallelRegionIdPrinter;

template<class OStream>
OStream& operator<<(OStream &os, const ParallelRegionIdPrinter &p) {
  const ParallelRegionId &id = p.obj;

  // Special case: the initial region
  if (id.contexts.empty()) {
    os << p.prefix << "<init>\n";
    return os;
  }

  char prefix[128];
  snprintf(prefix, sizeof prefix, "%s  ", p.prefix);

  // General case: print all single-threaded contexts, sorted by thread id
  ParallelRegionId::ContextListTy contexts = id.contexts;
  std::sort(contexts.begin(), contexts.end(), SortByThreadId);

  for (size_t k = 0; k < contexts.size(); ++k) {
    const ParallelRegionId::Context &c = contexts[k];
    os << p.prefix << c.canonicalTuid << "\n";
    os << klee::CallingContextPrinter(c.fullctx, prefix);
  }

  return os;
}

//-----------------------------------------------------------------------------
// SimpleStat
//-----------------------------------------------------------------------------

void DepthFirstICS::SimpleStat::addSample(size_t n) {
  samples++;
  accum += n;

  static const size_t sep[] =
    { 10, 50, 100, 250, 500, 1000, 2000, 10000, 25000, 50000, 75000, 100000, 200000, 0 };
  for (const size_t *b = sep; *b; ++b) {
    if (n <= *b) {
      buckets[*b]++;
      break;
    }
  }
}

void DepthFirstICS::SimpleStat::dump(std::ostream &os, const char *name) const {
  const double avg = samples ? (double)accum/(double)samples : 0.0;
  os << "# " << name << "Avg: " << avg << "\n";
  for (std::map<size_t,size_t>::const_iterator it = buckets.begin(); it != buckets.end(); ++it) {
    os << "# " << name << "Bucket[" << it->first << "]: " << it->second << "\n";
  }
}

//-----------------------------------------------------------------------------
// DFS scheduling optimizations
//-----------------------------------------------------------------------------

namespace {

//
// Shortest path first
//

struct ShortestPathFirstOpt_Stats {
  size_t attempts;
  size_t samebb;
  size_t returnKeep;
  size_t returnSwap;
  size_t unreachableKeep;
  size_t unreachableSwap;
  size_t assertfailKeep;
  size_t assertfailSwap;
  size_t postdomKeep;
  size_t postdomSwap;
  size_t loopNestKeep;
  size_t loopNestSwap;
  size_t loopAdvance;
  size_t loopExiting;
  size_t loopCanExitKeep;
  size_t loopCanExitSwap;
} TheShortestPathFirstOpt_Stats;

SymbolicState* PickCheapestNeighboringState(SymbolicState *curr, SymbolicState *other) {
  assert(curr);
  assert(other);

  // Check if we just did a branch
  klee::ExecutionState *s1 = curr->getKleeState();
  klee::ExecutionState *s2 = other->getKleeState();
  if (s1->prevPC() != s2->prevPC() || s1->crtThreadUid() != s2->crtThreadUid())
    return curr;

  // States should have started from the same instruction and stayed in the same function
  llvm::BasicBlock *bb0 = s1->prevPC()->inst->getParent();
  llvm::BasicBlock *bb1 = s1->pc()->inst->getParent();
  llvm::BasicBlock *bb2 = s2->pc()->inst->getParent();
  llvm::Function *F = bb0->getParent();
  assert(F == bb1->getParent());
  assert(F == bb2->getParent());

  if (DbgSchedulingOptimizations) {
    llvm::errs() << "TRY LOOP OPT:"
                 << "\n  .. F: " << F->getName();
    DumpKInstruction("\n  .. PrevPC:", s1->prevPC());
    DumpKInstruction("\n  .. PC_1:", s1->pc());
    DumpKInstruction("\n  .. PC_2:", s2->pc());
    llvm::errs() << "\n  .. BB_0: " << bb0
                 << "\n  .. BB_1: " << bb1
                 << "\n  .. BB_2: " << bb2
                 << "\n";
  }

  //
  // Check if bb2 "bails" out of the current loop or function faster than bb1
  //   yes => `return other`
  //   no  => `return curr`
  //
  // TODO: prefer branch that does not make a recursive call?
  // TODO: rewrite this function to use a cost/distance metric?
  //

  ShortestPathFirstOpt_Stats &stats = TheShortestPathFirstOpt_Stats;
  stats.attempts++;

  if (bb1 == bb2) {
    stats.samebb++;
    return curr;
  }

  // Prefer the block ending with a "return", if any
  // TODO: If both end in a return, pick the shortest distance?
  if (isa<ReturnInst>(bb1->getTerminator())) {
    stats.returnKeep++;
    return curr;
  }
  if (isa<ReturnInst>(bb2->getTerminator())) {
    stats.returnSwap++;
    return other;
  }

  // If both blocks end in unreachable, prefer the block that calls sync
  if (isa<UnreachableInst>(bb1->getTerminator()) &&
      isa<UnreachableInst>(bb2->getTerminator())) {
    if (DoesBlockCallSync(bb2)) {
      stats.unreachableSwap++;
      return other;
    } else {
      stats.unreachableKeep++;
      return curr;
    }
  }

  // Prefer the block ending with "unreachable", unless it calls __assert_fail(), in
  // which case, prefer the other block (we try to assume that assertions never fire).
  if (isa<UnreachableInst>(bb1->getTerminator())) {
    if (OptSkipEarlyExitBranches && DoesBlockEndWithProcessTerminate(bb1)) {
      stats.assertfailSwap++;
      return other;
    } else {
      stats.assertfailKeep++;
      return curr;
    }
  }
  if (isa<UnreachableInst>(bb2->getTerminator())) {
    if (OptSkipEarlyExitBranches && DoesBlockEndWithProcessTerminate(bb2)) {
      stats.assertfailKeep++;
      return curr;
    } else {
      stats.assertfailSwap++;
      return other;
    }
  }

  // Prefer postdominators (e.g., this skips around an if-then that has no else)
  if (GetPostdominatorTree(F)->dominates(bb1, bb2)) {
    stats.postdomKeep++;
    return curr;
  }
  if (GetPostdominatorTree(F)->dominates(bb2, bb1)) {
    stats.postdomSwap++;
    return other;
  }

  // N.B.: These loop checks are largely subsumed by the postdom check above,
  // but these can trigger in some more irregular cases.
  LoopInfoTy *LI = GetLoopInfo(F);
  llvm::Loop* loop1 = LI->getLoopFor(bb1);
  llvm::Loop* loop2 = LI->getLoopFor(bb2);

  if (loop1 != loop2) {
    // Prefer the outer-most loop
    if (loop1 && loop1->contains(loop2)) {
      stats.loopNestKeep++;
      return curr;
    }
    if (!loop2 || loop2->contains(loop1)) {
      stats.loopNestSwap++;
      return other;
    }
    // Prefer the *new* loop (e.g., if the code has consecutive loops)
    llvm::Loop* loop0 = LI->getLoopFor(bb0);
    if (loop0 == loop1) {
      stats.loopAdvance++;
      return other;
    }
  }
  // Prefer branches which jump to a node that exits the loop
  else {
    if (loop2 && loop2->isLoopExiting(bb2)) {
      stats.loopExiting++;
      return other;
    }
  }

  // Check if one state makes us more likely to trigger the branch-exit condition.
  // To do this, we find the induction variable "induct", then check if the backedge
  // would have been skipped in either state if "induct = induct+1".
  // N.B.: This is unsound (the trip count may not be loop-invariant), but that's
  // ok since this is a heuristic that affects performance, not correctness.
  if (loop1 && loop1 == loop2) {
    llvm::PHINode *induct = loop1->getCanonicalInductionVariable();
    if (induct && induct->getNumIncomingValues() == 2) {
      // Compute the branch for the backedge leading to the induction variable
      // Adapted from Loop::getTripCount()
      const bool use0 = loop1->contains(induct->getIncomingBlock(0));
      llvm::Value *nextInduct = induct->getIncomingValue(!use0);
      llvm::BasicBlock *backedgeBB = induct->getIncomingBlock(!use0);
      llvm::BranchInst *br = dyn_cast<BranchInst>(backedgeBB->getTerminator());
      if (br && br->isConditional()) {
        llvm::ICmpInst *icmp = dyn_cast<ICmpInst>(br->getCondition());
        if (icmp && (icmp->getOperand(0) == nextInduct || icmp->getOperand(1) == nextInduct)) {
          klee::KInstruction *kinduct = LookupKInstruction(s1, induct);
          klee::KInstruction *kicmp = LookupKInstruction(s1, icmp);
          // N.B.: We have to eval "induct" here rather than "nextInduct", because we
          // cannot be sure whether "nextInduct" olds the value of the current or prior
          // loop iteration (it depends on how the instructions are scheduled)
          ref<Expr> ind = s1->executor->getDestCell(*s1, kinduct).value;
          ref<Expr> one = klee::ConstantExpr::create(1, ind->getWidth());
          ref<Expr> lhs, rhs;
          ref<Expr> cond;
          if (icmp->getOperand(0) == nextInduct) {
            // N.B.: The operands should eval to the same exprs in both states (s1 & s2)
            lhs = klee::AddExpr::create(ind, one);
            rhs = s1->executor->eval(kicmp, 1, *s1).value;
          } else {
            lhs = s1->executor->eval(kicmp, 0, *s1).value;
            rhs = klee::AddExpr::create(ind, one);
          }
          if (lhs.get() && rhs.get()) {
            cond = Expr::createFromICmp(icmp, lhs, rhs);
            // Check satisfiability
            klee::TimingSolver *solver = s1->executor->getTimingSolver();
            bool canexit1 = false;
            bool canexit2 = false;
            if (br->getSuccessor(0) == loop1->getHeader()) {
              solver->mayBeFalse(*s1, cond, canexit1);
              solver->mayBeFalse(*s2, cond, canexit2);
            } else {
              solver->mayBeTrue(*s1, cond, canexit1);
              solver->mayBeTrue(*s2, cond, canexit2);
            }
            // Prefer the branch that may-exit
            if (canexit1 && !canexit2) {
              stats.loopCanExitKeep++;
              return curr;
            }
            if (!canexit1 && canexit2) {
              stats.loopCanExitSwap++;
              return other;
            }
          }
        }
      }
    }
  }

  return curr;
}

void DumpShortestPathFirstOptStats() {
  if (DumpOptimizationStats) {
    std::cerr << "ShortestPathFirst() Stats"
              << "\n >  attempts:           " << TheShortestPathFirstOpt_Stats.attempts
              << "\n    same-bb:            " << TheShortestPathFirstOpt_Stats.samebb
              << "\n    return-keep:        " << TheShortestPathFirstOpt_Stats.returnKeep
              << "\n    return-swap:        " << TheShortestPathFirstOpt_Stats.returnSwap
              << "\n    unreachable-keep:   " << TheShortestPathFirstOpt_Stats.unreachableKeep
              << "\n    unreachable-swap:   " << TheShortestPathFirstOpt_Stats.unreachableSwap
              << "\n    assertfail-keep:    " << TheShortestPathFirstOpt_Stats.assertfailKeep
              << "\n    assertfail-swap:    " << TheShortestPathFirstOpt_Stats.assertfailSwap
              << "\n    postdom-keep:       " << TheShortestPathFirstOpt_Stats.postdomKeep
              << "\n    postdom-swap:       " << TheShortestPathFirstOpt_Stats.postdomSwap
              << "\n    loop-nest-keep:     " << TheShortestPathFirstOpt_Stats.loopNestKeep
              << "\n    loop-nest-swap:     " << TheShortestPathFirstOpt_Stats.loopNestSwap
              << "\n    loop-advance:       " << TheShortestPathFirstOpt_Stats.loopAdvance
              << "\n    loop-exiting:       " << TheShortestPathFirstOpt_Stats.loopExiting
              << "\n    loop-can-exit-keep: " << TheShortestPathFirstOpt_Stats.loopCanExitKeep
              << "\n    loop-can-exit-swap: " << TheShortestPathFirstOpt_Stats.loopCanExitSwap
              << "\n";
  }
}

//
// Single-threaded early-exit
//

bool TerminateStateIfSingleThreaded(klee::ExecutionState *kstate,
                                    klee::KInstruction *targetKinst) {
  if (!OptNoSingleThreadedScheduling)
    return false;

  if (kstate->threads.size() == 1 && !MayLeadToThreadCreation(kstate->crtThread())) {
    if (DbgSchedulingOptimizations) {
      std::cerr << "=================================================\n";
      std::cerr << "=================================================\n";
      std::cerr << "== Early exit (single threaded) ==\n";
      kstate->dumpStackTrace(std::cerr);
      std::cerr << "=================================================\n";
      std::cerr << "=================================================\n";
    }
    kstate->executor->terminateStateEarly(*kstate, "leaving multithreaded code: halting");
    return true;
  }

  // passing targetKinst is ugly: leftover from when this used
  // to be called during onControlControlFlowEvent and I don't
  // want to change it for fear of breaking something subtle
  if (targetKinst && targetKinst->inst &&
      targetKinst->inst->getParent() &&
      targetKinst->inst->getParent()->getParent() &&
      targetKinst->inst->getParent()->getParent()->getName() == "exit") {
    if (DbgSchedulingOptimizations) {
      std::cerr << "=================================================\n";
      std::cerr << "=================================================\n";
      std::cerr << "== Early exit (calling exit) ==\n";
      kstate->dumpStackTrace(std::cerr);
      std::cerr << "=================================================\n";
      std::cerr << "=================================================\n";
    }
    kstate->executor->terminateStateEarly(*kstate, "leaving multithreaded code (via exit): halting");
    return true;
  }

  return false;
}

}  // namespace

//-----------------------------------------------------------------------------
// DFS branch queue
//-----------------------------------------------------------------------------

void DepthFirstICSQueue::clear() {
  assert(_queue.size() == _set.size());
  while (!_queue.empty()) {
    SymbolicState *state = _queue.front();
    klee::ExecutionState *kstate = state->getKleeState();
    kstate->executor->discardState(kstate);
    // The above should invoke DepthFirstICS::onStateDeactivated(), which
    // should invoke this->removeState().  Yes this is convoluted ...
    assert(_queue.empty() || _queue.front() != state);
    assert(_set.count(state) == 0);
  }
  assert(_set.empty());
}

SymbolicState* DepthFirstICSQueue::head() const {
  assert(!_queue.empty());
  return _queue.front();
}

void DepthFirstICSQueue::addState(SymbolicState *state) {
  assert(!_set.count(state));
  _set.insert(state);
  // Push to the front since more recently added states are usually closer
  // to exiting than are older states (this jives with OptShortestPathFirst).
  _queue.push_front(state);
}

void DepthFirstICSQueue::updateState(SymbolicState *state) {
  assert(state);
  assert(_set.count(state));

  // Check if "state" is cheaper than "head"
  SymbolicState *hd = head();
  if (!OptShortestPathFirst || state == hd)
    return;
  if (PickCheapestNeighboringState(state, hd) == hd)
    return;

  // Swap to head
  QueueTy::iterator it = std::find(_queue.begin(), _queue.end(), state);
  assert(it != _queue.end());
  std::swap(*it, _queue.front());

  // DEBUG
  assert(std::find(_queue.begin(), _queue.end(), hd) != _queue.end());
  assert(head() == state);
}

void DepthFirstICSQueue::removeState(SymbolicState *state) {
  assert(state);
  assert(_set.count(state));
  _set.erase(state);

  QueueTy::iterator it = std::find(_queue.begin(), _queue.end(), state);
  assert(it != _queue.end());
  _queue.erase(it);
}

bool DepthFirstICSQueue::isStateQueued(SymbolicState *state) const {
  return (_set.count(state) > 0);
}

//-----------------------------------------------------------------------------
// A DFS strategy that uses precondition slicing to bias the search space.
//-----------------------------------------------------------------------------

DepthFirstICS::DepthFirstICS(JobManager *jm)
  : _jobManager(jm),
    _currPathManager(NULL),
    _currRoot(NULL),
    _completedRegions(0),
    _nextRegionNumericId(1) // first valid numericId is "1"
{
  _jobManager->getSymbEngine()->registerStateEventHandler(&_logger);
  // We start with the <init> region
  addRegion(_currRegion);
  _currPathManager = getCurrPathManager();
}

void DepthFirstICS::finalize() {
  flushRegionStagingArea();
  std::cerr << "REMAINING REGIONS: " << _regionsFrontier.size() << "\n";
  DumpShortestPathFirstOptStats();
  dumpFinalRegionSummary(true);
  dumpFinalSymbolicObjects();
  if (DoInstrumentor && IcsExecMode == IcsNormal) {
    InstrumentModuleForICS(_jobManager->getKleeExecutor(),
                           _symbolicObjects.get(),
                           _resolvedSymbolicPtrs.get(),
                           _regions);
  }
}

bool DepthFirstICS::isQueuedState(SymbolicState *state) const {
  return !_currQueue.empty() && _currQueue.isStateQueued(state);
}

bool DepthFirstICS::isRootState(SymbolicState *state) const {
  return state == _currRoot;
}

bool DepthFirstICS::isRegionFrontierState(SymbolicState *state) const {
  ParallelRegionId id(state->getKleeState());
  return _regionsFrontier.count(id) || _localRegionsFrontier.count(id);
}

void DepthFirstICS::addRegion(const ParallelRegionId &id) {
  if (!_localRegions.count(id)) {
    ParallelRegionInfoRef &info = _localRegions[id];
    if (_regions.count(id)) {
      info = _regions[id];
    } else {
      info = new ParallelRegionInfo;
      info->numericId = _nextRegionNumericId++;
    }
  }
}

void DepthFirstICS::addRegionEdge(const ParallelRegionId &nextId,
                                  const bool exits, const bool deadlocks) {
  RegionPathEdge edge;
  edge.traceId = _jobManager->getTraceCounter("pathConstraints");
  edge.exits = exits;
  edge.deadlocks = deadlocks;
  if (!exits && !deadlocks) {
    edge.nextRegion = nextId;
    addRegion(nextId);
  }
  addRegion(_currRegion);
  _localRegions[_currRegion]->regionEdges.push_back(edge);
}

void DepthFirstICS::clearRegionStagingArea() {
  klee::Executor *executor = _jobManager->getKleeExecutor();

  // Discard the local staging area
  for (RegionsFrontierTy::iterator
       it = _localRegionsFrontier.begin(); it != _localRegionsFrontier.begin(); ++it) {
    executor->discardState(it->second->getKleeState());
  }

  _localRegions.clear();
  _localRegionsFrontier.clear();

  // Need to reset this in case regions were lost
  _nextRegionNumericId = _regions.size() + 1;
}

void DepthFirstICS::flushRegionStagingArea() {
  CLOUD9_DEBUG("Flushing " << _localRegions.size() << " local region ids");
  CLOUD9_DEBUG("Flushing " << _localRegionsFrontier.size() << " local frontier ids");

  // Move localRegions -> regions
  for (RegionsInfoTy::iterator it = _localRegions.begin(); it != _localRegions.end(); ++it) {
    if (_regions.count(it->first)) {
      assert(_regions[it->first].get() == it->second.get());
    } else {
      _regions[it->first] = it->second;
    }
  }
  _localRegions.clear();

  // Move localFrontier -> frontier
  for (RegionsFrontierTy::iterator
       it = _localRegionsFrontier.begin(); it != _localRegionsFrontier.end(); ++it) {
    assert(!_regionsFrontier.count(it->first));
    _regionsFrontier[it->first] = it->second;
  }
  _localRegionsFrontier.clear();
}

PathManager* DepthFirstICS::getCurrPathManager() {
  addRegion(_currRegion);
  return &_localRegions[_currRegion]->paths;
}

//
// Events
//

void DepthFirstICS::onUpdateAfterBranch(const std::vector<SymbolicState*> states) {
  //CLOUD9_DEBUG("onUpdateAfterBranch(state:" << state << ")");

  // Select the cheapest state
  for (size_t k = 0; k < states.size(); ++k)
    _currQueue.updateState(states[k]);

  // Check for an early exit on the head-of-queue
  assert(_currQueue.head());
  TerminateStateIfSingleThreaded(_currQueue.head()->getKleeState(), NULL);
}

ExecutionJob* DepthFirstICS::onNextJobSelection() {
  //CLOUD9_DEBUG("onNextJobSelection(qsize:" << _currQueue.size() << ")");

  klee::Executor *executor = _jobManager->getKleeExecutor();
  assert(_currPathManager);

  const size_t maxRegions = GetEffectiveMaxRegions();
  const size_t maxPaths = GetMaxPathsForCurrentRegion(_completedRegions);
  const size_t maxConcreteDepth = GetMaxConcretePathDepthForCurrentRegion(_completedRegions);
  const size_t maxSymbolicDepth = GetMaxSymbolicPathDepthForCurrentRegion(_completedRegions);

  // Context-switch at branches if requested
  if (EnableCtxSwitchOnBranches && !_currQueue.empty()) {
    SymbolicState *state = _currQueue.head();
    klee::ExecutionState *kstate = state->getKleeState();
    // Don't context-switch when executing in klee model functions
    // (to ensure atomicity in klee model functions).
    const llvm::Instruction *currI = kstate->prevPC()->inst;
    if (!kstate->partialEvalMode && (isa<BranchInst>(currI) || isa<SwitchInst>(currI))) {
      // Can't have a pthread fn on the stack
      bool canSched = true;
      for (size_t k = 0; k < kstate->stack().size(); ++k) {
        if (kstate->stack()[k].kf->function->getName().find("pthread") != llvm::StringRef::npos) {
          canSched = false;
          break;
        }
      }
      // Top function can't be a klee model
      if (currI->getParent()->getParent()->getName().startswith("__klee")) {
        canSched = false;
      }
      if (canSched) {
        assert(kstate->enabledCount());
        executor->schedule(*kstate, true);
      }
    }
  }

  // Limit the depth of a path if requested
  if ((maxConcreteDepth > 0 || maxSymbolicDepth > 0) && !_currQueue.empty()) {
    SymbolicState *state = _currQueue.head();
    klee::ExecutionState *kstate = state->getKleeState();
    if (!kstate->partialEvalMode) {
      if (maxSymbolicDepth > 0 && kstate->depth >= maxSymbolicDepth) {
        CLOUD9_DEBUG("Ending path early: symbolic depth = " << kstate->depth);
        processPath(state, false);
        executor->discardState(kstate);
        _currQueue.clear();
      } else if (maxConcreteDepth > 0 && kstate->totalBranches >= maxConcreteDepth) {
        CLOUD9_DEBUG("Ending path early: concrete depth = " << kstate->totalBranches);
        processPath(state, false);
        executor->discardState(kstate);
        _currQueue.clear();
      }
    }
  }

  // Limit the number of paths-per-region if requested
  if (maxPaths > 0 && _currPathManager->numExplored() == maxPaths) {
    CLOUD9_DEBUG("Ending region early: "
                 << _currPathManager->numFrontier() << " remaining on the frontier");
    _currPathManager->clearFrontier();
    _currQueue.clear();
  }

  // Need to upgrade some loops?
  // This implies that we need to restart the region
  if (!_loopsToUpgrade.empty()) {
    upgradeUncoveredLoops();
    clearRegionStagingArea();
    _currPathManager = getCurrPathManager();
    _currPathManager->resetToEmpty();
    _currQueue.clear();
    forkNextPath();
  }

  // Done with the current region?
  if (_currQueue.empty() && _currPathManager->isFrontierEmpty()) {
    flushRegionStagingArea();
    CLOUD9_DEBUG("Done with parallel region (" << _regionsFrontier.size() << " more regions)");
    if (DbgSymbolicPathsVerbose)
      std::cerr << ParallelRegionIdPrinter(_currRegion, "  ");

    assert(_currRoot);
    _completedRegions++;
    _currPathManager->dumpStats(_jobManager);

    // Clear old state
    klee::ExecutionState *tmp = _currRoot->getKleeState();
    _currRoot = NULL;
    executor->discardState(tmp);
    _currPathManager->clearCaches();

    // Done with everything?
    if (_regionsFrontier.empty()) {
      dumpSymbolicTree(NULL, NULL, "workertreeFINAL");
      CLOUD9_DEBUG("Done with all regions");
      return NULL;
    }
    if (maxRegions > 0 && _completedRegions >= maxRegions) {
      dumpSymbolicTree(NULL, NULL, "workertreeFINAL");
      CLOUD9_DEBUG("Stopping early with " << _regionsFrontier.size() << " regions remaining");
      return NULL;
    }

    // Next region
    RegionsFrontierTy::iterator it = _regionsFrontier.begin();
    _currRegion = it->first;
    _currRoot = it->second;
    _currPathManager = getCurrPathManager();
    _regionsFrontier.erase(it);

    // N.B.: We don't clone the root until after partial eval.
    // However, this must go in the queue now so we have a valid state to return!
    _currQueue.addState(_currRoot);

    dumpFinalRegionSummary(false);
    std::cerr << "=====================================================\n"
              << "Starting next parallel region (state:" << _currRoot
              << ", remaining: " << _regionsFrontier.size()
              << ", totalfound: " << _regions.size() << ")\n"
              << ParallelRegionIdPrinter(_currRegion, "  ")
              << "=====================================================\n";

    InitializeStateForParallelRegion(_currRoot->getKleeState());
  }

  // Done with the current DFS?
  if (_currQueue.empty()) {
    assert(_currRoot);
    forkNextPath();

    // Setup the initial input constraints
    klee::ExecutionState *kstate = _currQueue.head()->getKleeState();
    _currPathManager->instantiateNextInput(kstate, _jobManager);
  }

  assert(!_currQueue.empty());
  assert(_currQueue.head());

  WorkerTree::Node *node = _currQueue.head()->getNode().get();
  assert(node);

  // DEBUG
  if (!node->layerExists(WORKER_LAYER_JOBS)) {
    dumpSymbolicTree(NULL, node, "workertreeFAIL");
    assert(false);
  }

  ExecutionJob *job = (**node).getJob();
  assert(job);
  return job;
}

void DepthFirstICS::onStateActivated(SymbolicState *state) {
  //CLOUD9_DEBUG("onStateActivated(state:" << state << ")");
  assert(state);

  // Special case for the very first state
  if (!_currRoot) {
    _currRoot = state;

    assert(_currRegion.contexts.empty());
    assert(_currQueue.empty());
    assert(_currPathManager->isFrontierEmpty());
    assert(_currPathManager->numExplored() == 0);
    assert(_regionsFrontier.empty());
    assert(_regions.empty());
    assert(_localRegions.size() == 1);
    assert(_localRegionsFrontier.empty());
    assert(_resolvedSymbolicPtrs.get() == NULL);
    assert(_symbolicObjects.get() == NULL);

    _resolvedSymbolicPtrs.reset(
        new klee::SymbolicPtrResolutionTable(state->getKleeState()->resolvedSymbolicPtrs));
    _symbolicObjects.reset(
        new klee::SymbolicObjectTable(state->getKleeState()->symbolicObjects));
  }

  _currQueue.addState(state);
}

void DepthFirstICS::onStateDeactivated(SymbolicState *state) {
  //CLOUD9_DEBUG("onStateDeactivated(state:" << state << ")");

  klee::ExecutionState *kstate = state->getKleeState();

  // States should not be destroyed during partial eval
  if (kstate->partialEvalMode) {
    CLOUD9_DEBUG("onStateDeactivated(state:" << state << ") :: discarding from partial eval!");
    assert(0);
  }

  // The root state should never be destroyed
  if (state == _currRoot) {
    CLOUD9_DEBUG("onStateDeactivated(state:" << state << ") :: discarding root state!");
    assert(0);
  }

  // Discard memory errors, as their behavior is "undefined" anyway
  if (kstate->finalErrorCode == "ptr.err") {
    llvm::errs() << "WARNING: Discarding state with a memory error!\n";
    kstate->discarded = true;
  }

  // Ignore discarded states
  if (kstate->discarded) {
    CLOUD9_DEBUG("onStateDeactivated(state:" << state << ") :: ignoring discarded state!");
    if (DbgSymbolicPathsVerbose) {
      std::cerr << "=====================================================\n"
                << "Discarding path (state:" << state << ")\n";
      state->getKleeState()->dumpStackTrace(std::cerr);
      std::cerr << "=====================================================\n";
    }
    if (_currQueue.isStateQueued(state))
      _currQueue.removeState(state);
    return;
  }

  // Process this end-of-path
  assert(state != _currRoot);
  processPath(state, true);
}

void DepthFirstICS::onIcsBegin(SymbolicState *state) {
  CLOUD9_DEBUG("onIcsBegin(state:" << state << ")");

  klee::ExecutionState *kstate = state->getKleeState();
  const bool isInitialRegion = ((_regions.size() + _localRegions.size()) == 1);
  if (isInitialRegion)
    assert(_regions.empty());

  // This is called at the beginning of a new region, either explicitly by
  // the program (after setting up input constraints), or implicitly after
  // partial eval has completed.  We clear out state generated by that
  // initial region harness, and then clone the root to begin execution.
  assert(_currRoot);
  assert(_currRoot == state);
  assert(_currPathManager->isFrontierEmpty());
  assert(_currPathManager->numExplored() == 0);

  // Remove the root state from the queue
  assert(_currQueue.head() == _currRoot);
  _currQueue.removeState(_currRoot);

  // FIXME: A bit of unfortunate ugliness ...
  // If the initial code (at program entry) calls any symbolic models, it
  // may read and branch on symbolic input.  This actually happens often,
  // as the initial code often calls printf(), which invokes write().  In
  // this case, we may have forked multiple paths before klee_ics_begin().
  // FIXME: Perhaps it's cleaner to get rid of klee_ics_begin() and instead
  // use the usual dynamic-input scheme to select the initial schedule?
  if (!_currQueue.empty()) {
    assert(isInitialRegion);
    _currQueue.clear();
  }

  // There are two cases here:
  // (a) We are starting the first "real" region; i.e., this is the first call to
  //     klee_ics_begin().  In this case, we save the schedule from program entry
  //     up to now in the "initial region".  This schedule includes a sequence of
  //     program points that read input, and nothing else, since it is an error to
  //     create threads or branch on symbolic input before calling klee_ics_begin().
  // (b) We are starting a later region.  In this case, we have constructed an
  //     initial context with partial eval, and the schedule should be empty.
  if (isInitialRegion) {
    assert(kstate->threads.size() == 1);
    assert(_regionsFrontier.empty());
    assert(_localRegionsFrontier.empty());
    // Debugging
    if (DumpHbGraphs) {
      std::auto_ptr<std::ostream>
        os(_jobManager->openSummaryOutputFile("initialHbgraph.dot", "InitialHBGraph"));
      kstate->hbGraph.dumpGraph(*os);
    }
    // IMPORTANT: In the initial region, the HbGraph should NOT include any cross-thread
    // edges: this means we can collapse the HbGraph to include just the nodes which
    // read input, plus a final "done" node that we add below.
    for (hbnode_id_t n = 1; n <= kstate->hbGraph.getMaxNodeId(); ++n) {
      const HBNode &node = kstate->hbGraph.getNode(n);
      assert(node.tuid() == kstate->crtThreadUid());
    }
    // Add an HBNode for this event
    kstate->hbGraph.addNodeForThread(&kstate->crtThread());
  } else {
    assert(kstate->hbGraph.empty());
  }

  // Ensure an enabled thread is scheduled
  // Do this *before* initializing the harness trace
  kstate->executor->schedule(*kstate, false);

  // We save the set of assumptions generated by the harness.
  // These hold for all paths in this region.
  kstate->inputHarnessConstraints = kstate->globalConstraints;

  // We clear the trace since we want to compute a precond slice
  // relative to *this* root state (and not all the branches taken
  // to setup the region).  We save this trace so later we can dump
  // the set of assumptions generated by the harness.
  assert(kstate->inputHarnessTrace.empty());
  kstate->precondSliceTrace.swap(kstate->inputHarnessTrace);
  dumpConstraintsFromPrecondSlice(kstate->inputHarnessTrace, "regionHarnessConstraints", true);

  if (IcsExecMode != IcsExecFromMiddleViaConcretePath) {
    // All global objects get copied to the initialHarness list.
    // This is the list of objects that symbolic pointers can alias.
    for (klee::ExecutionState::processes_ty::iterator
         p = kstate->processes.begin(); p != kstate->processes.end(); ++p) {
      klee::MemoryMap &objects = p->second.addressSpace.objects;
      for (klee::MemoryMap::iterator mm = objects.begin(); mm != objects.end(); ++mm) {
        // XXX: take just the globals (partial eval adds the locals via executeAlloc)
        // XXX: BUT: take all for the first region (where we didn't do partial eval)
        // XXX: should we instead just do this BEFORE partial eval???
        if (isInitialRegion || mm->first->isGlobalOrThreadLocal()) {
          kstate->inputHarnessObjects.push_back(mm->first);
        }
      }
    }
  }

  // Reset branching stats
  kstate->depth = 0;
  kstate->totalBranches = 0;

  // Reset PathManager stats
  _currPathManager->resetStatsForNewPath();

  // Clone for the first path
  forkNextPath();
}

// FIXME: should pass more detailed info about the source of the region
// marker instead of just the "force" flag, which is opaque.  Currently,
// we assume "force"=>"barrier"

void DepthFirstICS::onIcsMaybeEndRegion(SymbolicState *state,
                                        klee::ThreadUid tuid,
                                        const bool force) {
  assert(!_currQueue.empty());
  assert(_currQueue.head() == state);

  klee::ExecutionState *kstate = state->getKleeState();
  klee::ExecutionState::threads_ty::iterator currIt = kstate->crtThreadIt;
  klee::ExecutionState::threads_ty::iterator thrIt = kstate->threads.find(tuid);
  assert(thrIt != kstate->threads.end());

  // The region marker applies to this thread
  klee::Thread &thread = thrIt->second;

  // When doing an "exec-from-middle" experiment, skip all region markers
  // once we've started the second region.
  if (IsIcsModeExecFromMiddle() && _completedRegions >= 1) {
    std::cerr << "SKIPPING REGION MARKER: EXPERIMENT IS RUNNING\n";
    return;
  }

  // Skip dormant markers
  if (!force && kstate->crtThreadUid() == tuid) {
    llvm::Instruction *curr = kstate->prevPC()->inst;
    if (DormantRegionMarkers.count(curr))
      return;
  }

  CLOUD9_DEBUG("onIcsMaybeEndRegion(state:" << state << " thread" << tuid << ")");

  //
  // We check if we should end the region for the thread named by "tuid".
  // There are two cases in which this is called:
  //
  // (1) "tuid" is enabled and hit a region marker, either via
  //     klee_ics_region_marker() or klee_thread_notify().  In this case,
  //     if the region ends, we move "tuid" to the ICS region waiting set.
  //
  // (2) "tuid" is disabled, all other threads are disabled, and at least
  //     one other thread is waiting on a region marker.  This case happens
  //     when all threads are asleep -- see call from Executor::schedule().
  //     In this case, the region must end NOW to avoid deadlock.
  //

  // All threads disabled => the region must end NOW
  if (kstate->enabledCount() == 0)
    goto EndRegion;

  // If the thread was enabled and this was NOT a notification, emit an
  // HbNode for this thread.  For region markers in klee_thread_sleep()
  // and klee_thread_notify(), our runtime system know how to end the region.
  // N.B.: We check kstate->prevPC rather than thread.prevPC because this
  // could be the *notified* thread, not just the notifier.
  if (thread.isEnabled()) {
    bool isNotify = false;
    if (const CallInst *call = dyn_cast<CallInst>(kstate->prevPC()->inst)) {
      const Value *callee = call->getCalledValue()->stripPointerCasts();
      if (isa<Function>(callee) && callee->getName() == "klee_thread_notify")
        isNotify = true;
    }
    if (!isNotify) {
      kstate->hbGraph.addNodeForThread(&thread);
    }
  }

  // Barrier-synchronized optimization:
  // * don't end the region until we reach a barrier
  if (OptAssumeRegionsEndInBarriers && !force) {
    llvm::errs() << "Skipping region marker: waiting for barrier\n";
    return;
  }

  // Stright-line-code optimization:
  // * don't end the region unless we've crossed a loop-region boundary
  // * N.B.: we ignore "force" here so we can run through straight-line barriers
  if (OptRunRegionsWhileAcyclic && !kstate->crossedIcsLoopedRegion[tuid]) {
    llvm::errs() << "Skipping region marker: no symbolic backedge yet\n";
    return;
  }

  // Skip markers if we haven't reached out minimum bounds yet
  if (!force && kstate->perThreadTime[tuid] < MinTimeBetweenRegionMarkers) {
    llvm::errs() << "Skipping region marker: "
                 << kstate->perThreadTime[tuid] << " < " << MinTimeBetweenRegionMarkers << "\n";
    return;
  }

  // We now end the region for the thread named by "tuid".
EndRegion:
  if (kstate->enabledCount() != 0) {
    std::cerr << "ENDING REGION FOR THREAD: " << thread.getTid() << "\n";
    assert(thread.isEnabled());

    // Add a RegionMarker event to the trace
    // Careful: temporarily schedule the specified thread so the appropriate
    // context-switch events are generated.
    // TODO: this pc = prevPC hack is super ugly, but necessary to ensure we
    // generate context-switch events with the right pc
    const klee::KInstIterator hackThrPC = thrIt->second.pc;
    const klee::KInstIterator hackCurPC = currIt->second.pc;
    thrIt->second.pc = thrIt->second.prevPC;
    currIt->second.pc = currIt->second.prevPC;

    kstate->scheduleNext(thrIt);
    kstate->precondSliceTrace.pushRegionMarker(kstate, tuid);
    kstate->scheduleNext(currIt);

    currIt->second.pc = hackCurPC;
    thrIt->second.pc = hackThrPC;

    // This thread's region just ended
    kstate->sleepThreadForEndOfIcsRegion(tuid);
  }

  assert(!kstate->waitingAtIcsRegion.empty());

  // Did the parallel region end?
  // I.e., did all threads end their region?
  if (kstate->enabledCount() == 0) {
    ParallelRegionId id(kstate);
    CLOUD9_DEBUG("Ending parallel region (state:" << state << ")");
    if (DbgSymbolicPathsVerbose)
      std::cerr << ParallelRegionIdPrinter(id, "  ");

    // Time stats
    assert(_localRegions.count(_currRegion));
    ParallelRegionInfo &currInfo = *_localRegions[_currRegion];
    size_t maxTime = 0;
    size_t maxTimeRunning = 0;
    for (klee::ExecutionState::threads_ty::iterator
         t = kstate->threads.begin(); t != kstate->threads.end(); ++t) {
      const size_t mytime = kstate->perThreadTime[t->first];
      maxTime = std::max(maxTime, mytime);
      if (!t->second.isOnWaitingList())
        maxTimeRunning = std::max(maxTimeRunning, mytime);
    }
    for (klee::ExecutionState::threads_ty::iterator
         t = kstate->threads.begin(); t != kstate->threads.end(); ++t) {
      const size_t mytime = kstate->perThreadTime[t->first];
      currInfo.perThreadTime.addSample(mytime);
      currInfo.perThreadImbalance.addSample(maxTime - mytime);
      if (!t->second.isOnWaitingList())
        currInfo.perThreadImbalanceNoSleepers.addSample(maxTimeRunning - mytime);
    }

    // Is this a new region?
    // Must check BEFORE calling processPath() as that will insert the new region edge
    const bool isNew = (_regions.count(id) == 0) && (_localRegions.count(id) == 0);

    // Build and process the precond slice
    processPath(state, false);

    // Discard dups
    if (!isNew) {
      CLOUD9_DEBUG("Found dup parallel region (discarding state:" << state << ")");
      _jobManager->getKleeExecutor()->discardState(kstate);
    } else {
      CLOUD9_DEBUG("Adding parallel region to frontier (state:" << state << ")");
      assert(!_regionsFrontier.count(id));
      assert(!_localRegionsFrontier.count(id));
      assert(_localRegions.count(id));
      _localRegionsFrontier.insert(std::make_pair(id, state));
    }
  }
}

//
// Processing
//

// TODO: What do we do on solver failure?
// Klee calls terminateStateEarly() in some cases of solver failure (in both
// fork() and executeMemoryOperation()).  Should we handle these paths?  How
// do we report to the user (so I don't miss it)?  Should we instead call
// terminateStateOnError()?

// TODO: Is the following still an issue with lazy ptr resolution?
// TODO: Need to handle case where we segfault on a null pointer generated via a
//  symbolic ptr fork.  To check for this case, we do a dependence analysis.  If
//  we find this case, we should pick *just one* branch of the symbolic ptr fork
//  and try that (i.e., try either "p = new" or "p = old").  Trying one of these
//  will tell us if we need to try all (otherwise, we'd always try all even for
//  ptrs which are not important, just because that ptr happened to be null and
//  trigger a deref).
//
//  Similar issue for many memory errors:
//  -- Deref of null (=> pick any non-null alternative)
//  -- Write of readonly obj (=> pick any non-readonly alternative)
//  -- Free of alloca (=> pick any non-local,non-global alternative)
//  -- Free of global (=> pick any non-local,non-global alternative)
//
//  Could handle this by:
//   1) Marking the ptr as a data dependence when initializing the slice algorithm
//   2) If we spot this special dependence at a symbolic ptr fork, do special handling as above
//   3) Modify computation to return a set of states to add to the frontier in addition to the
//      slice (this way we don't need to pass convoluted info to filterStatesFromPrecondSlice)

void DepthFirstICS::processPath(SymbolicState *state, const bool exited) {
  klee::ExecutionState *kstate = state->getKleeState();

  // DEBUG
  std::cerr << "ICS: Finished a path!\n";
  kstate->precondSliceTrace.sanityCheckTrace();

  if (DbgSymbolicPaths) {
    std::cerr << "=====================================================\n"
              << "Completed path (state:" << state << ")\n";
    state->getKleeState()->dumpStackTrace(std::cerr);
    std::cerr << "=====================================================\n";
  }

  if (DumpExecutionTraces) {
    kstate->inputHarnessTrace.dumpTrace(_jobManager, "inputHarnessTrace");
    kstate->precondSliceTrace.dumpTrace(_jobManager, "exectrace");
  }

  if (DumpHbGraphs) {
    std::auto_ptr<std::ostream> os(_jobManager->openTraceOutputFile("hbgraph", "dot", "HB Graph"));
    kstate->hbGraph.dumpGraph(*os);
  }

  if (DumpSymbolicPtrGraphs)
    dumpSymbolicPtrGraph(kstate, kstate->precondSliceTrace, "ptrgraph", "ptrgraphDetailed");

  if (DumpSymbolicTrees)
    dumpSymbolicTree(NULL, state->getNode().get(), "workertree");

  // Compute precond slice of the path
  std::auto_ptr<PrecondSliceTrace> slice;
  if (DoPrecondSlice) {
    slice.reset(new PrecondSliceTrace);
    ComputePrecondSlice(_jobManager->getKleeExecutor(), kstate, this, &*slice);
    dumpConstraintsFromPrecondSlice(*slice, "sliceConstraints");
  } else {
    slice.reset(new PrecondSliceTrace(kstate->precondSliceTrace));
  }

  if (DumpSliceTraces)
    slice->dumpTrace(_jobManager, "precondslice");

  if (DumpSymbolicPtrGraphs)
    dumpSymbolicPtrGraph(kstate, *slice, "ptrgraphFiltered", "ptrgraphDetailedFiltered");

  // Process the path constraints
  // This will updated "explored" and "frontier" sets
  CLOUD9_INFO("Processing the slice");
  _currPathManager->processSlice(kstate, *slice, _jobManager, exited);

  // Add a new edge for this region
  CLOUD9_INFO("Processing the slice finished");
  addRegionEdge(ParallelRegionId(kstate), exited, kstate->finalErrorCode == "deadlock.err");

  // Done with this path
  // Discard all remaining branches
  _currQueue.removeState(state);
  _currQueue.clear();
}

void DepthFirstICS::forkNextPath() {
  klee::Executor *executor = _jobManager->getKleeExecutor();
  assert(_currPathManager);

  executor->fork(*_currRoot->getKleeState(), klee::KLEE_FORK_INTERNAL);
  assert(_currQueue.size() == 1);
  assert(_currQueue.head());
  assert(_currQueue.head() != _currRoot);
}

void DepthFirstICS::shouldUpgradeUncoveredLoops(const LoopSetTy &loops) {
  if (DoLazyRegionCovering) {
    _loopsToUpgrade.insert(loops.begin(), loops.end());
  }
}

void DepthFirstICS::upgradeUncoveredLoops() {
  while (!_loopsToUpgrade.empty()) {
    const llvm::Loop *loop = *_loopsToUpgrade.begin();
    assert(UncoveredLoops.count(loop));

    llvm::Instruction *marker = UncoveredLoops[loop];
    assert(DormantRegionMarkers.count(marker));

    // DEBUG
    CLOUD9_INFO("Activating region marker in uncovered loop");
    const llvm::BasicBlock *bb = loop->getHeader();
    llvm::errs() << "LOOP TO ACTIVATE:\n"
                 << "  Func: " << bb->getParent()->getName() << "\n"
                 << "  Loop: " << bb->getName() << "\n"
                 << "  Marker: " << *marker << "\n";

    // Remove from the dormant lists
    UncoveredLoops.erase(loop);
    DormantRegionMarkers.erase(marker);
    _loopsToUpgrade.erase(loop);

    // Move to the active list
    CoveredLoops.insert(loop);
  }
}

//
// Output
//

namespace {
  template<class T>
  std::string llvmValueToString(const T /* llvm::Value or llvm::Type */ *value) {
    assert(value);
    std::string str;
    llvm::raw_string_ostream strstr(str);
    strstr << *value;
    str.erase(std::remove(str.begin(), str.end(), '\n'), str.end());
    return str;
  }
}

namespace SymbolicPtrGraphTypes {
  struct Object {
    typedef std::pair<ref<Expr>, const llvm::Instruction*> KeyTy;

    ref<Expr> baseAddress;
    const klee::SymbolicObject *baseObj;
    std::string name;
    size_t id;

    struct Target {
      ref<Expr> address;
      Object *object;
    };
    typedef llvm::SmallVector<Target, 4> TargetsTy;

    struct Field {
      ref<Expr> offset;
      TargetsTy targets;
    };
    typedef llvm::SmallVector<Field, 4> FieldsTy;

    FieldsTy fields;
    const llvm::Value *targetPointer;

    /////

    Object()
      : baseObj(NULL), id(0), targetPointer(NULL) {}

    /////

    static KeyTy Key(const klee::SymbolicObject *obj, ref<Expr> targetAddress) {
      if (!obj) {
        assert(targetAddress.get());
        return KeyTy(targetAddress, NULL);
      } else {
        assert(obj->mo || obj->inst);
        return obj->mo ? KeyTy(obj->mo->getBaseExpr(), NULL)
                       : KeyTy(Expr::Null, obj->inst);
      }
    }
  };

  bool TargetLess(const Object::Target &a, const Object::Target &b) {
    assert(a.address.get());
    assert(b.address.get());
    // Null-ptrs before Obj-ptrs
    if (!a.object && b.object) return true;
    if (a.object && !b.object) return false;
    // Sort by Obj or Addr
    if (a.object && b.object)
      return a.object->id < b.object->id;
    else
      return a.address < b.address;
  }
}

void DepthFirstICS::dumpSymbolicPtrGraph(klee::ExecutionState *kstate,
                                         const PrecondSliceTrace &slice,
                                         const char *fileprefixDot,
                                         const char *fileprefixTxt) {
  using namespace SymbolicPtrGraphTypes;
  using klee::SymbolicPtrResolution;

  //
  // Expand to concrete ptr resolutions
  // Each SymbolicPtrResolution struct represents an abstract ptr value,
  // so we ask the solver to produce all possible concrete targets of the ptr.
  //
  std::vector<ref<SymbolicPtrResolution> > resolutions;
  std::set<const klee::MemoryObject*> srcObjectsToSkip;

  for (PrecondSliceTrace::const_iterator e = slice.begin(); e != slice.end(); ++e) {
    if (e->kind() != PrecondSliceTrace::ResolvedSymbolicPtr)
      continue;

    const SymbolicPtrResolution *res = e->ptrResolution();
    assert(res->pointer);

    // Skip?
    if (res->uniqueSourceObj && srcObjectsToSkip.count(res->uniqueSourceObj->mo)) {
      if (res->targetObj)
        srcObjectsToSkip.insert(res->targetObj->mo);
      continue;
    }

    // Enumerate all possible targets
    klee::AddressSpace::ResolveOptions resolveOptions;
    resolveOptions.staticPtr = res->pointer;
    resolveOptions.accessBytes = Expr::Null;
    resolveOptions.maxResolutions = 0;
    resolveOptions.timeout = klee::clvars::MaxSTPTime;
    resolveOptions.ignoreAliases = false;

    klee::GuardedResolutionList rl;
    klee::TimingSolver *solver = _jobManager->getKleeExecutor()->getTimingSolver();
    klee::AddressSpace &addrSpace = kstate->addressSpace();
    if (res->isFunctionPtr()) {
      addrSpace.resolveFunctionPointer(*kstate, solver, res->targetAddress, &rl, resolveOptions);
    } else {
      addrSpace.resolve(*kstate, solver, res->targetAddress, &rl, resolveOptions);
    }

    for (klee::GuardedResolutionList::iterator r = rl.begin(); r != rl.end(); ++r) {
      const klee::ObjectPair &op = r->targetObj;

      // The resolution list ALWAYS includes each ptr's primary object, but here,
      // we want to eliminate primaries where possible to produce the "minimum"
      // object graph.  Example: if a constraint says that (pa == pb), then we
      // should include just one object: namely, the primary for either pa or pb.
      if (op.first) {
        ref<Expr> mayAlias = klee::EqExpr::create(r->ptr, op.first->getBaseExpr());
        bool truth;
        if (solver->mayBeTrue(*kstate, mayAlias, truth) && !truth) {
          srcObjectsToSkip.insert(op.first);
          continue;
        }
      }

      // New resolution
      ref<SymbolicPtrResolution> ptr(new SymbolicPtrResolution);
      resolutions.push_back(ptr);

      ptr->pointer = res->pointer;
      if (op.first) {
        ptr->targetAddress = klee::AddExpr::create(op.first->getBaseExpr(), r->targetOffset);
        ptr->targetObj = kstate->symbolicObjects.find(op.first);
      } else {
        ptr->targetAddress = Expr::createPointer((uint64_t)r->targetFn);
      }
      ptr->uniqueSourceOffset = res->uniqueSourceOffset;
      ptr->uniqueSourceObj = res->uniqueSourceObj;
    }
  }

  //
  // Construct a graph of pointer resolutions.  Use convoluted double-indirection
  // (key -> id -> object) so that objects are nicely sorted by id, so it's easy
  // to output objects in a deterministic order.
  //
  typedef std::map<size_t, Object> ObjectMapTy;
  typedef std::map<Object::KeyTy, size_t> IdMapTy;

  ObjectMapTy objects;
  IdMapTy ids;
  size_t count = 0;

  // Collect mappings for each resolution
  for (std::vector<ref<SymbolicPtrResolution> >::iterator
       it = resolutions.begin(); it != resolutions.end(); ++it) {
    const SymbolicPtrResolution *res = it->get();

    Object::KeyTy key = Object::Key(res->uniqueSourceObj, res->targetAddress);

    // New object?
    if (ids.count(key) == 0) {
      ids[key] = ++count;
    }

    Object *object = &objects[ids[key]];
    if (object->id == 0) {
      object->id = ids[key];
      object->baseAddress = res->targetAddress;
      object->baseObj = res->uniqueSourceObj;
      object->targetPointer = res->pointer;
    }

    if (!res->uniqueSourceObj || !res->uniqueSourceObj->mo)
      continue;

    // Locate the field specified by the offset
    Object::Field *field = NULL;
    for (Object::FieldsTy::iterator
         f = object->fields.begin(); f != object->fields.end(); ++f) {
      if (f->offset == res->uniqueSourceOffset) {
        field = &*f;
        break;
      }
    }
    if (!field) {
      object->fields.push_back(Object::Field());
      field = &object->fields.back();
      field->offset = res->uniqueSourceOffset;
    }

    // Add a new target for this field
    field->targets.push_back(Object::Target());

    Object::Target *target = &field->targets.back();
    target->address = res->targetAddress;

    if (res->targetObj) {
      Object::KeyTy targetKey = Object::Key(res->targetObj, NULL);
      if (ids.count(targetKey) == 0) {
        ids[targetKey] = ++count;
      }

      target->object = &objects[ids[targetKey]];
      if (target->object->id == 0) {
        target->object->id = ids[targetKey];
        target->object->baseAddress = res->targetAddress;
        target->object->baseObj = res->targetObj;
      }
    }
  }

  // Sort targets
  for (ObjectMapTy::iterator it = objects.begin(); it != objects.end(); ++it) {
    Object &object = it->second;
    for (Object::FieldsTy::iterator
         field = object.fields.begin(); field != object.fields.end(); ++field) {
      std::sort(field->targets.begin(), field->targets.end(), TargetLess);
    }
  }

  // Lookup names
  for (ObjectMapTy::iterator it = objects.begin(); it != objects.end(); ++it) {
    Object &object = it->second;
    object.name = object.baseObj->array->name;
  }

  // Dump
  std::auto_ptr<std::ostream>
    dot(_jobManager->openTraceOutputFile(fileprefixDot, "dot", "symbolic ptr dot-graph"));
  std::auto_ptr<std::ostream>
    txt(_jobManager->openTraceOutputFile(fileprefixTxt, "txt", "symbolic ptr txt-graph"));

  (*dot) << "digraph PtrGraph {\n";

  for (ObjectMapTy::iterator it = objects.begin(); it != objects.end(); ++it) {
    const size_t id = it->first;
    const Object &object = it->second;

    // TXT: Node info
    if (object.baseObj && object.baseObj->mo) {
      if (id != 0) {
        (*txt) << "======= Heap Object #" << id << " =======\n";
      } else {
        (*txt) << "======= Heap Object #??? =======\n";
      }
      const klee::MemoryObject *mo = object.baseObj->mo;
      (*txt) << "Size: " << mo->size << "\n";
      (*txt) << "Name: " << object.name << "\n";

      (*txt) << "Type:";
      if (mo->isLocal())
        (*txt) << " local";
      if (mo->isGlobal())
        (*txt) << " global";
      if (mo->isThreadLocal())
        (*txt) << " threadlocal";
      if (mo->isHeap())
        (*txt) << " heap";
      if (mo->isFixedAddr())
        (*txt) << " fixed";
      (*txt) << "\n";

      if (mo->allocSite) {
        (*txt) << "Alloc: " << llvmValueToString(mo->allocSite) << "\n";
      }
    } else if (object.baseObj) {
      (*txt) << "======= Register #" << id << " =======\n";
      (*txt) << "Name: " << llvmValueToString(object.baseObj->getProgramValue()) << "\n";
    } else {
      (*txt) << "======= Heap Object #" << id << " =======\n";
      (*txt) << "Name: ???\n";
      (*txt) << "BaseAddr: " << object.baseAddress << "\n";
    }

    // TXT: Node links
    for (Object::FieldsTy::const_iterator
         field = object.fields.begin(); field != object.fields.end(); ++field) {
      (*txt) << "--Field--\n";
      if (field->offset.get()) {
        (*txt) << "Offset: " << field->offset << "\n";
      } else {
        (*txt) << "Offset: ???\n";
      }
      for (size_t k = 0; k < field->targets.size(); ++k) {
        const Object::Target &target = field->targets[k];
        (*txt) << "Target[" << k << "]: ";
        if (target.object) {
          assert(target.object->baseObj->mo);
          (*txt) << target.object->id << "  (size=" << target.object->baseObj->mo->size << ")\n";
          continue;
        }
        ref<klee::ConstantExpr> addrExpr = dyn_cast<klee::ConstantExpr>(target.address);
        if (addrExpr.get()) {
          const uint64_t addr = addrExpr->getZExtValue();
          if (addr == 0) {
            (*txt) << "Null\n";
          } else if (_jobManager->getKleeExecutor()->isLegalFunction(addr)) {
            // XXX: ugly!
            const llvm::Function *fn = reinterpret_cast<const llvm::Function*>(addr);
            (*txt) << "@" << fn->getNameStr() << "\n";
          } else {
            (*txt) << "??? (0x" << addr << ")\n";
          }
        } else {
          assert(false);
          (*txt) << "??? (expr:" << target.address << ")\n";
        }
      }
    }

    (*txt) << "\n";

    // DOT: Node info
    if (object.baseObj && object.baseObj->mo) {
      const klee::MemoryObject *mo = object.baseObj->mo;
      (*dot) << "  n" << id << " [label=\"";
      if (mo->allocSite) {
        (*dot) << "(" << object.name << ") " << llvmValueToString(mo->allocSite);
      } else {
        (*dot) << "(" << object.name << ") ???";
      }
      (*dot) << "|{";
      for (size_t k = 0; k < object.fields.size(); ++k) {
        if (k != 0) {
          (*dot) << "|";
        }
        (*dot) << "<f" << k << "> ";
        if (isa<klee::ConstantExpr>(object.fields[k].offset)) {
          (*dot) << object.fields[k].offset;
        } else {
          (*dot) << "?";
        }
      }
      (*dot) << "}\",shape=record];\n";
    } else if (object.baseObj) {
      (*dot) << "  r" << id << " [label=\"" << object.baseObj->getProgramValue()->getNameStr()
             << "\",shape=oval];\n";
    } else {
      (*dot) << "  r" << id << " [label=\"???\",shape=oval];\n";
    }

    // DOT: Node links
    for (size_t k = 0; k < object.fields.size(); ++k) {
      const Object::Field &field = object.fields[k];
      for (Object::TargetsTy::const_iterator
           target = field.targets.begin(); target != field.targets.end(); ++target) {
        if (target->object) {
          if (object.baseObj) {
            (*dot) << "  n" << id << ":f" << k << " -> n" << target->object->id << ";\n";
          } else {
            (*dot) << "  r" << id << " -> n" << target->object->id << ";\n";
          }
        } else {
          ++count;
          std::ostringstream nodename;
          std::ostringstream nodelabel;
          assert(isa<klee::ConstantExpr>(target->address));
          uint64_t addr = cast<klee::ConstantExpr>(target->address)->getZExtValue();
          // Determine name/label
          if (addr == 0) {
            nodename << "null" << count;
            nodelabel << "null";
          } else if (_jobManager->getKleeExecutor()->isLegalFunction(addr)) {
            const llvm::Function *fn = reinterpret_cast<const llvm::Function*>(addr);
            nodename << "fn" << count;
            nodelabel << "@" << fn->getNameStr();
          } else {
            nodename << "unknown" << count;
            nodelabel << "???" << (void*)addr;
          }
          // Print
          (*dot) << "  " << nodename.str()
                 << " [label=\"" << nodelabel.str() << "\",shape=circle];\n";
          if (object.baseObj) {
            (*dot) << "  n" << id << ":f" << k << " -> " << nodelabel.str() << ";\n";
          } else {
            (*dot) << "  r" << id << " -> " << nodelabel.str() << ";\n";
          }
        }
      }
    }
  }

  (*dot) << "}" << std::endl;
}

void DepthFirstICS::dumpSymbolicTree(cloud9::worker::WorkerTree::Node *root,
                                     cloud9::worker::WorkerTree::Node *highlight,
                                     const char *fileprefix) {
  _jobManager->dumpSymbolicTree(root, ICSWorkerNodeDecorator(this,highlight), fileprefix);
}

void DepthFirstICS::dumpConstraintsFromPrecondSlice(const PrecondSliceTrace &slice,
                                                    const char *fileprefix,
                                                    bool filter) {
  if (!DumpSliceConstraints)
    return;

  std::auto_ptr<std::ostream>
    os(_jobManager->openTraceOutputFile(fileprefix, "txt", "AllConstraints"));
  llvm::raw_os_ostream llos(*os);
  llos.SetUnbuffered();

  for (PrecondSliceTrace::const_iterator e = slice.begin(); e != slice.end(); ++e) {
    if (e->kind() == PrecondSliceTrace::ResolvedSymbolicPtr)
      continue;
    // N.B.: the inputHarnessTrace hasn't been filtered, so we filter it now.
    if (filter) {
      if (!e->hasCondition())
        continue;
    }
    // Otherwise, whe only remaining entries in the slice should have constraints.
    else {
      assert(e->kind() == PrecondSliceTrace::Assertion ||
             e->kind() == PrecondSliceTrace::Branch ||
             e->kind() == PrecondSliceTrace::Call);
      assert(e->hasCondition());
    }

    (*os) << KleeForkClassToString(e->forkclass());
    DumpKInstruction("\n# ", e->kinst(), llos);
    (*os) << "\n" << e->condition() << "\n\n";
  }
}

void DepthFirstICS::dumpFinalSymbolicObjects() {
  if (!DumpRegionSymbolicObjects)
    return;

  assert(_symbolicObjects.get());

  // XXX: Should do this elsewhere?
  // Enumerate thread spawn fns
  std::set<const Function*> spawnFns;
  for (Value::use_iterator
       U = KleeThreadCreateFn->use_begin(); U != KleeThreadCreateFn->use_end(); ++U) {
    CallInst *call = dyn_cast<CallInst>(*U);
    if (call->getNumArgOperands() == 3 && isa<Function>(call->getArgOperand(1)))
      spawnFns.insert(cast<Function>(call->getArgOperand(1)));
  }

  std::auto_ptr<std::ostream>
    os(_jobManager->openSummaryOutputFile("symbolicObjectsSummary.txt", "SymbolicObjectsSummary"));

  (*os) << "======= SymbolicObjectTable =======\n";
  for (klee::SymbolicObjectTable::iterator
       it = _symbolicObjects->begin(); it != _symbolicObjects->end(); ++it) {
    const klee::SymbolicObject &obj = it->second;
    // Skip locals with an empty context
    // These were made for function summaries and should not appear in the final state
    if ((obj.kind == klee::SymbolicObject::VirtualRegister
         || obj.kind == klee::SymbolicObject::FunctionArgument) &&
        obj.callctx == klee::CallingContext::Empty()) {
      // EXCEPT FOR: main(), and any function passed to klee_thread_create()
      const Function *f =
        (obj.kind == klee::SymbolicObject::VirtualRegister) ? obj.inst->getParent()->getParent()
                                                            : obj.arg->getParent();
      if (f->getName() != "main" && !spawnFns.count(f) && f->getName() != "slave_sort" /* XXX */)
        continue;
    }

    // Dump
    (*os) << it->first << ":" << it->first->name << "\n";
    (*os) << "   Kind: " << klee::SymbolicObject::KindToString(obj.kind) << "\n";
    if (obj.arg)
      (*os) << "   Arg: "  << llvmValueToString(obj.arg) << "\n";
    if (obj.inst)
      (*os) << "   Inst: " << llvmValueToString(obj.inst) << "\n";
    if (obj.callctx)
      (*os) << "   CallCtx:\n" << klee::CallingContextPrinter(obj.callctx, "      ");
    if (obj.ptr)
      (*os) << "   Ptr: " << llvmValueToString(obj.ptr) << "\n";
    if (obj.mo)
      (*os) << "   Mem: " << obj.array->name  << " @ " << obj.mo->address << "\n";
    (*os) << "\n";
  }
}

void DepthFirstICS::dumpFinalRegionSummary(bool isFinal) {
  if (IcsExecMode != IcsNormal)
    return;

  // Dump a summary of the regions
  std::auto_ptr<std::ostream> os;
  if (isFinal) {
    os.reset(_jobManager->openSummaryOutputFile("regionSummary.txt", "RegionsSummary"));
  } else {
    os.reset(_jobManager->openTraceOutputFile("regionsCurr", "txt", "RegionsSummary"));
  }

  // Sort by id
  typedef std::map<size_t, ParallelRegionId> SortedRegionIds;
  SortedRegionIds sorted;
  for (RegionsInfoTy::iterator it = _regions.begin(); it != _regions.end(); ++it) {
    sorted[it->second->numericId] = it->first;
  }

  // Stats
  uint64_t schedTotal = 0;
  uint64_t schedMinPerEpoch = UINT64_MAX;
  uint64_t schedMinPerEpoch2 = UINT64_MAX;
  uint64_t schedMaxPerEpoch = 0;
  uint64_t schedEpochs = 0;
  uint64_t schedNontrivialTotal = 0;
  uint64_t schedNontrivialMinPerEpoch = UINT64_MAX;
  uint64_t schedNontrivialMinPerEpoch2 = UINT64_MAX;
  uint64_t schedNontrivialMaxPerEpoch = 0;
  uint64_t schedNontrivialEpochs = 0;
  uint64_t schedDeadlocks = 0;
  uint64_t schedExits = 0;

  // Print a summary for each region
  for (SortedRegionIds::iterator it = sorted.begin(); it != sorted.end(); ++it) {
    const ParallelRegionId &id = it->second;
    const ParallelRegionInfo &info = *_regions[id];
    bool isNonTrivial = (info.regionEdges.size() > 1);
    (*os) << "======= Parallel Region #" << info.numericId << " =======\n";
    info.perThreadTime.dump(*os, "PathTime");
    info.perThreadImbalance.dump(*os, "Imbalance");
    info.perThreadImbalanceNoSleepers.dump(*os, "ImbalanceNoSleep");
    (*os) << "Stack:\n"
          << ParallelRegionIdPrinter(id, "  ");
    for (size_t k = 0; k < info.regionEdges.size(); ++k) {
      const RegionPathEdge &edge = info.regionEdges[k];
      const ParallelRegionInfo *nextInfo = NULL;
      (*os) << "RegionEdge[" << k << "].traceId = " << edge.traceId << "\n"
            << "RegionEdge[" << k << "].exits = "
            << (edge.deadlocks ? "deadlock" : (edge.exits ? "yes" : "no")) << "\n";
      if (!edge.exits && !edge.deadlocks) {
        if (_regions.count(edge.nextRegion)) {
          nextInfo = _regions[edge.nextRegion].get();
          const size_t nextId = nextInfo->numericId;
          (*os) << "RegionEdge[" << k << "].nextRegion = " << nextId << "\n";
        } else {
          assert(!isFinal);
          (*os) << "RegionEdge[" << k << "].nextRegion = ???\n";
          (*os) << ParallelRegionIdPrinter(id, "    ");
        }
      }
      // Nontrivial if >1 edges (see initialization)
      // Also, nontrivial if there exists and edge that exits, deadlocks, or
      // connects to a region with multiple edges
      if (edge.traceId > 0 &&  // ignore the initial region
          (edge.exits || edge.deadlocks || (nextInfo && nextInfo->regionEdges.size() > 1))) {
        isNonTrivial = true;
      }
    }
    (*os) << "\n";
    // Stats
    const uint64_t n = info.regionEdges.size();
    schedTotal += n;
    schedEpochs++;
    schedMinPerEpoch = std::min(n, schedMinPerEpoch);
    schedMaxPerEpoch = std::max(n, schedMaxPerEpoch);
    if (n > 1) {
      schedMinPerEpoch2 = std::min(n, schedMinPerEpoch2);
    }
    if (isNonTrivial) {
      schedNontrivialTotal += n;
      schedNontrivialEpochs++;
      schedNontrivialMinPerEpoch = std::min(n, schedNontrivialMinPerEpoch);
      schedNontrivialMaxPerEpoch = std::max(n, schedNontrivialMaxPerEpoch);
      if (n > 1) {
        schedNontrivialMinPerEpoch2 = std::min(n, schedNontrivialMinPerEpoch2);
      }
    }
    for (size_t k = 0; k < info.regionEdges.size(); ++k) {
      const RegionPathEdge &edge = info.regionEdges[k];
      if (edge.exits) schedExits++;
      if (edge.deadlocks) schedDeadlocks++;
    }
  }

  // Dump stats
  llvm::errs() << "======= Final Region Stats =======\n"
               << "SchedTotal = "        << schedTotal << "\n"
               << "SchedMinPerEpoch  = " << schedMinPerEpoch << "\n"
               << "SchedMinPerEpoch2 = " << schedMinPerEpoch2 << "\n"
               << "SchedMaxPerEpoch  = " << schedMaxPerEpoch << "\n"
               << "SchedEpochs = "       << schedEpochs << "\n"
               << "SchedNontrivialTotal = "       << schedNontrivialTotal << "\n"
               << "SchedNontrivialMinPerEpoch  = " << schedNontrivialMinPerEpoch << "\n"
               << "SchedNontrivialMinPerEpoch2 = " << schedNontrivialMinPerEpoch2 << "\n"
               << "SchedNontrivialMaxPerEpoch  = " << schedNontrivialMaxPerEpoch << "\n"
               << "SchedNontrivialEpochs = "      << schedNontrivialEpochs << "\n"
               << "SchedDeadlocks = " << schedDeadlocks << "\n"
               << "SchedExits = "     << schedExits << "\n";
}

//-----------------------------------------------------------------------------
// The procondition slicing algorithm
// Adapted from "Bouncer: Securing Software by Blocking Bad Input"
//
// Note that the final slice includes only Assertions (e.g., klee_assume) and
// symbolic ControlTransfers (the key branches that define the input)
//-----------------------------------------------------------------------------

namespace {

const char *ExtraDebugEnabled = NULL;

struct PrecondSliceComputor {
private:
  typedef std::set<const llvm::Value*> liveregs_set_ty;  // can have Instructions and Arguments
  typedef std::map<const llvm::PHINode*, const llvm::Value*> livephis_map_ty;

  typedef llvm::SmallVector<const PrecondSliceTrace::Entry*,4> entry_list_ty;
  typedef std::tr1::unordered_map<const klee::MemoryObject*, entry_list_ty> livemem_ty;
  typedef std::multimap<const llvm::Instruction*, const PrecondSliceTrace::Entry*> liveloads_ty;
  typedef llvm::DenseSet<const klee::MemoryObject*> usedobjects_ty;
  typedef std::vector<std::pair<const PrecondSliceTrace::Entry*,
                                klee::ConstraintManager> > constraint_stack_ty;

  struct ThreadFrame {
    // note: no need to store a PC, since control transfer is explicit in the trace
    liveregs_set_ty liveRegs;
  };

  typedef std::deque<ThreadFrame> threadstack_ty;

  struct ThreadState {
    threadstack_ty stack;
    llvm::Instruction *sliceHead;
    llvm::Function *ignoredFn;
    int ignoredFnDepth;
  };

  typedef std::map<klee::ThreadUid, ThreadState> threadmap_ty;

  DepthFirstICS        *_strat;
  PrecondSliceTrace    *_slice;
  klee::TimingSolver   *_kleeSolver;
  klee::ExecutionState *_kleeFinalState;

  livemem_ty             _liveMem;
  liveloads_ty           _liveLoads;
  threadmap_ty           _threads;
  ThreadState*           _currThread;
  threadmap_ty::iterator _currThreadIt;
  constraint_stack_ty    _constraintStack;

  PrecondSliceTrace::const_reverse_iterator _currEntry;
  llvm::BasicBlock::iterator _currInst;

  // This is an output of the algorithm
  std::set<const llvm::Loop*> _loopsWithUncoveredConditions;

private:
  struct UpdateLiveMemForStore_Stats {
    size_t total;
    size_t objNotLive;
    size_t overlapChecks;
    size_t constChecks;
    size_t constFoundMust;
    size_t constFoundMay;
    size_t skipSolver;
    size_t solverCheckMust;
    size_t solverCheckMay;
    size_t solverFoundMust;
    size_t solverFoundMay;
  } _updateLiveMemForStore_Stats;

  struct PathMaySyncOrWriteLiveMem_Stats {
    size_t total;
    size_t withSync;
    size_t withLiveStore;
    size_t withLiveStoreViaCall;
    size_t storesChecked;
    size_t aliasQueries;
    size_t calleeQueries;
  } _pathMaySyncOrWriteLiveMem_Stats;

  struct FunctionMayWriteLiveMem_Stats {
    size_t total;
    size_t readOnly;
    size_t withLiveStore;
    size_t modrefQueries;
  } _functionMayWriteLiveMem_Stats;

  struct Algorithm_Stats {
    size_t instrs;
    size_t instrsTaken;
    size_t instrsSkipped;
    size_t fnCallsSkipped;
    size_t syncOps;
    size_t branchesTaken;
    size_t branchesTakenLive;
    size_t branchesTakenWithSymbolicCondition;
    size_t branchesTakenWithUpdatedFlips;
    size_t branchesSkippedWithSymbolicCondition;
    size_t branchesSkippedUnconditional;
    size_t branchesSkippedPrecondOpt;
    size_t branchesSkippedExitOpt;
    size_t branchesSkippedTrivial;
    size_t resolvedPtrsTaken;
  } _algorithm_Stats;

  void ResetStats();

private:
  // Used internally by the branch handling code
  // This describes the targets of a branch that was encountered along the current path.
  // Each branch edges as an (optional) symbolic condition, where the branch edges should
  // be taken when that condition holds.  N.B.: if a symbolic condition is given for one
  // branch edge, then it is given for all branch edges.
  struct BranchTargets {
    // This edge was taken in the current path
    llvm::BasicBlock *takenBB;
    ref<Expr> takenCond;
    // These are the other possible branch edges
    typedef std::map<llvm::BasicBlock*, ref<Expr> > MapTy;
    MapTy other;

    FlippedConditionsMapTy otherToFlippedConditions() const {
      FlippedConditionsMapTy flips;
      for (MapTy::const_iterator it = other.begin(); it != other.end(); ++it)
        flips = flips.insert(std::make_pair(it->second, it->first));
      return flips;
    }
  };

private:
  // Debugging
  void DumpIterators(const char *prefix);

  // Precomputations
  void PreComputeConstraintStack(const PrecondSliceTrace &trace);

  // Iterating over the trace
  // NB: the algorithm works backwards, so "advance" goes backwards
  bool IsLiveSetEnabledInCurrThread();
  void DoContextSwitchToNewThread(const klee::ThreadUid &tuid);
  void DoContextSwitch();
  void DoFunctionCall();
  void DoFunctionReturn();
  void DoControlTransfer();
  void AdvanceCurrInst();
  void AdvanceCurrEntry();

  // Main algorithm
  bool ProcessCurrEntry(const PrecondSliceTrace &trace);
  void ProcessCurrInst();
  void AddToSliceHead(llvm::Instruction *inst);
  void AddToLiveRegs(const llvm::Value *val);
  void AddOperandsToLiveRegs();
  void UpdateLiveMemForLoad(const PrecondSliceTrace::Entry &load);
  bool UpdateLiveMemForStore(const PrecondSliceTrace::Entry &store);
  bool UpdateLiveMemForAlloc(const PrecondSliceTrace::Entry &alloc);
  bool RemoveFromLiveLoads(const PrecondSliceTrace::Entry *e);

  bool SolverMayBeTrue(ref<Expr> expr);
  bool SolverMustBeTrue(ref<Expr> expr);

  void GetTargetsForBranch(const PrecondSliceTrace::Entry *e, BranchTargets *targets);
  bool Postdominates(llvm::Instruction *end, llvm::Instruction *beg);
  bool PathMaySyncOrWriteLiveMem(llvm::BasicBlock *begBlock,
                                 llvm::Instruction *end,
                                 bool mightReturn);
  bool PathsMaySyncOrWriteLiveMem(BranchTargets *targets,
                                  llvm::Instruction *end,
                                  bool mightReturn = false);
  bool FunctionMayWriteLiveMem(const llvm::Function *F);
  bool CallTargetsMaySyncOrWriteLiveMem(CallSite cs, const llvm::Function *ignoreF);

  friend void ComputePrecondSlice(klee::Executor *executor,
                                  klee::ExecutionState *finalState,
                                  DepthFirstICS *strat,
                                  PrecondSliceTrace *slice);

public:
  void Compute(const PrecondSliceTrace &trace);
};

//
// Debugging
//

void PrecondSliceComputor::DumpIterators(const char *prefix) {
  llvm::errs() << prefix << "\n  entry: " << _currEntry->kind();
  if (_currEntry->hasCondition()) {
    llvm::errs() << "  (has condition)";
  }
  DumpTuid        ("\n  currTuid:", _currThreadIt->first);
  DumpInstruction ("\n  currInst:", &*_currInst);
  DumpTuid        ("\n  currEntry.tuid:", _currEntry->tuid());
  DumpKInstruction("\n  currEntry.inst:", _currEntry->kinst());
  if (PrecondSliceTrace::IsControlTransfer(_currEntry->kind())) {
    DumpTuid        ("\n  currEntry.targetTuid:", _currEntry->targetTuid());
    DumpKInstruction("\n  currEntry.targetInst:", _currEntry->targetKinst());
  }
  llvm::errs() << "\n";
}

//
// Precomputations
//

void PrecondSliceComputor::PreComputeConstraintStack(const PrecondSliceTrace &trace) {
  _constraintStack.clear();

  // The initial constraint manager includes the inputHarness constraints
  klee::ConstraintManager tmp;
  tmp = _kleeFinalState->inputHarnessConstraints;
  _constraintStack.push_back(constraint_stack_ty::value_type(NULL, tmp));

  // Add constraints one-by-one
  for (PrecondSliceTrace::const_iterator e = trace.begin(); e != trace.end(); ++e) {
    if (e->hasCondition()) {
      tmp.addConstraint(e->condition());
      _constraintStack.push_back(std::make_pair(&*e, tmp));
    }
  }
}

//
// Iterating over the trace
//

bool IsSpecificFnCall(const PrecondSliceTrace::Entry &e, llvm::Function *f) {
  return f
      && e.kind() == PrecondSliceTrace::Call
      && e.targetKinst()->inst->getParent()->getParent() == f;
}

bool IsSpecificFnRet(const PrecondSliceTrace::Entry &e, llvm::Function *f) {
  return f
      && e.kind() == PrecondSliceTrace::Ret
      && e.kinst()->inst->getParent()->getParent() == f;
}

bool PrecondSliceComputor::IsLiveSetEnabledInCurrThread() {
  return _currThread->ignoredFn == NULL;
}

void PrecondSliceComputor::DoContextSwitchToNewThread(const klee::ThreadUid &tuid) {
  std::pair<threadmap_ty::iterator, bool> res =
    _threads.insert(std::make_pair(tuid, ThreadState()));

  if (DbgPrecondSlicing)
    std::cerr << "SLICE: new thread " << tuid << "\n";

  assert(_currInst);

  _currThreadIt = res.first;
  _currThread = &res.first->second;
  _currThread->sliceHead = _currInst;
  _currThread->ignoredFn = NULL;
  _currThread->ignoredFnDepth = 0;
  _currThread->stack.push_back(ThreadFrame());
}

void PrecondSliceComputor::DoContextSwitch() {
  assert(_currEntry->kind() == PrecondSliceTrace::CtxSwitch);

  // Newly terminated thread?
  llvm::CallInst *call = dyn_cast<CallInst>(_currEntry->kinst()->inst);
  if (call) {
    llvm::Function *callee = call->getCalledFunction();

    if (callee == KleeThreadTerminateFn) {
      DoContextSwitchToNewThread(_currEntry->tuid());
      return;
    }

    if (callee == KleeProcessTerminateFn) {
      // Remove all threads with a matching process id
      threadmap_ty::iterator it, next;
      for (next = _threads.begin(); next != _threads.end(); ) {
        it = next;
        next++;
        if (it->first.pid == _currEntry->tuid().pid) {
          assert(_currThread != &it->second);
          _threads.erase(it);
        }
      }

      // Make one new thread
      DoContextSwitchToNewThread(_currEntry->tuid());
      return;
    }
  }

  // New thread ID?  This happens when the thread was not shutdown with
  // klee_thread_terminate.  For example, the klee state was terminated
  // early due to an error during execution, or the thread was shutdown
  // as a group by klee_process_terminate.
  threadmap_ty::iterator it = _threads.find(_currEntry->tuid());
  if (it == _threads.end()) {
    DoContextSwitchToNewThread(_currEntry->tuid());
    return;
  }

  // Not a new thread: just update the thread pointer
  _currThreadIt = it;
  _currThread = &it->second;
}

void PrecondSliceComputor::DoFunctionCall() {
  assert(_currEntry->kind() == PrecondSliceTrace::Call);

  const Function *calleeF = _currEntry->targetKinst()->inst->getParent()->getParent();
  if (DbgPrecondSlicing) {
    llvm::errs() << "SLICE: call " << _currThread->stack.size()
                 << " " << calleeF->getNameStr() << "\n";
  }

  assert(_currThread->stack.size() >= 1);

  // XXX: Ugly case
  // See Steensgaard.cpp for more details.  Essentially, in this case, we pretend
  // that the memory locations written by calleeF will never alias memory locations
  // written by the caller.  E.g., we assume that pthread sync objects are opaque
  // to all functions except for the pthread sync functions, which are included in
  // the "ignored" set.
  // XXX: EVEN UGLIER: do this for all klee pthreads model functions.  The specific
  // motivation is that, since DSAA sucks, __tsync gets collapsed into a single node.
  // Thus, when calls to pthread_cleanup_{push,pop} are intermixed with calls to
  // pthread_{create,join}, as happens in most programs, the cleanup calls will appear
  // to be "live" as they write to __tsync (although they write to fields of __tsync
  // that should not affect the synchronization behavior of pthread_{create,join}).
  if (DSAA->isSteensIgnoredFunction(calleeF)
      || calleeF->getName().startswith("__klee_pthread_model_")) {
    if (DbgPrecondSlicingVerbose) {
      llvm::errs() << "REMOVING LiveLoads added by " << calleeF->getName() << "\n";
    }
    // Remove all live loads that originated in calleeF
    for (livemem_ty::iterator it = _liveMem.begin(); it != _liveMem.end(); /* no inc here */) {
      entry_list_ty &entries = it->second;
      for (entry_list_ty::iterator e = entries.begin(); e != entries.end(); /* no inc here */) {
        if ((*e)->kinst()->inst->getParent()->getParent() == calleeF) {
          bool found = RemoveFromLiveLoads(*e);
          assert(found);
          e = entries.erase(e);
        } else {
          ++e;
        }
      }
      if (entries.empty()) {
        it = _liveMem.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Since we're going over the trace backwards, we're "returning" to the caller
  liveregs_set_ty local;
  local.swap(_currThread->stack.back().liveRegs);
  _currThread->stack.pop_back();

  // If the end of the trace didn't start at the bottom of the call stack,
  // we'll need to push call stacks from below as they're discovered.  (For
  // example, if main() ends with a call to foo() which calls exit(), we'll
  // need to push a new stack for main when we get to "call foo".
  if (_currThread->stack.empty())
    _currThread->stack.push_back(ThreadFrame());

  // Now check if there were any live incoming arguments, and if so, go mark
  // the parameters live in the caller.  N.B.: Have to do this here rather than
  // in ProcessCurrEntry: by then, the stack frame (liveregs) have been popped.
  if (local.empty() || !IsLiveSetEnabledInCurrThread())
    return;

  llvm::CallSite cs(_currEntry->kinst()->inst);
  assert(cs.getInstruction());

  for (liveregs_set_ty::iterator r = local.begin(); r != local.end(); ++r) {
    if (const llvm::Argument *arg = dyn_cast<Argument>(*r))
      AddToLiveRegs(cs.getArgument(arg->getArgNo()));
  }
}

void PrecondSliceComputor::DoFunctionReturn() {
  assert(_currEntry->kind() == PrecondSliceTrace::Ret);

  // Since we're going over the trace backwards, we're "calling" to the callee
  _currThread->stack.push_back(ThreadFrame());

  if (DbgPrecondSlicing) {
    std::cerr << "SLICE: ret " << _currThread->stack.size()
              << " " << _currEntry->kinst()->inst->getParent()->getParent()->getNameStr()
              << "\n";
  }
}

void PrecondSliceComputor::DoControlTransfer() {
  // Process any major control transfers (cross-function or cross-thread) for _currEntry
  // N.B.: Must update `_currInst` *before* calling this! (see AdvanceCurrInst)
  switch (_currEntry->kind()) {
  case PrecondSliceTrace::Assertion:
  case PrecondSliceTrace::Branch:
  case PrecondSliceTrace::Load:
  case PrecondSliceTrace::Store:
  case PrecondSliceTrace::Allocation:
  case PrecondSliceTrace::RegionMarker:
  case PrecondSliceTrace::ResolvedSymbolicPtr:
    break;

  case PrecondSliceTrace::CtxSwitch:
    DoContextSwitch();
    return;

  case PrecondSliceTrace::Call:
    DoFunctionCall();
    break;

  case PrecondSliceTrace::Ret:
    DoFunctionReturn();
    break;

  default:
    assert(false);
  }
}

void PrecondSliceComputor::AdvanceCurrInst() {
  assert(_currInst);

  // If we have a pending control transfer, follow it backwards
  if (PrecondSliceTrace::IsControlTransfer(_currEntry->kind())) {
    if (&*_currInst == _currEntry->targetKinst()->inst) {
      _currInst = _currEntry->kinst()->inst;
      DoControlTransfer();
      return;
    }
  }

  // Otherwise, go to the prior instruction in the same basic block
  if (_currInst == _currInst->getParent()->begin()) {
    DumpIterators("????");
    assert(0);
  }
  _currInst--;  // recall this is a forward iterator
}

void PrecondSliceComputor::AdvanceCurrEntry() {
  // Pop the constraint stack?
  if (!_constraintStack.empty()) {
    if (&*_currEntry == _constraintStack.back().first) {
      // N.B.: The top-of-stack constraints are the current constraints
      // We want to pop these off and move to the next constraints
      _constraintStack.pop_back();
      assert(!_constraintStack.empty());
      _kleeFinalState->constraints() = _constraintStack.back().second;
    }
  }


  // Go to the next entry
  _currEntry++;
}

//
// Main algorithm
//

void PrecondSliceComputor::Compute(const PrecondSliceTrace &trace) {
  _liveMem.clear();
  _liveLoads.clear();
  _threads.clear();

  _currEntry = trace.rbegin();
  _currThread = NULL;

  // Empty trace?
  if (_currEntry == trace.rend()) {
    std::cerr << "WARNING: Empty precond slice trace?\n";
    return;
  }

  // Reconstruct the sequence of constraint sets that were used during
  // program execution.  As we iterate backwards over the trace, we pop
  // the current constraint set off the back of this sequence.
  klee::PushConstraints savedConstraints(_kleeFinalState->constraints());
  PreComputeConstraintStack(trace);

  // Setup a thread for a new trace
  // Careful: last entry in the trace might be a control transfer, in which case
  // the initial thread comes from the *target* of the control transfer
  if (PrecondSliceTrace::IsControlTransfer(_currEntry->kind())) {
    _currInst = _currEntry->targetKinst()->inst;
    DoContextSwitchToNewThread(_currEntry->targetTuid());
  } else {
    _currInst = _currEntry->kinst()->inst;
    DoContextSwitchToNewThread(_currEntry->tuid());
  }

  // We are control dependent on the last instruction in the trace
  // Should be handled by DoContextSwitchToNewThread()
  assert(_currThread);
  assert(_currThread->sliceHead == _currInst);

  // Now start taking the slice
  // Iteration strategy: go over all instructions in the execution one-by-one
  for (;;) {
    bool doneWithCurr = ProcessCurrEntry(trace);

    // Ugly: internally modelled functions may emit multiple trace entries per
    // instruction, for example, va_start emits 4 stores! :-/  Handle this by
    // looping back if we're on the same instruction after advancing the entry.
    while (_currEntry != trace.rend() && &*_currInst == _currEntry->kinst()->inst) {
      if (_currEntry->kind() == PrecondSliceTrace::CtxSwitch) {
        // This can happen when two threads execute the same instruction concurrently.
        DoControlTransfer();
      } else if (_currEntry->kind() == PrecondSliceTrace::Branch) {
        // This can happen when we executed a loop and had no events between two 
        // loop iterations.  In this case, we must iterate over all instructions
        // in the loop, so we process the branch events separately.
        break;
      } else {
        // XXX: Asserting here since these haven't appeared in practice, though I
        // think that Ret, at least, is possible in the case of recursive calls.
        assert(_currEntry->kind() != PrecondSliceTrace::Call);
        assert(_currEntry->kind() != PrecondSliceTrace::Ret);
      }

      if (DbgPrecondSlicing) {
        llvm::errs() << "SLICE: going around again "
                     << _currThreadIt->first << " " << (*_currInst) << "\n";
      }
      doneWithCurr |= ProcessCurrEntry(trace);
    }

    if (!doneWithCurr) {
      ProcessCurrInst();
    }

    // We hit the definition of this instruction, so it's not longer live
    liveregs_set_ty &liveregs = _currThread->stack.back().liveRegs;
    liveregs.erase(_currInst);

    if (_currEntry == trace.rend())
      break;

    // Advance to the previous instruction
    _algorithm_Stats.instrs++;
    AdvanceCurrInst();
  }
}

bool PrecondSliceComputor::ProcessCurrEntry(const PrecondSliceTrace &trace) {
  if (&*_currInst != _currEntry->kinst()->inst)
    return false;

  if (_currThreadIt->first != _currEntry->tuid()) {
    DumpIterators("BAD ENTRY");
    assert(false);
  }

  Algorithm_Stats &stats = _algorithm_Stats;

  if (!IsLiveSetEnabledInCurrThread()) {
    // Do less work in ignored functions
    if (_currEntry->kind() != PrecondSliceTrace::Call &&
        _currEntry->kind() != PrecondSliceTrace::Ret &&
        _currEntry->kind() != PrecondSliceTrace::Branch &&
        _currEntry->kind() != PrecondSliceTrace::Assertion &&
        _currEntry->kind() != PrecondSliceTrace::Allocation) {
      stats.instrsSkipped++;
      AdvanceCurrEntry();
      return true;
    }
  }

  llvm::Instruction *inst = _currEntry->kinst()->inst;
  liveregs_set_ty &liveregs = _currThread->stack.back().liveRegs;

  if (DbgPrecondSlicingVerbose) {
    DumpIterators("SLICE: entry");
  }

  switch (_currEntry->kind()) {
  case PrecondSliceTrace::Ret: {
    llvm::Function *f = inst->getParent()->getParent();
    assert(f);

    // Trudging through an ignored function?
    // If backtracking into a recursive call, inc depth and keep trudging
    if (!IsLiveSetEnabledInCurrThread()) {
      if (IsSpecificFnRet(*_currEntry, _currThread->ignoredFn)) {
        _currThread->ignoredFnDepth++;
      }
      stats.instrsSkipped++;
      break;
    }

    // We can ignore this function if:
    //  (1) it is a known ignorable function, or
    //  (2a) it won't perform a sync op, and
    //  (2b) it won't write to live memory
    if (IsIgnorableSyncOpCaller(f) || (!IsSyncOpCaller(f) && !FunctionMayWriteLiveMem(f))) {
      if (DbgPrecondSlicing) {
        std::cerr << "SLICE: skipped fn: " << f->getNameStr() << "\n";
      }
      _currThread->ignoredFn = f;
      _currThread->ignoredFnDepth = 1;
      stats.fnCallsSkipped++;
    }

    // Take()
    // N.B.: We need to take the return value only if that value is live in the caller
    else {
      llvm::BasicBlock::iterator call = _currEntry->targetKinst()->inst;
      if (call == call->getParent()->begin()) {
        // FIXME: this handling of invokes is conservative
        AddOperandsToLiveRegs();    // this call used an InvokeInst
      } else {
        --call;
        const size_t sz = _currThread->stack.size();
        assert(sz > 1);
        assert(isa<CallInst>(call));
        liveregs_set_ty &callerLiveregs = _currThread->stack[sz-2].liveRegs;
        if (callerLiveregs.count(call))
          AddOperandsToLiveRegs();  // this call used a CallInst
      }
      AddToSliceHead(inst);
    }

    break;
  }

  case PrecondSliceTrace::Call: {
    llvm::Function *f = _currThread->ignoredFn;
    bool take = true;

    // Leaving an ignored function?
    if (IsSpecificFnCall(*_currEntry, f)) {
      if ((--_currThread->ignoredFnDepth) == 0) {
        _currThread->ignoredFn = NULL;
      }
      // Careful: if this is an indirect call, we have to ensure that no
      // other possible target of the call can sync or write to live memory.
      take = CallTargetsMaySyncOrWriteLiveMem(CallSite(inst), f);
    }

    else if (!IsLiveSetEnabledInCurrThread()) {
      stats.instrsSkipped++;
      take = false;
    }

    // Other functions (not ignored, i.e., vital for control dependence):
    // We already took the args (if needed) in DoFunctionCall(), so now we
    // take (a) the value returned by the call call, and (b) the function
    // ptr (in case the call was an indirect call).
    if (take) {
      AddToSliceHead(inst);
      if (_currEntry->condition().get()) {
        _slice->pushEntry(*_currEntry);
      }
      if (isa<CallInst>(inst) || isa<InvokeInst>(inst)) {
        CallSite cs(inst);
        AddToLiveRegs(cs.getCalledValue()->stripPointerCasts());
      } else {
        // N.B.: We invoke these functions when a thread returns from
        // its main function.  These should be the only cases in which
        // "inst" is not a function call.
        const llvm::Function *targetF =
          _currEntry->targetKinst()->inst->getParent()->getParent();
        assert(targetF->getName() == "exit" ||
               targetF->getName() == "__klee_pthread_model_pthread_exit");
        assert(isa<ReturnInst>(inst));
      }
    }

    break;
  }

  case PrecondSliceTrace::Branch: {
    bool take = false;
    BranchTargets targets;
    GetTargetsForBranch(&*_currEntry, &targets);
    // Special case when in ignored functions
    if (!IsLiveSetEnabledInCurrThread()) {
      // We still must include branches in ignored functions if going
      // the "other way" on the branch must lead to program exit.  These
      // branches may contain assertions that we MUST collect for soundness.
      // The best example is klee_div_zero_check(z).  Consider:
      //
      //    call klee_div_zero_check(%2)  // if (%2==0) abort()
      //    %3 = sdiv %1, %2
      //    %4 = icmp %3, 42
      //    br %4, label1, label2
      //
      // We will "ignore" klee_div_zero_check() because it does not perform
      // sync ops and does not write to live memory.  However, it includes
      // the statement "if (%2==0) abort()" -- if klee_div_zero_check() returns,
      // then it must be true that (%2 != 0).
      //
      // Consider what happens in the above if we ignore klee_div_zero_check()
      // completely: After taking the branch "br %4" we emit the constraint
      // (%1/%2 != 42), but we did not emit its sibling constraint (%2 != 0).
      // This is problematic for SMT solvers -- STP in particular has a mode
      // where x/0 evaluates to "1" to avoid strange corner cases.  Thus, we
      // must emit the constraint (%2 != 0) for soundness.
      //
      // In general, we want to scan ignored functions for branches that
      // must lead to "unreachable" statements.  Such branches are converted
      // to assertions in the style of optimization (c), below.
      const llvm::BasicBlock *br = inst->getParent();
      const llvm::BasicBlock *target = _currEntry->targetKinst()->inst->getParent();
      if (_currEntry->condition().get() && !DoesBasicBlockLeadToReturn(br, target)) {
        _slice->pushEntry(_currEntry->convertBranchToAssertion());
      }
      break;
    }
    // Take this branch instruction if:
    // (1) it was marked as "live" by a PHI node, or
    if (liveregs.count(inst) != 0) {
      take = true;
      stats.branchesTakenLive++;
    }
    // (2) it fails all optimizations
    else {
      take = true;
      assert(_currThread->sliceHead);
      // (a) the branch is unconditional
      if (isa<BranchInst>(inst) && cast<BranchInst>(inst)->isUnconditional()) {
        take = false;
        stats.branchesSkippedUnconditional++;
      }
      // (b) the precondition slicing check
      //     see "Bouncer" paper for details
      else if (Postdominates(_currThread->sliceHead, inst)
               && !PathsMaySyncOrWriteLiveMem(&targets, _currThread->sliceHead)) {
        take = false;
        stats.branchesSkippedPrecondOpt++;
      }
      // (c) the early-exit-optimization check:
      //   * there does not exist a path to a sync op or live mem when "going another way"
      //     on this branch (but note: if we hit a return statement, we conservatively assume
      //     that the caller might perform a sync op or write live mem)
      //   * is intended to ignore branches that target a sink for assertion failures or aborts
      // TODO: we could relax this further by also checking all paths after the return
      else if (OptSkipEarlyExitBranches
               && !PathsMaySyncOrWriteLiveMem(&targets, _currThread->sliceHead, true)) {
        take = false;
        stats.branchesSkippedExitOpt++;
        // If this branch is symbolic, convert the branch to an assertion so that some
        // later branch doesn't generate an input that will trigger this branch.
        // For example, suppose _currEntry is the branch for the following assert:
        //    assert(x != 1); // Sometime later, the program branches on a condition
        //    ...             // implied by this assertion: we want to avoid creating
        //    if (x == 1)     // an input for (x != 1) in this case.  In this example,
        //      syncop()      // we will not check the other side of the (x==1) branch.
        // TODO: need a unit test for this
        // TODO: need to decide if this is a full replacement for (b), above (I think it is)
        if (_currEntry->condition().get())
          _slice->pushEntry(_currEntry->convertBranchToAssertion());
      }
    }
    // DEBUG: Check if we found a symbolic branch
    if (take && _currEntry->condition().get()) {
      llvm::BasicBlock *bb = inst->getParent();
      llvm::Loop *loop = GetLoopInfo(bb->getParent())->getLoopFor(bb);
      if (0 && loop && !CoveredLoops.count(loop)) {
        _loopsWithUncoveredConditions.insert(loop);
        // DEBUG
        llvm::SmallVector<std::pair<llvm::BasicBlock*,llvm::BasicBlock*>, 4> exitedges;
        loop->getExitEdges(exitedges);
        llvm::errs() << "FOUND CONDITION IN UNCOVERED LOOP:\n"
                     << "  ... func: " << bb->getParent()->getName() << "\n"
                     << "  ... loop: " << loop->getHeader()->getName() << "\n"
                     << "  ... trip: " << loop->getTripCount() << "\n"
                     << "  ... islatch: " << IsLoopLatch(loop, bb) << "\n"
                     << "  ... isuncov: " << UncoveredLoops.count(loop) << "\n"
                     << "  ... inst: " << *inst << "\n";
        for (size_t k = 0; k < exitedges.size(); ++k)
          llvm::errs() << "  ... exit: " << exitedges[k].second->getName() << "\n";
        llvm::errs() << "  ... cond:\n" << *_currEntry->condition() << "\n";
        // DEBUG Option
        if (DbgIgnoreUncoveredLoops) {
          llvm::errs() << "IGNORING UNCOVERED LOOP!\n";
          take = false;
        }
      }
    }
    // DEBUG
    if (0 && (1 || take) &&
        (((0 && _currEntry->condition().get())
          && (1 || inst->getParent()->getParent()->getName() == "foreach_path")) ||
         (0 && isa<BranchInst>(inst)
            && cast<BranchInst>(inst)->isConditional()
            && (cast<BranchInst>(inst)->getSuccessor(0)->getName() == "do_ftw.exit.thread" ||
                cast<BranchInst>(inst)->getSuccessor(0)->getName() == "do_ftw.exit"))
        )) {
      if (take) {
        llvm::errs() << "TAKING BRANCH";
      } else {
        llvm::errs() << "SKIPPING BRANCH";
      }
      ics::DumpKInstruction("\n  kinst: ", _currEntry->kinst());
      ics::DumpKInstruction("\n  target: ", _currEntry->targetKinst());
      llvm::errs() << "\n  slicehd:" << *_currThread->sliceHead;
      llvm::errs() << "\n  slicehd:" << _currThread->sliceHead->getParent()->getName();
      llvm::errs() << "\n  live:" << liveregs.count(inst);
      if (isa<BranchInst>(inst) && cast<BranchInst>(inst)->isConditional()) {
        const bool postdom = Postdominates(_currThread->sliceHead, inst);
        llvm::errs() << "\n  postdom:" << postdom;
        if (postdom) {
          ExtraDebugEnabled = "\n  maywrite";
          BranchTargets tmp;
          GetTargetsForBranch(&*_currEntry, &tmp);
          const bool b = PathsMaySyncOrWriteLiveMem(&tmp, _currThread->sliceHead);
          llvm::errs() << "\n  maywrite:" << b;
        }
        if (1) {
          ExtraDebugEnabled = "\n  maywriteX";
          FlippedConditionsMapTy tmpFlipped;
          BranchTargets tmp;
          GetTargetsForBranch(&*_currEntry, &tmp);
          const bool b = PathsMaySyncOrWriteLiveMem(&tmp, _currThread->sliceHead, true);
          llvm::errs() << "\n  maywriteX:" << b;
        }
        ExtraDebugEnabled = NULL;
      }
      llvm::errs() << "\n  cond:" << _currEntry->condition();
      llvm::errs() << "\n";
      static bool first = true;
      if (0 && first) {
        first = false;
        llvm::Function *f = inst->getParent()->getParent();
        f->viewCFGOnly();
        llvm::ViewGraph(GetPostdominatorTree(f)->getRootNode(), "postdomtree", true);
      }
    }
    // Take()
    if (take) {
      AddOperandsToLiveRegs();
      AddToSliceHead(inst);
      stats.branchesTaken++;
      // This goes in the final slice if it has a symbolic condition
      if (_currEntry->condition().get()) {
        _slice->pushEntry(*_currEntry);
        stats.branchesTakenWithSymbolicCondition++;
        // The flipped conditions may have been pruned.  This happens when
        // a SwitchInst contains multiple successors that do not perform sync.
        if (_currEntry->useFlippedConditions() &&
            _currEntry->flippedConditions().size() != targets.other.size()) {
          _slice->attachFlippedConditions(targets.otherToFlippedConditions());
          _slice->attachNewCondition(targets.takenCond);
          stats.branchesTakenWithUpdatedFlips++;
        }
      }
    } else {
      if (_currEntry->condition().get()) {
        stats.branchesSkippedWithSymbolicCondition++;
      }
    }
    break;
  }

  case PrecondSliceTrace::Assertion:
    if (!OptNoConcretizationConstraintsInSlice
        && (klee::IsConcretizationForkClass(_currEntry->forkclass()))) {
      AddOperandsToLiveRegs();
      AddToSliceHead(inst);
    }
    // Add to the final slice
    // Do not Take() except for concretization constraints when the above optimization is disabled
    _slice->pushEntry(*_currEntry);
    break;

  case PrecondSliceTrace::Load:
    if (liveregs.count(inst) != 0) {
      // Take()
      UpdateLiveMemForLoad(*_currEntry);
      AddOperandsToLiveRegs();
      AddToSliceHead(inst);
    }
    break;

  case PrecondSliceTrace::Store:
    if (UpdateLiveMemForStore(*_currEntry)) {
      // Take()
      AddOperandsToLiveRegs();
      AddToSliceHead(inst);
    }
    break;

  case PrecondSliceTrace::Allocation: {
    bool live = UpdateLiveMemForAlloc(*_currEntry);
    if (liveregs.count(inst) != 0 && IsLiveSetEnabledInCurrThread()) {
      // Take()
      // N.B.: We need do not take the allocation size, i.e., we do
      // not call AddOperandsToLiveRegs().  If the size is used
      // to compute an offset into the allocated memory, it will appear
      // in the live set all on its own.  For example:
      //     p = malloc(n)
      //     p[0]   = ...;   // does not depend on n
      //     p[n-1] = ...;   // does depend on n (determinied from the access, not the malloc)
      // TODO: This will be wrong if we implement symbolic sizes
      AddToSliceHead(inst);
    } else {
      // If the memoryobject was live, the pointer should be too
      if (IsLiveSetEnabledInCurrThread())
        assert(!live);
    }
    break;
  }

  case PrecondSliceTrace::RegionMarker:
    // Nop
    break;

  case PrecondSliceTrace::ResolvedSymbolicPtr: {
    const llvm::Value *pointer = _currEntry->ptrResolution()->pointer;
    const klee::SymbolicObject *targetObj = _currEntry->ptrResolution()->targetObj;
    bool take = false;
    // If a function ptr was resolved, then it was used
    if (!targetObj) {
      take = true;
    }
    // Data ptrs were used if any of their alises were live
    else {
      const klee::MemoryObject *target = targetObj->mo;
      assert(target);
      // DEBUG
      if (DbgPrecondSlicing || DbgSymbolicPtrSlicing) {
        llvm::errs() << "SLICE: Symbolic Ptr\n";
        klee::SymbolicPtrResolution::Dump("  ", _currEntry->ptrResolution());
        llvm::errs() << "LiveMem? " << _liveMem.count(target) << "\n"
                     << "LivePtr? " << liveregs.count(pointer) << "\n";
      }
      // Include this event in the slice if there is a data-dependence on the
      // object pointed to by the resolved pointer.  We check if _liveMem includes
      // 'target' or any objects in its may-alias set.
      for (klee::SymbolicPtrResolutionTable::alias_iterator
           alias = _kleeFinalState->resolvedSymbolicPtrs.aliasBegin(target);
           alias != _kleeFinalState->resolvedSymbolicPtrs.aliasEnd(); ++alias) {
        if (_liveMem.count(*alias)) {
          take = true;
          if (DbgPrecondSlicing || DbgSymbolicPtrSlicing) {
            if (*alias == target)
              llvm::errs() << "  .. LIVE\n";
            else
              llvm::errs() << "  .. LIVE VIA ALIAS: " << (*alias)->address << "\n";
          }
          break;
        }
      }
    }
    if (take) {
      // There is a data dependence on this pointer
      // Note that we don't update the sliceHead: we're just forwarding a
      // dependency from one of our aliased ptrs into this ptr.
      AddToLiveRegs(pointer);
      _slice->pushEntry(*_currEntry);
      stats.resolvedPtrsTaken++;
    }
    // We don't process the actual instruction here, so forward to ProcessCurrInst
    AdvanceCurrEntry();
    return false;
  }

  case PrecondSliceTrace::CtxSwitch:
    // Nothing special: forward to ProcessCurrInst
    AdvanceCurrEntry();
    return false;

  default:
    assert(false);
  }

  AdvanceCurrEntry();
  return true;
}

void PrecondSliceComputor::ProcessCurrInst() {
  liveregs_set_ty &liveregs = _currThread->stack.back().liveRegs;
  Algorithm_Stats &stats = _algorithm_Stats;

  if (!IsLiveSetEnabledInCurrThread()) {
    assert(liveregs.count(_currInst) == 0);
    stats.instrsSkipped++;
    return;
  }

  // Check for a sync op.  Notes:
  // * We have to do this here, rather than ProcessCurrEntry(), since
  //   primitive sync ops are all klee built-ins, and we don't add built-in
  //   calls to the precondSliceTrace.
  // * We don't do this check when the live set is disabled to support the
  //   optimization OptIgnoreUclibcSync.
  // * We assume sync ops are always called directly: this assumption is
  //   safe since the only calls to SyncOpFunctions happen in model code,
  //   and further note that high-level functions like pthread_mutex_lock
  //   are in SyncOpCallers, but NOT in SyncOpFunctions.
  if (IsLiveSetEnabledInCurrThread()) {
    if (llvm::CallInst *call = dyn_cast<CallInst>(_currInst)) {
      Function *f = call->getCalledFunction();
      if (f && IsSyncOpFunction(f)) {
        if (DbgPrecondSlicing) {
          std::cerr << "SLICE: syncop fn: " << f->getNameStr() << "\n";
        }
        // For sync ops, we take (a) all args, and (b) the call instruction.
        CallSite cs(_currInst);
        for (CallSite::arg_iterator arg = cs.arg_begin(); arg != cs.arg_end(); ++arg) {
          AddToLiveRegs(*arg);
        }
        AddToSliceHead(_currInst);
        stats.syncOps++;
        return;
      }
    }
  }

  // Ignore if this instruction is not live
  if (liveregs.count(_currInst) != 0) {
    if (DbgPrecondSlicingVerbose) {
      llvm::errs() << "SLICE: inst " << _currThreadIt->first << " " << (*_currInst) << "\n";
    }
    AddOperandsToLiveRegs();
    AddToSliceHead(_currInst);
  }
}

void PrecondSliceComputor::AddToSliceHead(llvm::Instruction *inst) {
  // Push the given instruction onto the head of the slice
  assert(inst);
  _currThread->sliceHead = inst;

  if (DbgPrecondSlicing) {
    llvm::errs() << "  NEW-HEAD: " << _currThreadIt->first << " " << (*inst) << "\n";
  }

  _algorithm_Stats.instrsTaken++;
}

void PrecondSliceComputor::AddToLiveRegs(const llvm::Value *val) {
  if (isa<Instruction>(val) || isa<Argument>(val)) {
    if (DbgPrecondSlicingVerbose) {
      llvm::errs() << "  ADD LIVE: " << *val << "\n";
    }
    _currThread->stack.back().liveRegs.insert(val);
  }
}

void PrecondSliceComputor::AddOperandsToLiveRegs() {
  liveregs_set_ty &liveregs = _currThread->stack.back().liveRegs;

  // Add all operands of _currInst to liveregs
  switch (_currInst->getOpcode()) {
  case Instruction::PHI: {
    // Take the operand corresponding to the prior block in the trace
    assert(_currEntry->kind() == PrecondSliceTrace::Branch);
    assert(_currEntry->tuid() == _currThreadIt->first);
    assert(_currEntry->targetKinst()->inst->getParent() == _currInst->getParent());

    llvm::BasicBlock *incoming = _currEntry->kinst()->inst->getParent();
    llvm::PHINode *phi = cast<PHINode>(_currInst);
    llvm::Value *inval = phi->getIncomingValueForBlock(incoming);

    AddToLiveRegs(inval);
    // PHI nodes imply a control dependence that needs to be satisfied: we do
    // this by marking the incoming branch 'live'
    if (DbgPrecondSlicingVerbose) {
      llvm::errs() << "  ADD LIVE BR: " << *incoming->getTerminator() << "\n";
    }
    liveregs.insert(incoming->getTerminator());
    break;
  }

  case Instruction::Call: {
    // For external calls, we can sometimes skip taking operands
    CallInst *call = dyn_cast<CallInst>(_currInst);
    Function *F = call->getCalledFunction();
    if (F->getName() == "klee_malloc")
      break;
    goto takeall;
  }

  default:
  takeall:
    // Take all the operands
    for (llvm::Instruction::op_iterator
         op = _currInst->op_begin(); op != _currInst->op_end(); ++op) {
      AddToLiveRegs(*op);
    }
  }
}

void PrecondSliceComputor::UpdateLiveMemForLoad(const PrecondSliceTrace::Entry &load) {
  assert(load.kind() == PrecondSliceTrace::Load);

  // Update live memory
  _liveMem[load.memoryobject()].push_back(&load);

  // This set is used for alias analysis in PathMaySyncOrWriteLiveMem
  _liveLoads.insert(std::make_pair(load.kinst()->inst, &load));
}

bool PrecondSliceComputor::UpdateLiveMemForStore(const PrecondSliceTrace::Entry &store) {
  assert(store.kind() == PrecondSliceTrace::Store);

  UpdateLiveMemForStore_Stats& stats = _updateLiveMemForStore_Stats;
  stats.total++;

  // Is the MemoryObject live?
  livemem_ty::iterator it = _liveMem.find(store.memoryobject());
  if (it == _liveMem.end()) {
    stats.objNotLive++;
    return false;
  }

  entry_list_ty &entries = it->second;

  // Precompute info about the store
  ref<Expr> store_lower = store.offset();
  ref<Expr> store_upper = klee::AddExpr::create(store.offset(), store.nbytes());
  bool isLive = false;

  // Does this store overlap any live load on the MemoryObject?
  // Each load that is completely overwritten is removed from the live set
  for (entry_list_ty::iterator e = entries.begin(); e != entries.end(); /* no inc here */) {
    stats.overlapChecks++;

    // Fast path: constant addresses
    if (isa<klee::ConstantExpr>(store.offset()) && isa<klee::ConstantExpr>(store.nbytes()) &&
        isa<klee::ConstantExpr>((*e)->offset()) && isa<klee::ConstantExpr>((*e)->nbytes())) {
      stats.constChecks++;

      const uint64_t soff = dyn_cast<klee::ConstantExpr>(store.offset())->getZExtValue();
      const uint64_t eoff = dyn_cast<klee::ConstantExpr>((*e)->offset())->getZExtValue();
      const uint64_t sb = dyn_cast<klee::ConstantExpr>(store.nbytes())->getZExtValue();
      const uint64_t eb = dyn_cast<klee::ConstantExpr>((*e)->nbytes())->getZExtValue();

      // Must completely overlap?
      if (soff <= eoff && (eoff+eb) <= (soff+sb)) {
        stats.constFoundMust++;
        goto overwritten;
      }

      // May partially overlap?
      if (!isLive && (soff < (eoff+eb) && eoff < (soff+sb))) {
        stats.constFoundMay++;
        isLive = true;
      }

      // Otherwise: must not overlap
      goto notoverwritten;
    }

    // Now do the more expensive solver checks
    // TOTAL HACK: Skip the solver checks if this looks like a memset() operation,
    // since those can reduce to loops with high trip counts and many many solver calls.
    if (LooksLikeMemsetLoop(_currEntry->kinst()->inst)) {
      stats.skipSolver++;
      isLive = true;
    } else {
      ref<Expr> e_lower = (*e)->offset();
      ref<Expr> e_upper = klee::AddExpr::create((*e)->offset(), (*e)->nbytes());

      stats.solverCheckMust++;

      // Must completely overlap?
      if (SolverMustBeTrue(klee::AndExpr::create(klee::UleExpr::create(store_lower, e_lower),
                                                 klee::UleExpr::create(e_upper, store_upper)))) {
        stats.solverFoundMust++;
        goto overwritten;
      }

      stats.solverCheckMay++;

      // May partially overlap? (skip if we already know this is live)
      if (!isLive &&
          SolverMayBeTrue(klee::AndExpr::create(klee::UltExpr::create(store_lower, e_upper),
                                                klee::UltExpr::create(e_lower, store_upper)))) {
        stats.solverFoundMay++;
        isLive = true;
      }
    }


  notoverwritten:
    ++e;
    continue;

  overwritten:
    isLive = true;
    // This load has been completely overwritten, so remove it
    // ... remove from _liveLoads
    bool found = RemoveFromLiveLoads(*e);
    assert(found);
    // ... remove from _liveMem
    e = entries.erase(e);
    continue;
  }

  if (entries.empty())
    _liveMem.erase(it);

  return isLive;
}

bool PrecondSliceComputor::UpdateLiveMemForAlloc(const PrecondSliceTrace::Entry &alloc) {
  assert(alloc.kind() == PrecondSliceTrace::Allocation);

  // Is the MemoryObject live?
  livemem_ty::iterator it = _liveMem.find(alloc.memoryobject());
  if (it == _liveMem.end())
    return false;

  entry_list_ty &entries = it->second;

  // This MemoryObject is no longer live, as it was just allocated, so
  // remove all loads of the object
  for (entry_list_ty::iterator e = entries.begin(); e != entries.end(); /* no inc here */) {
    // ... remove from _liveLoads
    bool found = RemoveFromLiveLoads(*e);
    assert(found);
    // ... remove from _liveMem
    e = entries.erase(e);
  }

  assert(entries.empty());
  _liveMem.erase(it);
  return true;
}

bool PrecondSliceComputor::RemoveFromLiveLoads(const PrecondSliceTrace::Entry *e) {
  // Remove from e from _liveLoads
  for (liveloads_ty::iterator ll = _liveLoads.lower_bound(e->kinst()->inst);
                              ll != _liveLoads.end() && ll->first == e->kinst()->inst; ++ll) {
    if (ll->second == e) {
      _liveLoads.erase(ll);
      return true;
    }
  }

  return false;
}

bool PrecondSliceComputor::SolverMayBeTrue(ref<Expr> expr) {
  bool result;
  _kleeSolver->setTimeout(klee::clvars::MaxSTPTime);
  if (!_kleeSolver->mayBeTrue(*_kleeFinalState, expr, result)) {
    result = true;      // solver failure: conservative response for "may be" is "true"
  }
  _kleeSolver->setTimeout(0);
  return result;
}

bool PrecondSliceComputor::SolverMustBeTrue(ref<Expr> expr) {
  bool result;
  _kleeSolver->setTimeout(klee::clvars::MaxSTPTime);
  if (!_kleeSolver->mustBeTrue(*_kleeFinalState, expr, result)) {
    result = false;     // solver failure: conservative response for "must be" is "false"
  }
  _kleeSolver->setTimeout(0);
  return result;
}

void PrecondSliceComputor::GetTargetsForBranch(const PrecondSliceTrace::Entry *e,
                                               BranchTargets *targets) {
  assert(e->kind() == PrecondSliceTrace::Branch);

  llvm::BasicBlock *srcBB = e->kinst()->inst->getParent();
  llvm::BasicBlock *dstBB = e->targetKinst()->inst->getParent();

  targets->takenBB = dstBB;
  targets->other.clear();

  // The symbolic condition for this branch
  if (e->hasCondition()) {
    targets->takenCond = e->condition();
  } else {
    targets->takenCond = NULL;
  }

  // Explicity specified flips? (this happens for switch insts)
  if (e->useFlippedConditions()) {
    for (FlippedConditionsMapTy::iterator
         it = e->flippedConditions().begin(); it != e->flippedConditions().end(); ++it) {
      assert(it->second);
      targets->other[it->second] = it->first;
    }
    return;
  }

  // Implicitly specified flip?
  // In this case, "other" includes one branch with "!takenCond" for the expr
  if (e->hasCondition()) {
    llvm::BranchInst *src = cast<BranchInst>(e->kinst()->inst);
    assert(src->isConditional());
    llvm::BasicBlock *otherBB = src->getSuccessor(0);
    if (otherBB == dstBB) {
      otherBB = src->getSuccessor(1);
      if (otherBB == dstBB) {
        llvm::errs() << "UNEXPECTED: Conditional BranchInst has one successor?\n"
                     << *src << "\n";
        _algorithm_Stats.branchesSkippedTrivial++;
        targets->takenCond = Expr::True;
        return;
      }
    }

    assert(otherBB);
    targets->other[otherBB] = Expr::createIsZero(e->condition());
    return;
  }

  // No symbolic condition
  // For "other", just enumerate all successors that are not "dst"
  for (succ_iterator succ = succ_begin(srcBB); succ != succ_end(srcBB); ++succ) {
    if (*succ != dstBB)
      targets->other[*succ] = NULL;
  }
}

bool PrecondSliceComputor::Postdominates(llvm::Instruction *end, llvm::Instruction *beg) {
  assert(end);
  assert(beg);

  // Does 'end' postdominate 'beg'?
  llvm::BasicBlock *endBlock = end->getParent();
  llvm::BasicBlock *begBlock = beg->getParent();
  assert(endBlock);
  assert(begBlock);

  // For our purposes, 'beg' is a branch at the end of 'begBlock'
  if (begBlock == endBlock)
    return false;

  llvm::Function *f = endBlock->getParent();
  assert(f);
  assert(f == begBlock->getParent());

  DominatorTree *dt = GetPostdominatorTree(f);
  assert(dt);

  return dt->dominates(endBlock, begBlock);
}

// TODO: We are not handling the case where loads in `_liveloads` were generated
//   internally by Klee to implement instrinsic functions (notably, Intrinsic::vastart).
//   These show as CallInsts in `_liveLoads`, rather than LoadInsts.  This is a source
//   of unsoundness (not capturing all data dependencies).

bool PrecondSliceComputor::PathMaySyncOrWriteLiveMem(llvm::BasicBlock *begBlock,
                                                     llvm::Instruction *end,
                                                     bool mightReturn) {
  assert(begBlock);
  assert(end);

  PathMaySyncOrWriteLiveMem_Stats& stats = _pathMaySyncOrWriteLiveMem_Stats;
  stats.total++;

  llvm::SmallPtrSet<const llvm::Function*,16> callees;
  std::vector<const llvm::Function*> calltargets;

  llvm::SmallPtrSet<llvm::BasicBlock*,16> pushed;
  std::stack<llvm::BasicBlock*> stack;

  //
  // Return true if both of the following hold:
  //   (1) All paths starting from 'beg' end at 'end or at an unreachable terminator
  //   (2) None of those paths ever perform synchronization or write to liveMem
  //
  // If !mightReturn, we can assume that 'end' postdominates 'beg', though we
  // really don't take advantage of that besides adding a few assertions.
  //
  // N.B.: We don't need to do anything special for context switches (e.g., not
  //   even there was a context switch between 'beg' and 'end' in the trace)
  //   because we assume the program is data race free.
  //

  // Feed the DFS with the initial block
  stack.push(begBlock);
  pushed.insert(begBlock);

  // DFS
  while (!stack.empty()) {
    llvm::BasicBlock *bb = stack.top();
    stack.pop();

    bool foundEnd = false;

    // Look for sync ops and stores in this block
    // Check if the stores alias any live loads
    for (BasicBlock::iterator inst = bb->begin(); inst != bb->end(); ++inst) {
      if (&*inst == end) {
        assert(end);
        foundEnd = true;
        break;
      }

      switch (inst->getOpcode()) {
      case Instruction::Ret:
      case Instruction::Unwind:
        assert(mightReturn);
        if (ExtraDebugEnabled)
          llvm::errs() << ExtraDebugEnabled << " via return";
        return true;

      case Instruction::Call:
      case Instruction::Invoke: {
        // Direct call to a sync op?
        CallSite CS(inst);
        const Function* targetF = dyn_cast<Function>(CS.getCalledValue()->stripPointerCasts());
        if (targetF && IsSyncOpCaller(targetF)) {
          stats.withSync++;
          if (ExtraDebugEnabled)
            llvm::errs() << ExtraDebugEnabled << " via sync in " << targetF->getName();
          return true;
        }
        // Enumerate all callees
        calltargets.clear();
        TheAARunner.getDSAA()->getCallTargets(CS, &calltargets);
        for (size_t i = 0; i < calltargets.size(); ++i) {
          // Indirect call to a sync op?
          if (IsSyncOpCaller(calltargets[i])) {
            stats.withSync++;
            if (ExtraDebugEnabled)
              llvm::errs() << ExtraDebugEnabled << " via sync in " << calltargets[i]->getName();
            return true;
          }
          // Save for later: will check if it writes to live mem
          callees.insert(calltargets[i]);
        }
        break;
      }

      case Instruction::Store: {
        stats.storesChecked++;
        AliasAnalysis::Location storeLoc = AA->getLocation(cast<StoreInst>(inst));
        liveloads_ty::iterator curr = _liveLoads.begin();
        liveloads_ty::iterator last = _liveLoads.end();
        for (; curr != _liveLoads.end(); last = curr, ++curr) {
          // Skip repeated loads (_liveLoads is a multimap)
          if (last != _liveLoads.end() && curr->first == last->first)
            continue;
          // TODO: see note above about Intrinsic::vastart
          const llvm::LoadInst *load = dyn_cast<LoadInst>(curr->first);
          if (!load)
            continue;
          // FIXME: another problem: memset/memcpy!!!
          // XXX: ugly: we try to remove these from liveloads, but they can stay in
          // via context-switches (e.g., while one thread is sleeping on a convar)
          // XXX: rationale: it is assumed that the set of variables written by the
          // "ignored functions" is disjoint from the rest of the program
          // XXX: rationale is not entirely true! could it be the memset problem?
          if (DSAA->isSteensIgnoredFunction(load->getParent()->getParent()) &&
              !DSAA->isSteensIgnoredFunction(inst->getParent()->getParent())) {
            continue;  // no alias
          }
          stats.aliasQueries++;
          AliasAnalysis::Location loadLoc = AA->getLocation(load);
          AliasAnalysis::AliasResult res = TheAACache.alias(storeLoc, loadLoc);
          if (res != AliasAnalysis::NoAlias) {
            stats.withLiveStore++;
            if (ExtraDebugEnabled) {
              llvm::errs() << ExtraDebugEnabled << " via store @ " << *inst;
              llvm::errs() << ExtraDebugEnabled << " aliasing load @ " << *load;
            }
            return true;
          }
        }
        break;
      }

      default:
        // nop
        break;
      }
    }

    // Can ignore successors if we reached an 'endpoint' within this block
    if (foundEnd)
      continue;

    // Push successors
    for (succ_iterator succ = succ_begin(bb); succ != succ_end(bb); ++succ) {
      if (pushed.count(*succ) == 0) {
        stack.push(*succ);
        pushed.insert(*succ);
      }
    }
  }

  // Now check if any callees along the path can write to any live loads
  for (llvm::SmallPtrSet<const llvm::Function*,16>::iterator
       F = callees.begin(); F != callees.end(); ++F) {
    stats.calleeQueries++;
    if (FunctionMayWriteLiveMem(*F)) {
      stats.withLiveStoreViaCall++;
      if (ExtraDebugEnabled)
        llvm::errs() << ExtraDebugEnabled << " via memop in " << (*F)->getName();
      return true;
    }
  }

  return false;
}

bool PrecondSliceComputor::PathsMaySyncOrWriteLiveMem(BranchTargets *targets,
                                                      llvm::Instruction *end,
                                                      bool mightReturn) {
  bool found = false;

  //
  // For each branch target T specified by "targets", we check if
  // any path from T --> end might perform sync or write to live mem.
  // We return true iff such a path exists.
  //
  // For each T where T --> end might perform sync or write to live mem,
  // we keep T in the "other" branch edges set.  For all other Ts, we
  // remove T from the "other" set add the corresponding T.condition to
  // "takenCond" via a disjunction.
  //
  // For example, given
  //   switch(x) {
  //   case 1: sync
  //   case 2: break
  //   case 3: sync
  //   case 4: break
  //   }
  //
  // If we explore "case 2", our input is:
  //   targets->taken :: { case2 --> x==2  }
  //   targets->other :: { case1 --> x==1, case3 --> x==3, case4 --> x==4 }
  //
  // and our final output is:
  //   targets->taken :: { case2 --> x==2||x==4  }
  //   targets->other :: { case1 --> x==1, case3 --> x==3 }
  //
  // FIXME TODO: We can do better.
  // We want this optimization to trigger in pfscan on the following switch:
  //    switch (f) {
  //      case FTW_F:   ...; return 0
  //      case FTW_D:   return 0
  //      case FTW_DNR: return 1
  //      case FTW_NS:  return 1
  //      default:      return 1
  //    }
  // The return value is "live" as it affects the sync schedule.  However, the
  // last three cases should be mergeable as they return the same value and do
  // not otherwise write different values to live regs/mem.
  //
  // In general, for all paths from a given branch target, instead of simply
  // checking whether any of those paths may write to live regs/mem, we can
  // enumerate the set of pairs (loc,val), where value "val" may-be-written to
  // the live reg/mem specified by "loc".  Then we can merge all branch targets
  // that share the same set of live writes {(loc,val)}.  In the above example,
  // we can merge the last three cases because their live-write sets are all the
  // same, namely, {(%ret,1)}.  This approach would also need more careful
  // handling of branches and phi nodes, as currently we always "take" branch
  // instructions when the target phi is live, but this updated scheme would
  // handle that case by tracking the set of writes to the phis on each path.
  //
  // This approach might be expanded more generally to abstract over a set
  // of path fragments that not only make the same live writes, but that also
  // perform the same sequence of synchronization operations.
  //

  for (BranchTargets::MapTy::iterator
       it = targets->other.begin(); it != targets->other.end(); /* nop */) {
    assert(it->first);
    assert(it->second.isNull() == targets->takenCond.isNull());
    if (PathMaySyncOrWriteLiveMem(it->first, end, mightReturn)) {
      found = true;
      ++it;
    } else {
      BranchTargets::MapTy::iterator next = it;
      ++next;
      if (targets->takenCond.get()) {
        if (0) {
          llvm::errs() << "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ\n"
                       << "TakenCond (@ " << targets->takenBB->getName() << "):\n"
                       << targets->takenCond << "\n"
                       << "AddingCond (@ " << it->first->getName() << "):\n"
                       << it->second << "\n";
        }
        targets->takenCond = klee::OrExpr::create(it->second, targets->takenCond);
      }
      targets->other.erase(it);
      it = next;
    }
  }

  return found;
}

bool PrecondSliceComputor::FunctionMayWriteLiveMem(const llvm::Function *F) {
  assert(F);

  FunctionMayWriteLiveMem_Stats& stats = _functionMayWriteLiveMem_Stats;
  stats.total++;

  // Read-only functions never write to memory
  AliasAnalysis::ModRefBehavior behavior = AA->getModRefBehavior(F);
  if ((behavior & AliasAnalysis::Mod) == 0) {
    stats.readOnly++;
    return false;
  }

  // Now query alias analysis for each live load
  liveloads_ty::iterator curr = _liveLoads.begin();
  liveloads_ty::iterator last = _liveLoads.end();
  for (; curr != _liveLoads.end(); last = curr, ++curr) {
    // Skip repeated loads (_liveLoads is a multimap)
    if (last != _liveLoads.end() && curr->first == last->first)
      continue;
    // TODO: see note above about Intrinsic::vastart
    const llvm::LoadInst *load = dyn_cast<LoadInst>(curr->first);
    if (!load)
      continue;
    // XXX: ugly: we try to remove these from liveloads, but they can stay in
    // via context-switches (e.g., while one thread is sleeping on a convar)
    // XXX: rationale: it is assumed that the set of variables written by the
    // "ignored functions" is disjoint from the rest of the program
    if (DSAA->isSteensIgnoredFunction(load->getParent()->getParent()) &&
        !DSAA->isSteensIgnoredFunction(F)) {
      continue;  // no alias
    }
    stats.modrefQueries++;
    AliasAnalysis::Location loadLoc = AA->getLocation(load);
    if (TheAACache.functionMayWriteLocation(F, loadLoc)) {
      stats.withLiveStore++;
      return true;
    }
  }

  return false;
}

bool PrecondSliceComputor::CallTargetsMaySyncOrWriteLiveMem(CallSite cs,
                                                            const llvm::Function *ignoreF) {
  // Enumerate the static call-targets of cs, ignoring "ignoreF"
  // Return true if any of those targets might sync or write live memory
  std::vector<const llvm::Function*> targets;
  TheAARunner.getDSAA()->getCallTargets(cs, &targets);

  for (size_t k = 0; k < targets.size(); ++k) {
    const llvm::Function *f = targets[k];
    if (f != ignoreF && (IsSyncOpCaller(f) || FunctionMayWriteLiveMem(f)))
      return true;
  }

  return false;
}

void PrecondSliceComputor::ResetStats() {
  memset(&_updateLiveMemForStore_Stats, 0, sizeof _updateLiveMemForStore_Stats);
  memset(&_pathMaySyncOrWriteLiveMem_Stats, 0, sizeof _pathMaySyncOrWriteLiveMem_Stats);
  memset(&_functionMayWriteLiveMem_Stats, 0, sizeof _functionMayWriteLiveMem_Stats);
  memset(&_algorithm_Stats, 0, sizeof _algorithm_Stats);
  TheAACache.resetStats();
}

void ComputePrecondSlice(klee::Executor *executor,
                         klee::ExecutionState *finalState,
                         DepthFirstICS *strat,
                         PrecondSliceTrace *slice) {
  assert(strat);
  assert(slice);

  static size_t totalPaths = 0;
  CLOUD9_INFO("Computing PrecondSlice "
              << "(size=" << finalState->precondSliceTrace.size()
              << ", totalPaths=" << (++totalPaths) << ")");
  klee::WallTimer timer;

  PrecondSliceComputor c;
  c._strat = strat;
  c._slice = slice;
  c._kleeSolver = executor->getTimingSolver();;
  c._kleeFinalState = finalState;
  c.ResetStats();
  c.Compute(finalState->precondSliceTrace);

  // Slice entries are pushed end->start, reverse to make them start->end
  slice->reverse();

  // Inform the strat of any uncovered loops that had symbolic conditions
  strat->shouldUpgradeUncoveredLoops(c._loopsWithUncoveredConditions);

  if (DumpPrecondSlicingStats) {
    uint64_t endtime = timer.check();
    std::cerr << "ComputePrecondSlice() Stats"
              << "\n > Running Time"
              << "\n     total:     " << double(endtime)/1000000.0 << "s "
              << "\n     per-entry: " << double(endtime)/double(finalState->precondSliceTrace.trace().size()) << "us"
              << "\n > Algorithm"
              << "\n     instr-traced:  " << c._algorithm_Stats.instrs
              << "\n     instr-taken:   " << c._algorithm_Stats.instrsTaken
              << "\n     instr-skipped: " << c._algorithm_Stats.instrsSkipped
              << "\n     fn-call-skipped: " << c._algorithm_Stats.fnCallsSkipped
              << "\n     sync-ops: " << c._algorithm_Stats.syncOps
              << "\n     branches-taken:           " << c._algorithm_Stats.branchesTaken
              << "\n     branches-taken-live:      " << c._algorithm_Stats.branchesTakenLive
              << "\n     branches-taken-with-cond: " << c._algorithm_Stats.branchesTakenWithSymbolicCondition
              << "\n     branches-taken-new-flips: " << c._algorithm_Stats.branchesTakenWithUpdatedFlips
              << "\n     branches-skipped-with-cond:   " << c._algorithm_Stats.branchesSkippedWithSymbolicCondition
              << "\n     branches-skipped-uncondition: " << c._algorithm_Stats.branchesSkippedUnconditional
              << "\n     branches-skipped-precond-opt: " << c._algorithm_Stats.branchesSkippedPrecondOpt
              << "\n     branches-skipped-exit-opt:    " << c._algorithm_Stats.branchesSkippedExitOpt
              << "\n     branches-skipped-trivial:     " << c._algorithm_Stats.branchesSkippedTrivial
              << "\n     resolved-ptrs-taken: " << c._algorithm_Stats.resolvedPtrsTaken
              << "\n > UpdateLiveMemForStore()"
              << "\n     total:        " << c._updateLiveMemForStore_Stats.total
              << "\n     obj-not-live: " << c._updateLiveMemForStore_Stats.objNotLive
              << "\n     obj-live:     " << (c._updateLiveMemForStore_Stats.total - c._updateLiveMemForStore_Stats.objNotLive)
              << "\n     overlap-checks:    " << c._updateLiveMemForStore_Stats.overlapChecks
              << "\n     const-checks:      " << c._updateLiveMemForStore_Stats.constChecks
              << "\n     const-found-must:  " << c._updateLiveMemForStore_Stats.constFoundMust
              << "\n     const-found-may:   " << c._updateLiveMemForStore_Stats.constFoundMay
              << "\n     skip-solver:       " << c._updateLiveMemForStore_Stats.skipSolver
              << "\n     solver-check-must: " << c._updateLiveMemForStore_Stats.solverCheckMust
              << "\n     solver-found-must: " << c._updateLiveMemForStore_Stats.solverFoundMust
              << "\n     solver-check-may:  " << c._updateLiveMemForStore_Stats.solverCheckMay
              << "\n     solver-found-may:  " << c._updateLiveMemForStore_Stats.solverFoundMay
              << "\n > PathMaySyncOrWriteLiveMem()"
              << "\n     total:      " << c._pathMaySyncOrWriteLiveMem_Stats.total
              << "\n     with-sync:  " << c._pathMaySyncOrWriteLiveMem_Stats.withSync
              << "\n     with-store: " << c._pathMaySyncOrWriteLiveMem_Stats.withLiveStore
              << "\n     with-store-viacall: " << c._pathMaySyncOrWriteLiveMem_Stats.withLiveStoreViaCall
              << "\n     stores-checked: " << c._pathMaySyncOrWriteLiveMem_Stats.storesChecked
              << "\n     alias-queries:  " << c._pathMaySyncOrWriteLiveMem_Stats.aliasQueries
              << "\n     callee-queries: " << c._pathMaySyncOrWriteLiveMem_Stats.calleeQueries
              << "\n > FunctionMayWriteLiveMem()"
              << "\n     total:      " << c._functionMayWriteLiveMem_Stats.total
              << "\n     read-only:  " << c._functionMayWriteLiveMem_Stats.readOnly
              << "\n     with-store: " << c._functionMayWriteLiveMem_Stats.withLiveStore
              << "\n     modref-queries: " << c._functionMayWriteLiveMem_Stats.modrefQueries
              << "\n > TheAACache"
              << "\n     alias-cache-hit:  " << TheAACache.nAliasCacheHit()
              << "\n     alias-cache-miss: " << TheAACache.nAliasCacheMiss()
              << "\n     alias-cache-size: " << TheAACache.nAliasCacheSize()
              << "\n     modref-cache-hit:  " << TheAACache.nModRefCacheHit()
              << "\n     modref-cache-miss: " << TheAACache.nModRefCacheMiss()
              << "\n     modref-cache-size: " << TheAACache.nModRefCacheSize()
              << "\n     modref-cache-hit-transitive:  " << TheAACache.nModRefCacheHitTransitive()
              << "\n     modref-cache-miss-transitive: " << TheAACache.nModRefCacheMissTransitive()
              << "\n";
  }
}

//-----------------------------------------------------------------------------
// Symbolic execution of parallel regions
// To initialize a region, we wipe all state from the heap, leaving just globals
// and the stack.  We then make the remaining state symbolic (with some exceptions
// as detailed below).
//-----------------------------------------------------------------------------

bool IsObjectModified(const llvm::Value *obj) {
  assert(obj);

  // Return true if the object is a pointer that may-be-modified.
  // We will trust the LLVM 'constant' attribute (the most common use
  // of this attribute is for static strings generated by the compiler)
  const GlobalVariable *var = dyn_cast<GlobalVariable>(obj);
  if (var && var->isConstant()) {
    // Sanity check: DSAA should agree
    //if (DSAA->mayBeModified(obj)) {
    //  llvm::errs() << "WARNING: constant GlobalVariable may-be-modified "
    //               << "(possible analysis imprecision): " << (*var) << "\n";
    //}
    return false;
  }

  // Now just query DSAA
  // FIXME: This is broken: it appears to not reason correctly about objects
  // that are never written by the main thread but are written be other threads
  //return DSAA->mayBeModified(obj);
  return true;
}

klee::ThreadUid
ThreadIdForThreadLocalObject(klee::ExecutionState *kstate,
                             const klee::MemoryObject *mo,
                             const llvm::GlobalValue *var) {
  typedef klee::ExecutionState::threads_ty ThreadsTy;

  for (ThreadsTy::const_iterator
       it = kstate->threads.begin(); it != kstate->threads.end(); ++it) {
    const klee::Cell *cell = kstate->lookupThreadLocalAddress(it->first, var);
    assert(cell);
    klee::ObjectPair op;
    bool ok = kstate->addressSpace().findAddress(cast<klee::ConstantExpr>(cell->value), &op);
    assert(ok);
    if (op.first == mo)
      return it->first;
  }

  llvm::errs() << "???: " << *var << "\n";
  // XXX assert(0);
  return klee::ThreadUid(0,2);
}

void ResetStateForParallelRegion(klee::ExecutionState *kstate) {
  // stupid typedefs
  typedef klee::ExecutionState::threads_ty ThreadsTy;
  typedef klee::ExecutionState::processes_ty ProcessesTy;
  typedef klee::ExecutionState::stack_ty StackTy;

  using klee::MemoryMap;
  using klee::MemoryObject;
  using klee::ObjectPair;
  using klee::ObjectState;
  using klee::Process;

  // TODO: what about zombies?
  // TODO: zombie threads happen in dedup: one thread exits, but stays around
  // waiting for others to join; then the region ends; next region, we try to
  // execute that thread, but it's sitting on the "unreachable" instruction
  // following klee_thread_terminate(); perhaps that's the way to notice this
  // condition?  Hmm ... why were we trying to execute a thread that is a zombies?

  // Clear state
  // TODO: what other state needs to be cleared?
  kstate->symbolics = klee::ExecutionState::SymbolicsSetTy();
  kstate->globalConstraints.clear();

  kstate->inputHarnessTrace.clear();
  kstate->inputHarnessConstraints.clear();
  kstate->inputHarnessObjects.clear();
  kstate->initializedAddrs = klee::ExecutionState::InitializedAddrsMap();
  kstate->resolvedSymbolicPtrs.clearPathState();

  kstate->ctxSwitchesToReplay = std::queue<klee::ThreadUid>();
  kstate->hbGraph.clear();
  kstate->precondSliceTrace.clear();
  kstate->clearWaitingAtIcsRegion();

  kstate->locksets.clear();
  kstate->barrierInitCounts.clear();

  // This shouldn't be called from partial eval!
  assert(kstate->partialEvalMode == 0);

  // Re-emit some fundamental invariants
  kstate->executor->addThreadIdConstraints(*kstate);

  //
  // --- 1 ---
  // For each process, unbind all memory objects except globals.
  // The discarded objects will be rebound either:
  //   (a) during paritial-evaluation, when we re-execute calls to alloca and malloc, or
  //   (b) during symbolic ptr resolution, when we fork for a fresh object
  //
  // N.B.: In klee parlance, a "global" is any non-alloca, non-thread-local pointer
  // that cannot be passed to free().  Notably, this includes argv[] and all pointers
  // therein.  We discard everything except for the true GlobalVariables.
  //
  for (ProcessesTy::iterator
       p = kstate->processes.begin(); p != kstate->processes.end(); ++p) {
    MemoryMap fresh;
    MemoryMap &old = p->second.addressSpace.objects;

    for (MemoryMap::iterator mm = old.begin(); mm != old.end(); ++mm) {
      const MemoryObject *mo = mm->first;
      const bool check = mo->isGlobalOrThreadLocal() && mo->allocSite;
      // Discard all objects with a non-global allocSite
      if (!check || !isa<GlobalVariable>(mo->allocSite)) {
        if (check)
          llvm::errs() << "WARNING: discarding global object: " << *mo->allocSite << "\n";
        continue;
      }
      fresh = fresh.insert(std::make_pair(mm->first, mm->second));
    }

    p->second.addressSpace.objects = fresh;
  }

  //
  // --- 2 ---
  // Make all global memory objects symbolic
  // This is not necessary when:
  //   (a) the object is not shared by multiple address spaces
  //   (b) and either:
  //         -- the object is read-only, or
  //         -- the object is never modifed according to alias analysis
  //
  std::tr1::unordered_set<const ObjectState*> done;

  for (ProcessesTy::iterator
       pIt = kstate->processes.begin(); pIt != kstate->processes.end(); ++pIt) {
    Process &p = pIt->second;

    for (MemoryMap::iterator
         mm = p.addressSpace.objects.begin(); mm != p.addressSpace.objects.end(); ++mm) {
      const MemoryObject *mo = mm->first;
      ObjectState *os = mm->second;
      if (done.count(os))
        continue;

      done.insert(os);

      // Can keep concrete?
      // Must be a read-only unshared global
      if (!os->isShared() && mo->isGlobal() &&
          (os->isReadOnly() || !IsObjectModified(mo->allocSite)))
        continue;

      // Make this object symbolic
      const llvm::GlobalVariable *var = cast<GlobalVariable>(mo->allocSite);
      const klee::Array *array;
      if (var->isThreadLocal()) {
        klee::ThreadUid tuid = ThreadIdForThreadLocalObject(kstate, mo, var);
        array = kstate->symbolicObjects.getUniqueArrayForThreadLocalVar(var, mo, tuid);
      } else {
        array = kstate->symbolicObjects.getUniqueArrayForGlobalVar(var, mo);
      }
      os->initializeToSymbolic(array);
    }
  }

  //
  // --- 3 ---
  // Remove all virtual registers and local allocations, including all function
  // args.  All removed registers will be regenerated during partial evaluation.
  //
  for (ThreadsTy::iterator
       t = kstate->threads.begin(); t != kstate->threads.end(); ++t) {
    for (StackTy::iterator
         sf = t->second.getStack().begin(); sf != t->second.getStack().end(); ++sf) {
      sf->allocas.clear();
      for (unsigned reg = 0; reg < sf->kf->numRegisters; ++reg) {
        sf->locals[reg].value = NULL;
      }
      sf->varargs = NULL;
    }
  }
}

void GeneralizeStateSyncInvariants(klee::ExecutionState *kstate) {
  typedef klee::ExecutionState::threads_ty ThreadsTy;
  typedef klee::ExecutionState::stack_ty StackTy;

  // N.B.: No need to generalize the barrierInitCounts since an "empty" set
  // is already the most general representation.

  // Generalize the locksets
  for (ThreadsTy::iterator t = kstate->threads.begin(); t != kstate->threads.end(); ++t) {
    kstate->locksets[t->first].setIncludesAll();
  }
}

void InitializeStateForParallelRegion(klee::ExecutionState *kstate) {
  klee::Executor *executor = kstate->executor;

  // TODO
  kstate->stateTime = 0;
  kstate->depth = 0;
  kstate->totalBranches = 0;
  kstate->perThreadTime.clear();
  kstate->crossedIcsLoopedRegion.clear();
  kstate->oncePerRegionCalls = klee::ImmutableSet<klee::KInstruction*>();

  switch (IcsExecMode) {
  case IcsExecFromMiddleViaConcretePath:
    // Clear a minimal amount of state
    kstate->hbGraph.clear();            // onIcsBegin() wants this to be empty
    kstate->inputHarnessTrace.clear();  // onIcsBegin() wants this to be empty
    kstate->clearWaitingAtIcsRegion();  // must do this or we deadlock in onIcsBegin()
    // No analysis here: we reuse the concrete state from the end of the prior region
    executor->fireIcsBegin(kstate);
    return;

  case IcsNormal:
    if (DoRegionStateViaDataflow) {
      goto dodataflow;
    } else {
      goto dounconstrained;
    }

  case IcsExecFromMiddleViaDataflowAnalysis:
  dodataflow:
    ResetStateForParallelRegion(kstate);
    executor->prepareToPartialEvalState(*kstate);
    return;

  case IcsExecFromMiddleUnconstrained:
  dounconstrained:
    // As above, but does not do partial eval
    // Since we don't do partial eval, we must go through the stacks and assign
    // a fresh symbolic to each virtual register and function argument.  We also
    // allocate vararg objects where needed so the VaArg instruction does not fail.
    ResetStateForParallelRegion(kstate);
    BuildSymbolicStacks(kstate);
    // We also generalize synchronization invariants to the "any" state; i.e., each
    // thread may hold any lock, and each barrier may have an unknown init count.
    GeneralizeStateSyncInvariants(kstate);
    executor->fireIcsBegin(kstate);
    return;

  default:
    assert(0);
  }
}

}  // namespace

void BuildSymbolicStacks(klee::ExecutionState *kstate) {
  typedef klee::ExecutionState::threads_ty ThreadsTy;
  typedef klee::ExecutionState::stack_ty StackTy;
  typedef klee::CallingContext CallingContext;

  for (ThreadsTy::iterator
       t = kstate->threads.begin(); t != kstate->threads.end(); ++t) {
    const CallingContext *callctx = CallingContext::Empty();
    for (StackTy::iterator
         sf = t->second.getStack().begin(); sf != t->second.getStack().end(); ++sf) {
      if (sf->caller) {
        callctx = CallingContext::Append(callctx, sf->caller);
      }
      // XXX: DUMB: Due to the super ugly points-to analysis, we'll cheat
      // and keep the reaching-defs computed for these ignored fns as
      // otherwise, any ptr within these fns will alias anything and kill perf
      klee::KFunction *kf = sf->kf;
      if (DSAA->isSteensIgnoredFunction(kf->function))
        continue;
      // Make symbolics for args
      llvm::Function::arg_iterator arg;
      unsigned k;
      for (arg = kf->function->arg_begin(), k = 0; k < kf->numArgs; ++k, ++arg) {
        sf->locals[kf->getArgRegister(k)].value =
          kstate->symbolicObjects.getExprForLocalVar(callctx, t->first, arg);
      }
      // Make symbolics for insts
      klee::KInstIterator ki;
      for (ki = kf->instructions, k = 0; k < kf->numInstructions; ++k, ++ki) {
        if (ki->inst->getType()->isVoidTy())
          continue;
        sf->locals[ki->dest].value =
          kstate->symbolicObjects.getExprForLocalVar(callctx, t->first, ki->inst);
      }
      // Make varargs object
      // FIXME?: the SymbolicObjectTable currently does not support a "VaArg" type
      // for SymbolicObjects, so for now we'll assume we don't run into vararg fns
      // in this execution mode (and note that this mode is used for experiments only)
      assert(!kf->function->isVarArg());
    }

    // If the thread begins life asleep, setup its currWaitingList
    if (!t->second.isEnabled()) {
      klee::KInstruction *ki = t->second.getPrevPC();
      if (const llvm::CallInst *call = dyn_cast<CallInst>(ki->inst)) {
        const llvm::Function *f = call->getCalledFunction();
        if (f && f->getName() == "klee_thread_sleep") {
          assert(call->getNumArgOperands() == 3);
          klee::Thread &thread = t->second;
          // Eval all arguments to this function
          klee::CrtThreadSaver savedThread(*kstate, thread);
          std::vector<ref<Expr> > arguments;
          for (int k = 0; k < 3; ++k) {
            arguments.push_back(kstate->executor->eval(ki, k+1, *kstate).value);
          }
          // Update the wl
          ref<Expr> wlistSlot = kstate->symbolicObjects.getExprForWaitSlot(thread.getTuid());
          thread.updateWaitingListAtSleep(arguments, wlistSlot);
        }
      }
    }
  }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

}  // namespace ics
