/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * Representation of path constraints and sets of path constraints.
 *
 */

#ifndef ICS_PATHCONSTRAINTS_H
#define ICS_PATHCONSTRAINTS_H

#include "ics/PrecondSliceTrace.h"
#include "ics/HBGraph.h"
#include "klee/Expr.h"
#include "klee/util/ExprHashMap.h"
#include "cloud9/worker/CoreStrategies.h"

#include <map>
#include <queue>
#include <stack>
#include <vector>
#include <tr1/unordered_map>

namespace ics {

//-----------------------------------------------------------------------------
// A schedule, with its path constraint
//-----------------------------------------------------------------------------

struct PathConstraint {
public:
  PathConstraint()
    : branch(NULL), fullSummary(klee::Expr::True), conciseSummary(klee::Expr::True),
      canonical(klee::Expr::True)
  {}

  // For debugging (valid only in "frontier" branches)
  klee::KInstruction*   branch;    // the branch that was flipped for this input (might be NULL)
  klee::ref<klee::Expr> lastCond;  // the branch condition emitted by "branch"
  klee::ThreadUid       lastTuid;  // the thread that executed 'branch'

  // The set of branch constraints
  klee::ref<klee::Expr> fullSummary;    // conjunction of constraints at all branches
  klee::ref<klee::Expr> conciseSummary; // simplified version of "fullSummary"
  klee::ref<klee::Expr> canonical;      // for checking subsumption (used internally)

  // The complete list of constraints in the order they were encountered.
  // This includes everything in "fullSummary" as well as extra invariants
  // needed to the ensure soudness of symbolic execution.
  struct Entry {
    klee::ForkClass fc;          // constraint type
    klee::ref<klee::Expr> expr;  // constraint
    hbnode_id_t currHbNodeId;    // HBGraph.MaxNodeId when this constraint was generated,
                                 // or 0 if unknown
    Entry(klee::ForkClass a, klee::ref<klee::Expr> b, hbnode_id_t c)
      : fc(a), expr(b), currHbNodeId(c) {}
  };
  typedef std::vector<Entry> ListTy;
  ListTy list;

  // The complete list of context switches
  typedef std::queue<klee::ThreadUid> ThreadListTy;
  ThreadListTy ctxswitches;
};

struct QueuedPathConstraint {
public:
  QueuedPathConstraint(const PathConstraint &_pc)  // not explit: allow implicit conversions
    : pc(_pc), hbGraph(NULL), hbGraphMaxIdToReplay(0)
  {}

  PathConstraint pc;
  HBGraphRef hbGraph;
  hbnode_id_t hbGraphMaxIdToReplay;
};

struct CoveredSchedule {
private:
  friend class klee::ref<CoveredSchedule>;
  unsigned refCount;
  static size_t NextId;

public:
  PathConstraint inputConstraint;
  HBGraphRef hbGraph;
  const bool exitsProgram;
  const size_t id;

  CoveredSchedule(PathConstraint pc, HBGraphRef g, bool exits)
    : refCount(0), inputConstraint(pc), hbGraph(g), exitsProgram(exits), id(++NextId)
  {}
};

//-----------------------------------------------------------------------------
// A set of path constraints
//-----------------------------------------------------------------------------

class PathManager {
public:
  typedef std::deque<QueuedPathConstraint> PathConstraintQueueTy;
  typedef std::deque<klee::ref<CoveredSchedule> > ScheduleListTy;
  struct MirroredKleeStats;
  struct PathConstraintBuilder;

private:
  // Used for exploring paths
  PathConstraintQueueTy _frontier;     // inputs to-be-explored
  klee::ExprHashSet     _frontierSet;  // the set of inputs added to '_frontier'
  ScheduleListTy        _explored;     // inputs already-explored (actual paths)
  klee::ExprHashSet     _exploredSet;  // inputs already-explored (summarized constraints)

  // The last PathConstraint that was instantiated
  PathConstraint _priorPC;

  // Statistics
  // Count of pc.branch for all pcs added to '_frontier'
  struct BranchStats {
    klee::ExprHashSet condsSeen;
    size_t timesSeen;
  };
  std::map<klee::KInstruction*, BranchStats> _branchStats;  // branch -> stats
  std::map<size_t, size_t>                   _pathLengths;  // lengths -> counts

  // Stats aggregated from lib/Core/CoreStats.h and lib/Solver/SolverStats.h
  // for paths instantiated by this PathManager
  klee::ref<MirroredKleeStats> _currStats;

private:
  bool alreadySeen(const PathConstraintBuilder &pcBuilder) const;
  void addExplored(PathConstraintBuilder *pcBuilder, klee::ref<klee::Expr> cond, hbnode_id_t currHbNodeId);

  // DEBUG
  void dumpFrontierSummary(klee::ExecutionState *kstate,
                           cloud9::worker::JobManager *jm) const;

public:
  PathManager();
  ~PathManager();

  void clearFrontier() { _frontier.clear(); }
  void clearCaches() { _frontierSet.clear(); _exploredSet.clear(); }
  void resetToEmpty();

  bool isFrontierEmpty() const { return _frontier.empty(); }
  bool isExploredEmpty() const { return _explored.empty(); }
  size_t numFrontier() const { return _frontier.size(); }
  size_t numExplored() const { return _explored.size(); }

  // Returns the full list of explored schedules
  const ScheduleListTy& getSchedules() const { return _explored; }

  // Add a new schedule
  // N.B.: This is used rarely: you probably want processSlice()
  void addSchedule(klee::ref<CoveredSchedule> sched);

  // Compute a path constraint for the given slice.
  // Add all "flipped" path constraints to the frontier.
  void processSlice(klee::ExecutionState *kstate,
                    const PrecondSliceTrace &slice,
                    cloud9::worker::JobManager *jm,
                    const bool programExited);

  // Instantiate the next frontier input into 'kstate'.
  // REQUIRES: !isFrontierEmpty()
  void instantiateNextInput(klee::ExecutionState *kstate,
                            cloud9::worker::JobManager *jm);

  // Reset stats for a new path
  // This is done automatically via instantiateNextInput(),
  // but in some cases it needs to be done separately.
  void resetStatsForNewPath();

  // Dump all stats
  void dumpGlobalStats(cloud9::worker::JobManager *jm) const;
  void dumpStats(cloud9::worker::JobManager *jm) const;
};

}  // namespace ics

#endif
