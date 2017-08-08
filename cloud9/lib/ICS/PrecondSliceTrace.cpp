/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * Keeps a trace of symbolic execution path that will be used to compute
 * a precondition slice of the path.
 *
 */

#include "ics/PrecondSliceTrace.h"

#include "../lib/Core/Common.h"
#include "../lib/Core/Memory.h"
#include "klee/Constants.h"
#include "klee/ExecutionState.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "cloud9/worker/JobManager.h"
#include "cloud9/worker/TreeObjects.h"

#include "llvm/Instruction.h"
#include "llvm/Instructions.h"
#include "llvm/Function.h"
#include "llvm/BasicBlock.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>

using cloud9::worker::JobManager;
using namespace llvm;

namespace ics {

namespace {
cl::opt<bool> DumpExecutionTraceStats("ics-dump-execution-trace-stats", cl::init(false));

}  // namespace

//-----------------------------------------------------------------------------
// Execution trace entry structs
//-----------------------------------------------------------------------------

PrecondSliceTrace::Entry::Entry(Kind kind, klee::ForkClass forkclass, klee::ExecutionState *kstate)
  : _type(MakeType(kind, forkclass)),
    _tuid(kstate->crtThreadUid()),
    _kinst(kstate->prevPC()),
    _targetKinst(0)
{}

PrecondSliceTrace::Entry
PrecondSliceTrace::Entry::convertBranchToAssertion() const {
  assert(kind() == Branch);
  assert(condition().get());
  Entry e = *this;
  e._type = ChangeTypeKind(e._type, Assertion);
  e._type = ChangeTypeForkClass(e._type, klee::KLEE_FORK_CONVERTED_BRANCH);
  e._targetKinst = NULL;
  assert(e.kind() == Assertion);
  return e;
}

//-----------------------------------------------------------------------------
// Execution traces
//-----------------------------------------------------------------------------

void PrecondSliceTrace::pushAssertion(klee::ExecutionState *kstate,
                                      klee::ref<klee::Expr> condition,
                                      klee::ForkClass forkclass) {
  Entry e(Assertion, forkclass, kstate);
  e._e = condition;
  e._number = kstate->hbGraph.getMaxNodeId();
  _trace.push_back(e);
}

void PrecondSliceTrace::pushControlTransfer(klee::ExecutionState *kstate,
                                            klee::ThreadUid targetTuid,
                                            klee::KInstruction *targetKinst,
                                            Kind kind) {
  // CtxSwitch should actually switch threads
  if (kind == CtxSwitch) {
    assert(targetTuid != kstate->crtThreadUid());
  }

  // Merge with a prior Assertion?
  if (!_trace.empty() && (kind == Call || kind == Branch)) {
    Entry &e = _trace.back();
    if (e.kind() == Assertion &&
        e.tuid() == kstate->crtThreadUid() &&
        e.kinst() == kstate->prevPC() &&
        e.forkclass() != klee::KLEE_FORK_INVARIANT_INPUT) {
      e._type = Entry::ChangeTypeKind(e._type, kind);
      e._targetTuid = targetTuid;
      e._targetKinst = targetKinst;
      e._number = kstate->hbGraph.getMaxNodeId();
      return;
    }
  }

  // Make a new ControlTransfer
  Entry e(kind, klee::KLEE_FORK_BRANCH, kstate);
  e._targetTuid = targetTuid;
  e._targetKinst = targetKinst;
  e._number = kstate->hbGraph.getMaxNodeId();

  _trace.push_back(e);
}

void PrecondSliceTrace::pushMemoryAccess(klee::ExecutionState *kstate,
                                         const klee::MemoryObject* mo,
                                         klee::ref<klee::Expr> offset,
                                         klee::ref<klee::Expr> nbytes,
                                         Kind kind) {
  assert(kind == Load || kind == Store);
  Entry e(kind, klee::ForkClass(0), kstate);
  e._mo = mo;
  e._e = offset;
  e._nbytes = nbytes;
  _trace.push_back(e);
}

void PrecondSliceTrace::pushAllocation(klee::ExecutionState *kstate,
                                       const klee::MemoryObject* mo) {
  Entry e(Allocation, klee::ForkClass(0), kstate);
  e._mo = mo;
  _trace.push_back(e);
}

void PrecondSliceTrace::pushRegionMarker(klee::ExecutionState *kstate,
                                         klee::ThreadUid tuid) {
  Entry e(RegionMarker, klee::ForkClass(0), kstate);
  // Need to fixup the thread id
  e._tuid = tuid;
  e._kinst = kstate->threads.find(tuid)->second.getPrevPC();
  _trace.push_back(e);
}

void PrecondSliceTrace::pushResolvedSymbolicPtr(klee::ExecutionState *kstate,
                                                klee::ref<klee::SymbolicPtrResolution> res) {
  Entry e(ResolvedSymbolicPtr, klee::KLEE_FORK_POINTER_ALIASING, kstate);
  e._ptrResolution = res;
  _trace.push_back(e);
}

void PrecondSliceTrace::attachFlippedConditions(const FlippedConditionsMapTy &f) {
  // Must have a have a prior Assertion
  assert(!_trace.empty());
  Entry &e = _trace.back();
  assert(e.kind() == Assertion || e.kind() == Branch);
  e._flippedConditions = f;
  e._type |= Entry::UseFlippedConditions;
}

void PrecondSliceTrace::attachNewCondition(klee::ref<klee::Expr> cond) {
  // Must have a have a prior Assertion
  assert(!_trace.empty());
  Entry &e = _trace.back();
  assert(e.kind() == Assertion || e.kind() == Branch);
  e._e = cond;
}

void PrecondSliceTrace::attachOtherCtxSwitches(const ThreadSetTy &s) {
  // Must have a have a prior CtxSwitch
  assert(!_trace.empty());
  Entry &e = _trace.back();
  assert(e.kind() == CtxSwitch);
  // Must not include the actual "next thread" in the set of others
  assert(!s.lookup(e.targetTuid()));
  e._otherCtxSwitches = s;
  e._type |= Entry::HasFlippableCtxSwitch;
}

// Debugging
#define DUMP_FILE_LOC(kinst) \
  do { \
    if ((kinst) && (kinst)->info->file.size()) \
      (*os) << (kinst) << " " << (kinst)->info->file << ":" << (kinst)->info->line; \
    else if (kinst) \
      (*os) << (kinst) << " ??:?? [in: " << (kinst)->inst->getParent()->getParent()->getNameStr() \
            << "]"; \
    else \
      (*os) << "??:??"; \
  } while (0)

#define DUMP_TRACE_ENTRY(msg) \
  do { \
    (*os) << i->tuid() << " " << msg << " @ ";\
    DUMP_FILE_LOC(i->kinst()); \
    (*os) << "\n"; \
  } while (0)

void PrecondSliceTrace::sanityCheckTrace() {
  std::ostream *os = &std::cerr;
  std::map<int,size_t> kstats;
  std::map<int,size_t> fstats;
  size_t fstatsTotal = 0;
  size_t uniquePredStat = 0;

  for (const_iterator i = begin(); i != end(); ++i) {
    // Stats
    kstats[i->kind()]++;
    if ((i->kind() == Assertion || i->kind() == Branch) && i->condition().get()) {
      fstats[i->forkclass()]++;
      fstatsTotal++;
    }
    if (i->kind() == Branch) {
      llvm::BasicBlock *bb = i->targetKinst()->inst->getParent();
      if (pred_end(bb) == ++pred_begin(bb))
        uniquePredStat++;
    }
    // Sanity check
    switch (i->kind()) {
      case Assertion:
        assert(i->kinst());
        assert(i->kinst()->inst);
        assert(i->condition().get());
        break;
      case Branch:
      case Call:
      case Ret:
      case CtxSwitch: {
        const_iterator next = i;
        ++next;
        if (next != end()) {
          assert(i->targetTuid() == next->tuid());
        }
        assert(i->kinst());
        assert(i->kinst()->inst);
        if (i->kind() == CtxSwitch) {
          assert(i->targetTuid() != i->tuid());
        } else {
          assert(i->targetTuid() == i->tuid());
        }
        if (i->hasCondition()) {
          assert(i->kind() == Branch || i->kind() == Call);
        }
        break;
      }
      case Load:
      case Store:
      case Allocation:
        assert(i->kinst());
        assert(i->kinst()->inst);
        assert(i->memoryobject());
        if (i->kind() == Load || i->kind() == Store) {
          assert(!i->offset().isNull());
          assert(!i->nbytes().isNull());
        }
        break;
      case RegionMarker:
        assert(i->kinst());
        assert(i->kinst()->inst);
        break;
      case ResolvedSymbolicPtr:
        assert(i->kinst());
        assert(i->kinst()->inst);
        assert(i->ptrResolution());
        {
          const klee::SymbolicPtrResolution *res = i->ptrResolution();
          assert(res->pointer);
          assert(res->targetAddress.get());
          assert(!res->targetAddress->isZero());
          if (res->uniqueSourceObj) {
            assert(res->uniqueSourceOffset.get());
          }
          if (res->uniqueSourceOffset.get()) {
            assert(res->uniqueSourceObj);
          }
          if (res->targetObj) {
            assert(res->targetObj->ptr);
            assert(res->targetObj->array);
            assert(res->targetObj->mo);
          }
          // XXX: not true for function pointers ...
          //assert(res->targetObj);
        }
        break;
      default:
        (*os) << "BAD TRACE ENTRY (unknown)\n";
        if (i->kinst()) {
          DUMP_TRACE_ENTRY("??? kind:" << i->kind());
        } else {
          (*os) << i->tuid() << " ??? kind:" << i->kind() << "\n";
        }
        assert(false);
        break;
    }
  }

  if (!DumpExecutionTraceStats)
    return;

  std::cerr << "Trace stats\n";
  for (std::map<int,size_t>::iterator it = kstats.begin(); it != kstats.end(); ++it) {
    double r = (double)it->second / (double)_trace.size();
    switch (it->first) {
      case Assertion:      std::cerr << "  assert: "; break;
      case Branch:         std::cerr << "  branch: "; break;
      case Call:           std::cerr << "  call:   "; break;
      case Ret:            std::cerr << "  ret:    "; break;
      case CtxSwitch:      std::cerr << "  ctxsw:  "; break;
      case Load:           std::cerr << "  load:   "; break;
      case Store:          std::cerr << "  store:  "; break;
      case Allocation:     std::cerr << "  alloc:  "; break;
      case RegionMarker:   std::cerr << "  region: "; break;
      case ResolvedSymbolicPtr: std::cerr << "  resolveptr: "; break;
      default: std::cerr << "  ??:" << it->first << ": "; break;
    }
    std::cerr << it->second << " (" << (r*100.0) << "% of total";
    if (it->first == Branch) {
      r = (double)uniquePredStat / (double)it->second;
      std::cerr << ", of which " << (r*100.0) << "% have a unique predecessor";
    }
    std::cerr << ")\n";
  }
  std::cerr << "  Total:  " << _trace.size() << "\n";

  std::cerr << "Trace constraint stats\n";
  for (std::map<int,size_t>::iterator it = fstats.begin(); it != fstats.end(); ++it) {
    double r = (double)it->second / (double)fstatsTotal;
    switch (it->first) {
      case klee::KLEE_FORK_BRANCH:          std::cerr << "  branch:        "; break;
      case klee::KLEE_FORK_FAULTINJ:        std::cerr << "  fault-inject:  "; break;
      case klee::KLEE_FORK_RESOLVE_FN_POINTER:
                                            std::cerr << "  resolve-fn-ptr: "; break;
      case klee::KLEE_FORK_SCHEDULE:        std::cerr << "  sched:         "; break;
      case klee::KLEE_FORK_SCHEDULE_MULTI:  std::cerr << "  sched-multi:   "; break;
      case klee::KLEE_FORK_INTERNAL:        std::cerr << "  internal:      "; break;
      case klee::KLEE_FORK_ASSUME:          std::cerr << "  assume:        "; break;
      case klee::KLEE_FORK_CONVERTED_BRANCH:std::cerr << "  converted-brach: "; break;
      case klee::KLEE_FORK_INVARIANT_INTERNAL:
                                            std::cerr << "  invariant-internal: "; break;
      case klee::KLEE_FORK_INVARIANT_INTERNAL_HIDDEN:
                                            std::cerr << "  invariant-hidden: "; break;
      case klee::KLEE_FORK_INVARIANT_INPUT: std::cerr << "  invariant-input: "; break;
      case klee::KLEE_FORK_CONCRETIZE_FP:   std::cerr << "  concrete-fp:   "; break;
      case klee::KLEE_FORK_CONCRETIZE_ADDR_ON_SOLVER_FAILURE:
                                            std::cerr << "  concrete-addr-onfail: "; break;
      case klee::KLEE_FORK_CONCRETIZE_ADDR_ON_BIG_SYMBOLIC_ARRAY:
                                            std::cerr << "  concrete-addr-onbigarray: "; break;
      case klee::KLEE_FORK_POINTER_ALIASING:
                                            std::cerr << "  pointer-aliasing: "; break;
      case klee::KLEE_FORK_BOUNDS_CHECK:    std::cerr << "  bounds-check: "; break;
      default: std::cerr << "  ??:" << it->first << ": "; break;
    }
    std::cerr << it->second << " (" << (r*100.0) << "% of total)\n";
  }

  std::cerr << "Trace memory stats\n";
  {
    size_t n, maxE, minE;
    std::string nodes;
    _trace.collectStats(&n, &nodes, &maxE, &minE);
    std::cerr << "  nodes: " << n << "\n";
    std::cerr << "  nodeSummary: " << nodes << "\n";
    std::cerr << "  maxPerNode: " << maxE << "\n";
    std::cerr << "  minPerNode: " << minE << "\n";
  }
}

void PrecondSliceTrace::dumpTrace(JobManager* jobManager, const char* filename) {
  std::auto_ptr<std::ostream>
    os(jobManager->openTraceOutputFile(filename, "txt", "PrecondSlice"));

  for (const_iterator i = begin(); i != end(); ++i) {
    switch (i->kind()) {
      case Assertion:
        (*os) << i->tuid();
        (*os) << " " << i->kinst()->inst->getOpcodeName()
              << " assertion (" << i->forkclass() << ")" ;
        DUMP_FILE_LOC(i->kinst());
        (*os) << "\n         cond " << i->condition();
        for (FlippedConditionsMapTy::iterator
             e = i->flippedConditions().begin(); e != i->flippedConditions().end(); ++e) {
          (*os) << "\n         flipped " << (e->second ? e->second->getNameStr() : std::string(""))
                << " " << e->first;
        }
        (*os) << "\n";
        break;
      case Branch:
      case Call:
      case Ret:
      case CtxSwitch:
        (*os) << i->tuid();
        if (i->kind() == CtxSwitch) {
          (*os) << " ctxswitch ";
          DUMP_FILE_LOC(i->kinst());
          (*os) << "\n        to " << i->targetTuid() << " ";
        } else {
          (*os) << " " << i->kinst()->inst->getOpcodeName()
                << (i->kind() == Branch ? " branch " :
                   (i->kind() == Call   ? " fncall " :
                   (i->kind() == Ret    ? " fnret " : " ??? ")));
          if (i->kind() == Call) {
            (*os) << " " << i->targetKinst()->inst->getParent()->getParent()->getNameStr() << " ";
          }
          (*os) << "(" << i->forkclass() << ") ";
          DUMP_FILE_LOC(i->kinst());
          (*os) << "\n             to ";
        }
        if (i->targetKinst()) {
          DUMP_FILE_LOC(i->targetKinst());
        } else {
          (*os) << "<external call>";
        }
        if (i->condition().get())
          (*os) << "\n        cond " << i->condition();
        for (FlippedConditionsMapTy::iterator
             e = i->flippedConditions().begin(); e != i->flippedConditions().end(); ++e) {
          (*os) << "\n         flipped " << (e->second ? e->second->getNameStr() : std::string(""))
                << " " << e->first;
        }
        (*os) << "\n";
        break;
      case Load:
        DUMP_TRACE_ENTRY("ld " << i->memoryobject() << "," << i->offset() << "," << i->nbytes());
        break;
      case Store:
        DUMP_TRACE_ENTRY("st " << i->memoryobject() << "," << i->offset() << "," << i->nbytes());
        break;
      case Allocation:
        DUMP_TRACE_ENTRY("alloc " << i->memoryobject());
        break;
      case RegionMarker:
        DUMP_TRACE_ENTRY("region-marker");
        break;
      case ResolvedSymbolicPtr:
        DUMP_TRACE_ENTRY("resolved-symbolic-ptr " << i->ptrResolution()->targetObj << ","
                          << i->ptrResolution()->targetAddress);
        break;
      default:
        if (i->kinst()) {
          DUMP_TRACE_ENTRY("??? kind:" << i->kind());
        } else {
          (*os) << i->tuid();
          (*os) << " ??? kind:" << i->kind() << "\n";
        }
        break;
    }
  }
}

}  // namespace ics
