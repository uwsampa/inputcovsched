/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * Representation of path constraints and sets of path constraints.
 *
 */

#include "ics/PathManager.h"
#include "ics/SearchStrategy.h"

#include "../Core/Common.h"
#include "../Core/CoreStats.h"
#include "../Core/Memory.h"
#include "../Core/TimingSolver.h"

#include "klee/Context.h"
#include "klee/Executor.h"
#include "klee/ExecutionState.h"
#include "klee/Solver.h"
#include "klee/SymbolicObjectTable.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Support/Timer.h"
#include "klee/util/ExprVisitor.h"
#include "cloud9/worker/JobManager.h"

#include "llvm/Argument.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/PassManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/DebugInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <sstream>

// N.B.: About implied value concretization
// Implied value concretization can screw things up.  Consider the following
// example:
//
//    if (x == 0) ... else ...
//    if (x == 2) lock()
//
// If we take the true-branch for (x == 0), it is tempting to concretize x to
// 0.  However, if we do so, we lose the fact that (x == 2) is input-dependent,
// so we miss the (x == 2) branch and fail to cover an important set of inputs.
// It is okay to concretize x as long as the original symbolic value for x is
// kept in a shadow structure which is made available to the slicing algorithm.
// Currently we avoid all implied value concretization optimizatinos, because
// that is simpler.

// N.B.: About addresses
// We assume that concrete addresses do not appear in path constraints because
// they may change from run-to-run.  The exceptions are:
//
// (1) Global variable and function addresses, since these are constants.
//
// (2) Constraints of type KLEE_FORK_RESOLVE_PTR_LAZY constraints, which are
//     ignored in the path constraint (we will regenerate them during input
//     instantiation).
//
// However, the user can still screw us with code like the following:
//
//     intptr_t *x;
//     klee_ics_region_marker();
//     foo = malloc();
//     if (*x < foo)
//        ...
//
// In this case, the concrete heap address of "foo" is included in the path
// constraint as part of the comparison with x.  Perhaps we should observe
// that the result of malloc() is a nondeterministic input and we should update
// the schedule after loading that input, but for now we have decided to assume
// that the above does not happen.
// TODO: We do need to handle nondeterministic input from read() and friends
// This will be implemented eventually...

using klee::ref;
using klee::RefCounted;
using klee::Expr;
using klee::ExprNaryIterator;
using klee::AndExpr;
using klee::OrExpr;
using klee::ImmutableExprSet;
using klee::Array;
using klee::MemoryObject;
using klee::SymbolicObject;
using klee::SymbolicPtrResolution;
using klee::ThreadUid;

namespace {
using namespace llvm;

// Options to write more output
cl::opt<bool> DumpPathConstraints("ics-dump-path-constraints", cl::init(false));

// this option is meant for testing code only
cl::opt<bool> SortPathConstraintTerms("ics-sort-path-constraint-terms", cl::init(false));

// debugging
cl::opt<bool> DbgInputConstraints("ics-debug-input-constraints", cl::init(false));
cl::opt<bool> DbgInputConstraintsVerbose("ics-debug-input-constraints-verbose", cl::init(false));
cl::opt<bool> DbgPathConstraints("ics-debug-path-constraints", cl::init(false));
cl::opt<bool> DbgPathConstraintsVerbose("ics-debug-path-constraints-verbose", cl::init(false));
cl::opt<bool> DbgProgress("ics-debug-progress", cl::init(false));
}

namespace ics {

size_t CoveredSchedule::NextId;

//-----------------------------------------------------------------------------
// PathManager::MirroredKleeStats
// This is pretty ugly, but easier that re-architecting klee to do what I want
//-----------------------------------------------------------------------------

struct PathManager::MirroredKleeStats {
private:
  struct MyStat {
    std::string name;
    uint64_t value;
    unsigned refCount;
    MyStat *linked;

    explicit MyStat(const std::string &n)
      : name(n), value(0), refCount(0), linked(NULL)
    {}

    explicit MyStat(MyStat *accum)
      : name(accum->name), value(0), refCount(0), linked(accum)
    {}

    void operator++(int) {
      *this += 1;
    }
    MyStat &operator+=(const uint64_t addend) {
      value += addend;
      if (linked)
        linked->value += addend;
      return *this;
    }
    MyStat &operator=(const uint64_t newvalue) {
      value = newvalue;
      return *this;
    }
  };

  typedef ref<MyStat> MyStatPtr;
  unsigned refCount;

public:
  // Stats across all PathMangers
  // This is special in the sense that we only ever update "_stats"
  // and the only method we call is "dump()".
  static MirroredKleeStats* GlobalStats;

private:
  friend class PathManager;
  friend class ref<MirroredKleeStats>;

  // Local vars are in _stats[i]
  // Forall i < PairedStats.size(), _stat[i] is updated from PairedStats[i].
  static std::vector<klee::Statistic*> PairedStats;
  std::vector<MyStatPtr> _stats;

