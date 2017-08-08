/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * A search strategy for computing input covering schedules.
 *
 */

#ifndef ICS_SEARCHSTRATEGY_H
#define ICS_SEARCHSTRATEGY_H

#include "ics/HBGraph.h"
#include "ics/PrecondSliceTrace.h"
#include "ics/PathManager.h"

#include "klee/Expr.h"
#include "klee/SymbolicObjectTable.h"
#include "cloud9/worker/SymbolicEngine.h"
#include "cloud9/worker/CoreStrategies.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/DominanceFrontier.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_os_ostream.h"

#include <map>
#include <stack>
#include <vector>
#include <tr1/unordered_map>

namespace ics {

extern llvm::cl::opt<bool> DoRegionFormation;
extern llvm::cl::opt<bool> DbgSymbolicPaths;
extern llvm::cl::opt<bool> OptNoConcretizationConstraintsInSlice;
extern llvm::cl::opt<bool> OptIgnoreUclibcSync;

bool IsIgnorableSyncOpCaller(const llvm::Function *f);
void InitializePrecondSlice(klee::KModule *kmodule);
void FinalizePrecondSlice();
void RemoveDormantRegionMarkers(const bool verbose);
void BuildSymbolicStacks(klee::ExecutionState *kstate);

llvm::DominatorTreeBase<llvm::BasicBlock>* GetDominatorTree(llvm::Function *f);
llvm::DominatorTreeBase<llvm::BasicBlock>* GetPostdominatorTree(llvm::Function *f);
llvm::LoopInfoBase<llvm::BasicBlock, llvm::Loop>* GetLoopInfo(llvm::Function *f);

klee::KInstruction* LookupKInstruction(const klee::ExecutionState *kstate,
                                       const llvm::Instruction *inst);

bool MayHappenAfterThreadCreation(const klee::Thread &thread,
                                  const llvm::Instruction *pc = NULL);

void DumpInstruction(const char *prefix, const llvm::Instruction *inst);
void DumpValue(const char *prefix, const llvm::Value *val);
void DumpKInstruction(const char *prefix, klee::KInstruction *kinst);
void DumpKInstruction(const char *prefix, klee::KInstruction *kinst,
                      llvm::raw_ostream &out, bool lineNoOnly = false);
void DumpTuid(const char *prefix, klee::ThreadUid tuid);
std::string KleeForkClassToString(klee::ForkClass fc);

//-----------------------------------------------------------------------------
// An event handler that logs a trace to ExecutionState::precondSliceTrace
//-----------------------------------------------------------------------------

class PrecondSliceTraceLogger : public cloud9::worker::StateEventHandler {
public:
  virtual ~PrecondSliceTraceLogger() {}

  // Important
  virtual void onConstraintAdded(klee::ExecutionState *kstate,
                                 klee::ref<klee::Expr> condition,
                                 klee::ForkClass forkclass);

  virtual void onControlFlowEvent(klee::ExecutionState *kstate,
                                  cloud9::worker::ControlFlowEvent event,
                                  klee::ThreadUid targetTuid,
                                  klee::KInstruction *targetKinst,
                                  llvm::Function *f);

  virtual void onMemoryAccess(klee::ExecutionState *kstate,
                              const klee::MemoryObject *mo,
                              klee::ref<klee::Expr> offset,
                              klee::ref<klee::Expr> nbytes,
                              bool isStore,
                              bool isAlloc);

  virtual void onResolvedSymbolicPtr(klee::ExecutionState *kstate,
                                     klee::ref<klee::SymbolicPtrResolution> res);

  // Ignored
  virtual bool onStateBranching(klee::ExecutionState *kstate, klee::ForkTag forkTag)
  { return false; }

  virtual void onStateBranched(klee::ExecutionState *kstate, klee::ExecutionState *parent,
                               int index, klee::ForkTag forkTag) {}
	virtual void onUpdateSearcherAfterBranch(const std::vector<klee::ExecutionState*> &states) {}