  // Other vars are updated manually
  std::auto_ptr<klee::WallTimer> _myTimer;
  MyStatPtr _myExecTime;
  MyStatPtr _myNumPaths;
  MyStatPtr _myPathTotalLength;
  MyStatPtr _myPathTotalConstraintSize;
  MyStatPtr _myInputHarnessConstraintSize;
  std::map<std::pair<klee::ForkClass, std::string>, MyStatPtr> _myForksByType;
  std::map<std::string, MyStatPtr> _myErrsByType;

private:
  void MirrorInLocalStats(klee::Statistic *s) {
    PairedStats.push_back(s);
    _stats.push_back(new MyStat(s->getName()));
  }
  void AddToLocalStats(MyStatPtr *s, const std::string &name) {
    *s = new MyStat(name);
    _stats.push_back(*s);
  }
  void AddToLocalStats(MyStatPtr linked) {
    _stats.push_back(new MyStat(linked.get()));
  }
  void AddToLocalStats(MyStatPtr MirroredKleeStats::*sfield) {
    (this->*sfield) = new MyStat((GlobalStats->*sfield).get());
    _stats.push_back(this->*sfield);
  }
  template<class KeyTy>
  void AddToLocalStatsMap(const KeyTy &key,
                          const std::string &statname,
                          std::map<KeyTy, MyStatPtr> MirroredKleeStats::*sfield) {
    MyStatPtr *gs = &(GlobalStats->*sfield)[key];
    MyStatPtr *ls = &(this->*sfield)[key];
    if (!gs->get()) {
      *gs = new MyStat(statname);
    }
    if (!ls->get()) {
      *ls = new MyStat(gs->get());
      _stats.push_back(*ls);
    }
  }

  explicit MirroredKleeStats(const bool isGlobal) : refCount(0) {
    if (isGlobal) {
      // Copy some klee stats
      // TODO: more stats for branches due to symbolic fn ptrs, wlist notifies, etc.
      MirrorInLocalStats(&::klee::stats::instructions);
      MirrorInLocalStats(&::klee::stats::solverTime);
      MirrorInLocalStats(&::klee::stats::resolveTime);
      MirrorInLocalStats(&::klee::stats::solverTimeOuts);
      // Add some local stats
      AddToLocalStats(&_myExecTime, "PathsExecTimeTotalMicroSec");
      AddToLocalStats(&_myNumPaths, "PathsTotal");
      AddToLocalStats(&_myPathTotalLength, "PathTotalLength");
      AddToLocalStats(&_myPathTotalConstraintSize, "PathConstraintsTotalSize");
      AddToLocalStats(&_myInputHarnessConstraintSize, "InputHarnessConstraintSize");
    } else {
      assert(GlobalStats);
      // Paired stats
      for (size_t k = 0; k < PairedStats.size(); ++k) {
        AddToLocalStats(GlobalStats->_stats[k]);
      }
      // Unpaired stats
      AddToLocalStats(&MirroredKleeStats::_myNumPaths);
      AddToLocalStats(&MirroredKleeStats::_myExecTime);
      AddToLocalStats(&MirroredKleeStats::_myPathTotalLength);
      AddToLocalStats(&MirroredKleeStats::_myPathTotalConstraintSize);
      AddToLocalStats(&MirroredKleeStats::_myInputHarnessConstraintSize);
    }
  }

public:
  static MirroredKleeStats* New() {
    // Create GlobalStats first, if it does not exist
    if (!GlobalStats) {
      GlobalStats = new MirroredKleeStats(true);
    }
    return new MirroredKleeStats(false);
  }

  void startPath() {
    // Reset klee stats for the next path
    for (size_t k = 0; k < PairedStats.size(); ++k) {
      PairedStats[k]->resetValue();
    }
    _myTimer.reset(new klee::WallTimer);
  }

  void endPath(klee::ExecutionState *kstate) {
    // Extract stats from the klee stats counters
    for (size_t k = 0; k < PairedStats.size(); ++k) {
      (*_stats[k]) += PairedStats[k]->getValue();
    }
    // Update my stats
    (*_myNumPaths)++;
    (*_myExecTime) += _myTimer->check();
    // This should really be set just once (the value should be the same for all paths)
    // Hence, we use "=", not "+="
    (*_myInputHarnessConstraintSize) = kstate->inputHarnessConstraints.getSummaryExpr()->getSize();
    // Update error counts
    {
      std::string key = kstate->finalErrorCode;
      std::string statname = "NumWithErrorCode.[" + kstate->finalErrorCode + "]";
      AddToLocalStatsMap(key, statname, &MirroredKleeStats::_myErrsByType);
      (*_myErrsByType[key])++;
    }
  }