  virtual void onStateDestroy(klee::ExecutionState *kstate, bool silenced) {}
  virtual void onDebugInfo(klee::ExecutionState *kstate, const std::string &message) {}
  virtual void onOutOfResources(klee::ExecutionState *destroyedState) {}
  virtual void onEvent(klee::ExecutionState *kstate, klee::EventClass type, uint64_t val) {}
  virtual void onIcsBegin(klee::ExecutionState *kstate) {}
  virtual void onIcsMaybeEndRegion(klee::ExecutionState *kstate, klee::ThreadUid tuid, bool force) {}
};

//-----------------------------------------------------------------------------
// We name single-threaded regions by (thread id, callstack)
// We name parallel regions by a list of single-threaded regions, sorted by thread id
//-----------------------------------------------------------------------------

struct ParallelRegionId {
  ParallelRegionId() {}
  explicit ParallelRegionId(klee::ExecutionState *kstate);

  // Contexts compare equal if their "strippedctx"s are equal,
  // where "strippedctx = fullctx->removePthreadsFunctions()"
  struct Context {
    const klee::CallingContext *fullctx;
    const klee::CallingContext *strippedctx;
    klee::ThreadUid canonicalTuid;
  };

  // The sorted list of contexts
  typedef llvm::SmallVector<Context,8> ContextListTy;
  ContextListTy contexts;
};

bool operator<(const ParallelRegionId &lhs, const ParallelRegionId &rhs);

//-----------------------------------------------------------------------------
// A priority queue of branches
// Used by our DFS strategy to prioritize paths
//-----------------------------------------------------------------------------

class DepthFirstICSQueue {
public:
  typedef std::deque<cloud9::worker::SymbolicState*> QueueTy;
  typedef std::set<cloud9::worker::SymbolicState*> SetTy;

  QueueTy _queue;
  SetTy   _set;

public:
  DepthFirstICSQueue() {}

  void clear();
  size_t size() const { return _queue.size(); }
  bool empty() const { return _queue.empty(); }

  cloud9::worker::SymbolicState* head() const;
  void addState(cloud9::worker::SymbolicState* state);
  void updateState(cloud9::worker::SymbolicState* state);
  void removeState(cloud9::worker::SymbolicState* state);
  bool isStateQueued(cloud9::worker::SymbolicState* state) const;
};

//-----------------------------------------------------------------------------
// A DFS strategy that uses precondition slicing to bias the search space
//-----------------------------------------------------------------------------

class DepthFirstICS : public cloud9::worker::JobSelectionStrategy {
public:
  struct RegionPathEdge {
    size_t traceId;   // correlates with the trace-id of output files
    bool   exits;     // does this path end with program-exit?
    bool   deadlocks; // does this path end with deadlock?
    ParallelRegionId nextRegion;  // region after this one (if !exits && !deadlocks)
  };

  struct SimpleStat {
    size_t samples;
    size_t accum;
    std::map<size_t, size_t> buckets;

    SimpleStat() : samples(0), accum(0) {}
    void addSample(size_t n);
    void dump(std::ostream &os, const char *name) const;
  };

  struct ParallelRegionInfo {
    size_t numericId;
    std::vector<RegionPathEdge> regionEdges;
    PathManager paths;

    // Statistics
    SimpleStat perThreadTime;
    SimpleStat perThreadImbalance;
    SimpleStat perThreadImbalanceNoSleepers;

    ParallelRegionInfo() : refCount(0) {}
    unsigned refCount;
  };

  typedef klee::ref<ParallelRegionInfo> ParallelRegionInfoRef;

  typedef std::set<klee::ref<klee::Expr> >    ConstraintSetTy;
  typedef std::vector<klee::ref<klee::Expr> > ConstraintVectorTy;
  typedef std::map<ParallelRegionId, cloud9::worker::SymbolicState*> RegionsFrontierTy;
  typedef std::map<ParallelRegionId, ParallelRegionInfoRef> RegionsInfoTy;
  typedef std::set<const llvm::Loop*> LoopSetTy;

private:
  PrecondSliceTraceLogger _logger;
  cloud9::worker::JobManager *_jobManager;