  void addForkingBranch(const klee::ForkClass fc, size_t numFlips, const std::string &fname) {
    for (int i = 0; i < 2; ++i) {
      std::pair<klee::ForkClass, std::string> key(fc, fname);
      std::string statname;
      if (i == 0) {
        key.second.clear();
        statname = "NumForksTotal." + KleeForkClassToString(key.first);
      } else {
        statname = "NumForksTotal." + KleeForkClassToString(key.first) + "." + fname;
      }
      AddToLocalStatsMap(key, statname, &MirroredKleeStats::_myForksByType);
      (*_myForksByType[key]) += numFlips;
    }
  }

  void addFinishedPath(const PathConstraint &pc, const size_t length) {
    (*_myPathTotalLength) += length;
    (*_myPathTotalConstraintSize) += pc.conciseSummary->getSize();
  }

  void dumpCsv(std::ostream &os, bool headers, int regionNum) {
    if (headers) {
      os << "RegionNum";
      for (size_t k = 0; k < _stats.size(); ++k) {
        os << "," << _stats[k]->name;
      }
      os << "\n";
    }
    os << regionNum;
    for (size_t k = 0; k < _stats.size(); ++k) {
      os << "," << _stats[k]->value;
    }
    os << "\n";
  }

  void dumpTxt(std::ostream &os) {
    for (size_t k = 0; k < _stats.size(); ++k) {
      os << _stats[k]->name << " = " << _stats[k]->value << "\n";
    }
  }

  size_t numTotalPaths() const {
    return _myNumPaths->value;
  }
};

PathManager::MirroredKleeStats* PathManager::MirroredKleeStats::GlobalStats = NULL;
std::vector<klee::Statistic*> PathManager::MirroredKleeStats::PairedStats;

//-----------------------------------------------------------------------------
// PathConstraintBuilder
//-----------------------------------------------------------------------------

// TODO: for canonicalizing pcs: maybe we need to sort by exprs? e.g., what can
// happen is either this case, in which two branches are executed in different
// orders:
//    T1, first exec:       T2, second exec:
//      if(a) {} else {}      if(b) {} else {}
//      if(b) {} else {}      if(a) {} else {}
//
// or this case (perhaps more likely), in which two threads are executed in
// different orders:
//    first exec:             second exec:
//      T1: if(a) {} else {}    T2: if(b) {} else {}
//      T2: if(b) {} else {}    T1: if(a) {} else {}
//
// If it comes down to just two constraints, we could always send the solver the
// query C1=>C2 & C2=>C1

struct PathManager::PathConstraintBuilder {
  PathConstraint pc;
  ImmutableExprSet fullSummarySet;
  ref<Expr> fullCanonical;     // ordered by Expr::compare
  ref<Expr> conciseCanonical;  // ordered by Expr::compare
  size_t length;

  PathConstraintBuilder() {
    fullCanonical = Expr::True;
    conciseCanonical = Expr::True;
    length = 0;
  }

  static ref<Expr> serializeExprSet(ImmutableExprSet set) {
    ref<Expr> e = Expr::True;
    for (ImmutableExprSet::iterator it = set.begin(), itEnd = set.end(); it != itEnd; ++it)
      e = AndExpr::create(e, *it);
    return e;
  }

  static ref<Expr> serializeExprSetDisjunct(ImmutableExprSet set) {
    ref<Expr> e = Expr::False;
    for (ImmutableExprSet::iterator it = set.begin(), itEnd = set.end(); it != itEnd; ++it)
      e = OrExpr::create(e, *it);
    return e;
  }

  void addToSummary(ref<Expr> expr) {
    pc.fullSummary = AndExpr::create(pc.fullSummary, expr);
    klee::ConstraintManager cm;
    cm.addConstraint(pc.fullSummary);
    pc.conciseSummary = cm.getSummaryExpr();
  }

  void addToCanonical(ref<Expr> expr) {
    if (!fullSummarySet.count(expr)) {
      fullSummarySet = fullSummarySet.insert(expr);
      // Rebuild "full"
      fullCanonical = serializeExprSet(fullSummarySet);
      // Rebuild "concise"
      klee::ConstraintManager cm;
      cm.addConstraint(fullCanonical);
      ImmutableExprSet set;
      for (klee::ConstraintManager::iterator it = cm.begin(),
                                          itEnd = cm.end(); it != itEnd; ++it) {
        set = set.insert(*it);
      }
      conciseCanonical = serializeExprSet(set);
      pc.canonical = conciseCanonical;
    }
  }

  void add(ref<Expr> expr, hbnode_id_t currHbNodeId) {
    pc.lastCond = expr;
    addToSummary(expr);
    addToCanonical(expr);
    pc.list.push_back(PathConstraint::Entry(klee::KLEE_FORK_INVARIANT_INPUT, expr, currHbNodeId));
    ++length;
  }