  //
  // Info about the current region
  //
  // We execute one region at a time.  The DFS strategy is to execute one
  // path, compute a precond slice to determine the path constraint, then
  // try all possible "flips" of that path constraint.  All paths in a
  // region begin as clones of the same root state.
  //
  ParallelRegionId  _currRegion;       // the current region's id
  PathManager*      _currPathManager;  // the current region's set of inputs
  cloud9::worker::SymbolicState *_currRoot;  // root state for the current region

  // Used to guide DFS for the current path
  DepthFirstICSQueue _currQueue;

  // Info about other regions
  RegionsFrontierTy _regionsFrontier;  // regions to-be-explored (maps Id -> RootState)
  RegionsInfoTy     _regions;          // summary of all regions, including already-explored
  size_t            _completedRegions; // total number of previously-completed regions

  // Staging area for the above fields
  // We flush this staging area each time a region completes
  RegionsFrontierTy _localRegionsFrontier;
  RegionsInfoTy     _localRegions;
  size_t            _nextRegionNumericId;

  // Pending loop upgrades
  LoopSetTy _loopsToUpgrade;

  // Info about symbolic objects
  // These are shared with all klee states
  std::auto_ptr<klee::SymbolicPtrResolutionTable> _resolvedSymbolicPtrs;
  std::auto_ptr<klee::SymbolicObjectTable> _symbolicObjects;

private:
  PathManager* getCurrPathManager();

  void addRegion(const ParallelRegionId &id);
  void addRegionEdge(const ParallelRegionId &nextId, const bool exits, const bool deadlocks);
  void clearRegionStagingArea();
  void flushRegionStagingArea();

  void forkNextPath();
  void processPath(cloud9::worker::SymbolicState *state, const bool exited);
  void upgradeUncoveredLoops();

  void dumpSymbolicPtrGraph(klee::ExecutionState *kstate,
                            const PrecondSliceTrace &slice,
                            const char *fileprefixDot,
                            const char *fileprefixTxt);
  void dumpSymbolicTree(cloud9::worker::WorkerTree::Node *root,
                        cloud9::worker::WorkerTree::Node *highlight,
                        const char *fileprefix);
  void dumpConstraintsFromPrecondSlice(const PrecondSliceTrace &slice,
                                       const char *fileprefix,
                                       bool filter = false);

  void dumpFinalRegionSummary(bool isFinal);
  void dumpFinalSymbolicObjects();

public:
  bool isQueuedState(cloud9::worker::SymbolicState *state) const;
  bool isRootState(cloud9::worker::SymbolicState *state) const;
  bool isRegionFrontierState(cloud9::worker::SymbolicState *state) const;
  size_t regionFrontierSize() const;

public:
  DepthFirstICS(cloud9::worker::JobManager *jm);
  virtual ~DepthFirstICS() {}
  virtual void finalize();

  // Called internally
  void shouldUpgradeUncoveredLoops(const LoopSetTy &loops);

  // Important
  virtual void onUpdateAfterBranch(const std::vector<cloud9::worker::SymbolicState*> states);
  virtual cloud9::worker::ExecutionJob* onNextJobSelection();
  virtual void onStateActivated(cloud9::worker::SymbolicState *state);
  virtual void onStateDeactivated(cloud9::worker::SymbolicState *state);
  virtual void onIcsBegin(cloud9::worker::SymbolicState *state);
  virtual void onIcsMaybeEndRegion(cloud9::worker::SymbolicState *state,
                                   klee::ThreadUid tuid,
                                   bool force);

  // Ignored
  virtual void onJobAdded(cloud9::worker::ExecutionJob *job) {}
  virtual void onRemovingJob(cloud9::worker::ExecutionJob *job) {}
  virtual void onStateUpdated(cloud9::worker::SymbolicState *state,
                              cloud9::worker::WorkerTree::Node *oldNode) {}
  virtual void onStateStepped(cloud9::worker::SymbolicState *state) {}
};

}  // namespace ics

#endif