  void addExtraInvariant(const klee::ForkClass fc, ref<Expr> expr) {
    assert(fc != klee::KLEE_FORK_INVARIANT_INPUT);
    pc.list.push_back(PathConstraint::Entry(fc, expr, 0));
  }

  void trimExtraInvariants() {
    // Remove all non-branch-related invariants from the back of the pclist
    while (!pc.list.empty()) {
      const klee::ForkClass fc = pc.list.back().fc;
      if (fc == klee::KLEE_FORK_INVARIANT_INPUT || klee::IsBranchingForkClass(fc))
        break;
      pc.list.pop_back();
    }
  }
};

//-----------------------------------------------------------------------------
// PathManager
//-----------------------------------------------------------------------------

PathManager::PathManager()
  : _currStats(MirroredKleeStats::New())
{}

PathManager::~PathManager()
{}

void PathManager::resetToEmpty() {
  *this = PathManager();
  resetStatsForNewPath();
}

bool PathManager::alreadySeen(const PathConstraintBuilder &pcBuilder) const {
  return _exploredSet.count(pcBuilder.fullCanonical)
      || _exploredSet.count(pcBuilder.conciseCanonical)
      || _frontierSet.count(pcBuilder.fullCanonical)
      || _frontierSet.count(pcBuilder.conciseCanonical);
}

void PathManager::addSchedule(klee::ref<CoveredSchedule> sched) {
  assert(sched.get());
  _explored.push_back(sched);
  _exploredSet.insert(sched->inputConstraint.conciseSummary);
}

void PathManager::addExplored(PathConstraintBuilder *pcBuilder, ref<Expr> cond, hbnode_id_t currHbNodeId) {
  PathConstraintBuilder prior;
  if (isa<OrExpr>(cond))
    prior = *pcBuilder;

  // Append the condition and add to "explored"
  pcBuilder->add(cond, currHbNodeId);
  _exploredSet.insert(pcBuilder->fullCanonical);
  _exploredSet.insert(pcBuilder->conciseCanonical);

  // If "cond" is a disjunction, add each term of the disjunction separately.
  // This is motivated by switch statements, as in the following:
  //   switch(x) {
  //   case 1: sync
  //   case 2: break
  //   case 3: break
  //   }
  // Suppose we explore case 1 first: we will add (x==2) and (x==3) as flipped
  // conditions.  Then suppose we explore case 2: precond slicing will generalize
  // the condition to (x==2||x==3), and here we inject (x==3) to ensure that we
  // do no explore that path.
  if (isa<OrExpr>(cond)) {
    for (ExprNaryIterator it(cond, Expr::Or); it != ExprNaryIterator(); ++it) {
      PathConstraintBuilder tmp = prior;
      tmp.addToCanonical(*it);
      _exploredSet.insert(tmp.fullCanonical);
      _exploredSet.insert(tmp.conciseCanonical);
    }
  }
}

void PathManager::processSlice(klee::ExecutionState *kstate,
                               const PrecondSliceTrace &slice,
                               cloud9::worker::JobManager *jm,
                               const bool programExited) {
  klee::TimingSolver *solver = kstate->executor->getTimingSolver();;
  solver->setTimeout(klee::clvars::MaxSTPTime);

  PathConstraintBuilder pcBuilder;
  size_t important = 0;
  size_t frontier = 0;

  // This ends a path
  _currStats->endPath(kstate);

  // Save the HBGraph so we can replay it
  const HBGraphRef oldHbGraph = kstate->hbGraph.makeImmutableClone();
  hbnode_id_t oldHbGraphLastId = 0;   // updated on each branch

  // Reset the assumptions as they were at the beginning of the region.
  // We use these to make MustBeTrue queries more precise.
  klee::PushConstraints savedConstraints(kstate->globalConstraints);
  kstate->globalConstraints = kstate->inputHarnessConstraints;

  //
  // Compute:
  //  (a) a summary path constraint for the path that was executed, and
  //  (b) a set of "flipped" path constraints
  //
  // We try to avoid generating redundant "flipped" constraints, so before
  // flipping a constraint, we check if the "flipped" constraint is feasible
  // given all constraints up to that point.
  //
  // We expect the slice will include, first, a group of input constraints that
  // were added by PathManager::instantiateNextInput(), and then, a
  // group of constraints that represent the path actually taken.
  //
  for (PrecondSliceTrace::const_iterator e = slice.begin(); e != slice.end(); ++e) {
    // Don't bother continuing if there are no symbolic branches remaining
    bool anyBranchesLeft = false;
    for (PrecondSliceTrace::const_iterator next = e; next != slice.end(); ++next) {
      if ((next->hasCondition() && klee::IsBranchingOrInputForkClass(next->forkclass())) ||
          (next->hasOtherCtxSwitches())) {
        anyBranchesLeft = true;
        break;
      }
    }
    if (!anyBranchesLeft) {
      if (DbgPathConstraints)
        llvm::errs() << "PathConstraint.SkippingEarly\n";
      break;
    }

    // Apply ctx switches
    if (e->hasOtherCtxSwitches()) {
      for (ThreadSetTy::iterator
           t = e->otherCtxSwitches().begin(); t != e->otherCtxSwitches().end(); ++t) {
        PathConstraintBuilder sw = pcBuilder;
        sw.pc.branch = e->kinst();
        sw.pc.lastTuid = e->tuid();
        sw.pc.ctxswitches.push(*t);
        _frontier.push_back(sw.pc);
        _frontier.back().hbGraph = oldHbGraph;
        _frontier.back().hbGraphMaxIdToReplay = oldHbGraphLastId;  // FIXME: this is wrong
      }
      pcBuilder.pc.ctxswitches.push(e->targetTuid());
      if (!e->otherCtxSwitches().empty()) {
        _currStats->addForkingBranch(klee::KLEE_FORK_SCHEDULE_MULTI, e->otherCtxSwitches().size(),
                                     e->kinst()->inst->getParent()->getParent()->getNameStr());
      }
    }

    // Skip all other entries that do not have symbolic constraints
    if (!e->hasCondition()) {
      if (DbgPathConstraints && e->kind() == PrecondSliceTrace::ResolvedSymbolicPtr) {
        llvm::errs() << "PathConstraint.PtrResolution\n";
        SymbolicPtrResolution::Dump("  ", e->ptrResolution());
      }
      continue;
    }

    assert(e->kind() == PrecondSliceTrace::Assertion ||
           e->kind() == PrecondSliceTrace::Branch ||
           e->kind() == PrecondSliceTrace::Call);
    assert(e->condition().get());

    const ref<Expr>& cond = e->condition();

    // Input constraints get ignored: they should be duplicated by
    // actual branch constraints that should appear later in the slice.
    if (e->forkclass() == klee::KLEE_FORK_INVARIANT_INPUT) {
      assert(e->kind() == PrecondSliceTrace::Assertion);
      if (DbgPathConstraints)
        std::cerr << "PathConstraint.Input:\n" << cond << "\n";
      continue;
    }

    // For DEBUG: these conditions won't be printed
    const bool debugNoPrintCond =
       (e->forkclass() == klee::KLEE_FORK_CONCRETIZE_FP)
        || (!DbgPathConstraintsVerbose &&
            e->forkclass() == klee::KLEE_FORK_POINTER_ALIASING);

    // Skip some assumptions
    // N.B.: We cannot skip all assumptions because that could break things
    // later.  Two specific examples of things we cannot skip:
    //   (a) Divide-by-zero invariants: if we have a constraint (x/y) and we
    //       elide the y!=0 constraint, the solver might return "satisfiable"
    //       even for queries that are satisfiable only when y==0.
    //   (b) Bounds-check invariants: if we have a constraint x[i]=y and we
    //       ask the solver for a satisfying assignment for i, we don't want
    //       the solver to return an i that is out-of-bounds.
    // These are some assumptions we can definitely skip:
    //   (a) KLEE_FORK_POINTER_ALIASING: these contain the raw addresses
    //       of symbolic ptrs, which we don't care about
    //   (b) Concretization constraints when the "no concretes" optimization is enabled
    if (e->forkclass() == klee::KLEE_FORK_POINTER_ALIASING ||
        (OptNoConcretizationConstraintsInSlice
         && klee::IsConcretizationForkClass(e->forkclass()))) {
      // Include for MustBeTrue tests
      kstate->addConstraint(cond);
      // DEBUG
      assert(e->kind() == PrecondSliceTrace::Assertion);
      if (DbgPathConstraints) {
        if (debugNoPrintCond) {
          std::cerr << "PathConstraint.Ignored." << KleeForkClassToString(e->forkclass()) << "\n";
        } else {
          std::cerr << "PathConstraint.Ignored." << KleeForkClassToString(e->forkclass())
                    << ":\n" << cond << "\n";
        }
      }
      continue;
    }

    // The non-branching constraints do not get flipped, but they do
    // get added to the pc in the special "extra invariants" field.
    if (klee::IsNonBranchingForkClass(e->forkclass())) {
      // Skip if redundant by the cheap redundancy test
      if (kstate->constraints().simplifyExpr(cond)->isTrue()) {
        if (DbgPathConstraints) {
          std::cerr << "PathConstraint.Redundant." << KleeForkClassToString(e->forkclass())
                    << ":\n" << cond << "\n";
        }
        continue;
      }

      pcBuilder.addExtraInvariant(e->forkclass(), cond);
      kstate->addConstraint(cond);

      // DEBUG
      assert(e->kind() == PrecondSliceTrace::Assertion);
      assert(!e->useFlippedConditions());
      if (DbgPathConstraints) {
        if (debugNoPrintCond) {
          std::cerr << "PathConstraint.Assert." << KleeForkClassToString(e->forkclass()) << "\n";
        } else {
          std::cerr << "PathConstraint.Assert." << KleeForkClassToString(e->forkclass())
                    << ":\n" << cond << "\n";
        }
      }

      continue;
    }

    // "MustBeTrue" implies a redundant constraint
    bool truth;
    if (solver->mustBeTrue(*kstate, cond, truth) && truth) {
      if (DbgPathConstraints) {
        std::cerr << "PathConstraint.RedundantBySolver." << KleeForkClassToString(e->forkclass())
                  << ":\n" << cond << "\n";
      }
      continue;
    }

    ++important;
    if (DbgPathConstraintsVerbose) {
      std::cerr << "PathConstraint.Branch:\n" << cond << "\n";
    }

    // All paths that match the current "pcBuilder.pc" must replay the
    // old HBGraph up to (at least) this point, though "flipped" paths
    // might take the HbGrah in a new direction after this branch.
    oldHbGraphLastId = e->maxHbNodeId();

    // Process all flipped constraints: these represent other possible
    // paths that could be taken from this branch.  Occasionally these
    // are specified explicitly in the trace, but otherwise (usually)
    // we just flip the branch condition.
    FlippedConditionsMapTy flips;
    if (e->useFlippedConditions()) {
      flips = e->flippedConditions();
    } else {
      flips = flips.insertNullData(Expr::createIsZero(cond));
    }

    for (FlippedConditionsMapTy::iterator it = flips.begin(); it != flips.end(); ++it) {
      PathConstraintBuilder flipped;
      flipped = pcBuilder;
      flipped.pc.branch = e->kinst();
      flipped.pc.lastTuid = e->tuid();
      flipped.add(it->first, oldHbGraphLastId);

      if (!alreadySeen(flipped)) {
        _frontier.push_back(flipped.pc);
        _frontier.back().hbGraph = oldHbGraph;
        _frontier.back().hbGraphMaxIdToReplay = oldHbGraphLastId;
        _frontierSet.insert(flipped.fullCanonical);
        _frontierSet.insert(flipped.conciseCanonical);
        ++frontier;
        _branchStats[flipped.pc.branch].condsSeen.insert(flipped.pc.lastCond);
        _branchStats[flipped.pc.branch].timesSeen++;
        if (DbgPathConstraints) {
          llvm::errs() << "PathConstraint.FlippedInput:";
          DumpKInstruction("\n@ ", flipped.pc.branch);
          std::cerr << "\n" << flipped.pc.fullSummary << "\n";
        }
      }
    }

    // Stats
    if (!flips.empty()) {
      _currStats->addForkingBranch(e->forkclass(), flips.size(),
                                   e->kinst()->inst->getParent()->getParent()->getNameStr());
    }

    // Update the path
    // TODO: could do a better job of canonicalizing: when adding a constraint, we could
    // TODO: check if any prior constraints are subsumed by the constraint being inserted
    // TODO: N.B.: we'd still need to update _exploredSet as we're doing here in order
    // TODO: to include all prefixes of the input in _exploredSet (could do this by taking
    // TODO: the conjunction of constraints in kstate->constraints())
    kstate->addConstraint(cond);
    addExplored(&pcBuilder, cond, oldHbGraphLastId);
  }

  solver->setTimeout(0);

  // Done with this path
  pcBuilder.trimExtraInvariants();
  _explored.push_back(new CoveredSchedule(pcBuilder.pc, oldHbGraph, programExited));
  if (DbgPathConstraints) {
    std::cerr << "PathConstraint.Final:\n" << pcBuilder.pc.fullSummary << "\n";
  }

  // DUMP
  if (DumpPathConstraints) {
    std::auto_ptr<std::ostream>
      os(jm->openTraceOutputFile("pathConstraints", "txt", "PathConstraints"));
    const PathConstraint::ListTy &list = pcBuilder.pc.list;
    for (size_t k = 0; k < list.size(); ++k) {
      if (list[k].fc != klee::KLEE_FORK_INVARIANT_INTERNAL_HIDDEN) {
        ref<Expr> expr = list[k].expr;
        // For test code: sort conjuncts and disjuncts
        // N.B.: for maximal effectiveness, enable --use-stable-array-hashing as well
        if (SortPathConstraintTerms && (isa<AndExpr>(expr) || isa<OrExpr>(expr))) {
          ImmutableExprSet set;
          for (ExprNaryIterator it(expr, expr->getKind()); it != ExprNaryIterator(); ++it) {
            set = set.insert(*it);
          }
          if (isa<AndExpr>(expr)) {
            expr = PathConstraintBuilder::serializeExprSet(set);
          } else {
            expr = PathConstraintBuilder::serializeExprSetDisjunct(set);
          }
        }
        (*os) << KleeForkClassToString(list[k].fc) << "\n"
              << expr << "\n\n";
      }
    }
  }

  // STATS
  _pathLengths[pcBuilder.length]++;
  _currStats->addFinishedPath(pcBuilder.pc, pcBuilder.length);

  CLOUD9_INFO("ProcessPath: " << _currStats->numTotalPaths() << " paths");
  CLOUD9_INFO("ProcessPath: important branches: "
              << important << "/" << slice.size() << " ("
              << 100.0*(double)(slice.size()-important)/(double)slice.size()
              << "% redundant)");
  CLOUD9_INFO("ProcessPath: frontier size: was " << (_frontier.size() - frontier)
              << ", now " << _frontier.size());
  CLOUD9_INFO("ProcessPath: concise vs full summary: "
              << pcBuilder.pc.conciseSummary->getSize() << " vs "
              << pcBuilder.pc.fullSummary->getSize());

  if (DbgProgress) {
    if (_explored.size() % 10 == 1)
      dumpFrontierSummary(kstate, jm);
  }

  // Check if anything on the frontier was recently subsumed
  // TODO: better?
  for (size_t i = 0; i < _frontier.size(); /* nop */) {
    const PathConstraint &pc = _frontier[i].pc;
    if (_exploredSet.count(pc.canonical)) {
      //llvm::errs() << "PathConstraint.Subsumed:\n" << pc.conciseSummary << "\n";
      _frontier.erase(_frontier.begin() + i);
    } else {
      i++;
    }
  }
}

void PathManager::instantiateNextInput(klee::ExecutionState *kstate,
                                       cloud9::worker::JobManager *jm) {
  klee::Executor *executor = kstate->executor;

  // Next input
  // N.B.: we pop() this at the end
  assert(!_frontier.empty());
  const QueuedPathConstraint &next = _frontier.front();
  const PathConstraint &pc = next.pc;
  _priorPC = pc;

  // DEBUG
  if (DbgSymbolicPaths) {
    std::cerr << "=====================================================\n"
              << "Starting next path (state:" << kstate->getCloud9State() << ")";
    DumpKInstruction("\n  ", pc.branch);
    std::cerr << "\n=====================================================\n";
  }

  if (DbgInputConstraintsVerbose) {
    for (PathConstraint::ListTy::const_iterator it = pc.list.begin(); it != pc.list.end(); ++it) {
      if (it->fc == klee::KLEE_FORK_INVARIANT_INPUT) {
        std::cerr << "InputConstraint:\n" << it->expr << "\n";
      } else {
        std::cerr << "InputInvariant." << KleeForkClassToString(it->fc) << "\n"
                  << it->expr << "\n";
      }
    }
  } else if (DbgInputConstraints) {
    std::cerr << "InputConstraint:\n" << pc.fullSummary << "\n";
  }

  // Apply the input constraint
  // We include all branch constraints as well as all extra constraints
  // that the branch constraints are dependent on.
  klee::ConstraintManager tmp;
  for (PathConstraint::ListTy::const_iterator it = pc.list.begin(); it != pc.list.end(); ++it) {
    if (it->fc != klee::KLEE_FORK_INVARIANT_INPUT)
      tmp.addConstraintNoSimplify(it->expr);
  }

  std::set< ref<Expr> > dependent;
  klee::getDependentConstraintsSet(klee::Query(tmp, pc.fullSummary), dependent);

  for (PathConstraint::ListTy::const_iterator it = pc.list.begin(); it != pc.list.end(); ++it) {
    if (it->fc == klee::KLEE_FORK_INVARIANT_INPUT || dependent.count(it->expr))
      executor->addConstraint(*kstate, it->expr, it->fc);
  }

  // This starts a new path which will partially replay an old HBGraph and old set of CtxSwitches
  kstate->hbGraph.startReplay(next.hbGraph, next.hbGraphMaxIdToReplay);
  kstate->ctxSwitchesToReplay = next.pc.ctxswitches;
  resetStatsForNewPath();

  // Pop "next" (invalidates the reference)
  _frontier.pop_front();
}

//-----------------------------------------------------------------------------
// PathManager: debugging and stats
//-----------------------------------------------------------------------------

struct FrontierBranchStats {
  klee::ExprHashSet lastCondSet;
  std::vector<ref<Expr> > lastConds;
  std::vector<ThreadUid> lastTuids;
  size_t explored;
  FrontierBranchStats() : explored(0) {}
};

void PathManager::dumpFrontierSummary(klee::ExecutionState *kstate,
                                      cloud9::worker::JobManager *jm) const {
  // Summarize all branches on the frontier
  std::map<klee::KInstruction*, FrontierBranchStats> branches;
  for (PathConstraintQueueTy::const_iterator
       it = _frontier.begin(); it != _frontier.end(); ++it) {
    FrontierBranchStats &stats = branches[it->pc.branch];
    stats.lastCondSet.insert(it->pc.lastCond);
    stats.lastConds.push_back(it->pc.lastCond);
    stats.lastTuids.push_back(it->pc.lastTuid);
  }
  const size_t frontierBranches = branches.size();

  // Include stats of all branches that have been explored
  for (std::map<klee::KInstruction*, BranchStats>::const_iterator
       it = _branchStats.begin(); it != _branchStats.end(); ++it) {
    FrontierBranchStats &stats = branches[it->first];
    stats.lastCondSet.insert(it->second.condsSeen.begin(), it->second.condsSeen.end());
    stats.explored = it->second.timesSeen;
  }

  std::auto_ptr<std::ostream>
    os(jm->openTraceOutputFile("frontierSummary", "txt", "FrontierSummary"));
  llvm::raw_os_ostream llos(*os);
  llos.SetUnbuffered();

  (*os) << "Frontier Size: " << _frontier.size() << "\n";
  (*os) << "Explored Size: " << _explored.size() << "\n";
  (*os) << "Frontier Branches: " << frontierBranches << "\n";
  (*os) << "Total Branches: " << branches.size() << "\n";

  (*os) << "\n======== PATH LENGTH HISTOGRAM ========\n";
  size_t maxPathLength = 0;
  size_t avgPathLength = 0;
  if (!_pathLengths.empty()) {
    maxPathLength = _pathLengths.rbegin()->first;
    size_t sum = 0;
    size_t cnt = 0;
    for (std::map<size_t,size_t>::const_iterator
         it = _pathLengths.begin(); it != _pathLengths.end(); ++it) {
      sum += it->second * it->first;
      cnt += it->second;
    }
    avgPathLength = sum / cnt;
  }
  (*os) << "avg :: " << avgPathLength << "\n";
  for (size_t k = 0; k <= maxPathLength; ++k) {
    std::map<size_t,size_t>::const_iterator it = _pathLengths.find(k);
    const size_t cnt = (it != _pathLengths.end()) ? it->second : 0;
    (*os) << k << " :: " << cnt << "\n";
  }

  (*os) << "\n======== BRANCHES QUICK SUMMARY ========";
  for (std::map<klee::KInstruction*, FrontierBranchStats>::iterator
       it = branches.begin(); it != branches.end(); ++it) {
    DumpKInstruction("\n  ", it->first, llos, true);
    (*os) << " (" << it->second.lastConds.size() << " on frontier, "
          << it->second.explored << " total, "
          << it->second.lastCondSet.size() << " unique conds)";
  }
  (*os) << "\n";

  for (std::map<klee::KInstruction*, FrontierBranchStats>::iterator
       it = branches.begin(); it != branches.end(); ++it) {
    (*os) << "\n=========== BRANCH ===========";
    DumpKInstruction("\n  ", it->first, llos);
    (*os) << "\n\nParent Loops:";
    llvm::BasicBlock *bb = it->first->inst->getParent();
    llvm::Loop *loop = GetLoopInfo(bb->getParent())->getLoopFor(bb);
    if (!loop) {
      (*os) << "\n  none";
    } else {
      for (; loop; loop = loop->getParentLoop()) {
        (*os) << "\n  header: " << loop->getHeader()->getNameStr();
        DumpKInstruction("\n    ", LookupKInstruction(kstate, loop->getHeader()->begin()), llos);
      }
    }
    (*os) << "\n\nLast Conditions On Frontier:\n";
    for (size_t k = 0; k < it->second.lastConds.size(); ++k) {
      (*os) << it->second.lastTuids[k] << "\n"
            << it->second.lastConds[k] << "\n\n";
    }
  }
}

void PathManager::resetStatsForNewPath() {
  _currStats->startPath();
}

void PathManager::dumpStats(cloud9::worker::JobManager *jm) const {
  std::cerr << "=====================================================\n"
            << "End of region stats:\n";
  _currStats->dumpTxt(std::cerr);
  std::cerr << "=====================================================\n";

  std::auto_ptr<std::ostream>
    os(jm->openTraceOutputFile("pathManagerStats", "csv", "PathManagerStats"));
  _currStats->dumpCsv(*os, true, jm->getTraceCounter("pathManagerStats"));
}

void PathManager::dumpGlobalStats(cloud9::worker::JobManager *jm) const {
  std::cerr << "=====================================================\n"
            << "Global stats:\n";
  MirroredKleeStats::GlobalStats->dumpTxt(std::cerr);
  std::cerr << "=====================================================\n";

  std::auto_ptr<std::ostream>
    os(jm->openTraceOutputFile("pathManagerGlobalStats", "csv", "PathManagerStats"));
  MirroredKleeStats::GlobalStats->dumpCsv(*os, true, 0);
}

}  // namespace ics
