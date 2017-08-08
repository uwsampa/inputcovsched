/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * Keeps a trace of symbolic execution path that will be used to compute
 * a precondition slice of the path.
 *
 * TODO: use a union and placement-new to reduce space usage of Entrys?
 */

#ifndef ICS_PRECONDSLICETRACE_H
#define ICS_PRECONDSLICETRACE_H

#include "klee/Constants.h"
#include "klee/Expr.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Threading.h"
#include "klee/util/ExprHashMap.h"
#include "cloud9/worker/TreeNodeInfo.h"
#include "ics/ForkableTrace.h"

#include <boost/static_assert.hpp>
#include <vector>

namespace llvm { class BasicBlock; }
namespace llvm { class Value; }
namespace klee { class ExecutionState; }
namespace klee { class MemoryObject; }
namespace klee { class SymbolicObject; }
namespace klee { class SymbolicPtrResolution; }
namespace cloud9 { namespace worker { class JobManager; } }

namespace ics {

typedef klee::ImmutableMap<klee::ref<klee::Expr>, llvm::BasicBlock*> FlippedConditionsMapTy;
typedef klee::ImmutableSet<klee::ThreadUid> ThreadSetTy;

//-----------------------------------------------------------------------------
// An execution trace
//-----------------------------------------------------------------------------

class PrecondSliceTrace {
public:
  enum Kind {
    // A symbolic constraint (the branch constraints get merged into ControlTransfers)
    Assertion = 0x10,

    // ControlTransfers
    Branch    = 0x20,
    Call      = 0x21,
    Ret       = 0x22,
    CtxSwitch = 0x23,

    // MemoryAccesses
    Load  = 0x40,
    Store = 0x41,

    // Memory allocation
    Allocation = 0x80,

    // Call to klee_ics_region_marker()
    RegionMarker = 0x100,

    // Fork point for resolving a symbolic pointer
    ResolvedSymbolicPtr = 0x200
  };

  static bool IsControlTransfer(Kind k) {
    return (k & 0x20) != 0;
  }

  static bool IsMemoryAccess(Kind k) {
    return (k & 0x40) != 0;
  }

  BOOST_STATIC_ASSERT((Assertion    & 0x20) == 0);
  BOOST_STATIC_ASSERT((Branch       & 0x20) != 0);
  BOOST_STATIC_ASSERT((Call         & 0x20) != 0);
  BOOST_STATIC_ASSERT((Ret          & 0x20) != 0);
  BOOST_STATIC_ASSERT((CtxSwitch    & 0x20) != 0);
  BOOST_STATIC_ASSERT((Load         & 0x20) == 0);
  BOOST_STATIC_ASSERT((Store        & 0x20) == 0);
  BOOST_STATIC_ASSERT((Allocation   & 0x20) == 0);
  BOOST_STATIC_ASSERT((RegionMarker & 0x20) == 0);
  BOOST_STATIC_ASSERT((ResolvedSymbolicPtr & 0x20) == 0);

  BOOST_STATIC_ASSERT((Assertion    & 0x40) == 0);
  BOOST_STATIC_ASSERT((Branch       & 0x40) == 0);
  BOOST_STATIC_ASSERT((Call         & 0x40) == 0);
  BOOST_STATIC_ASSERT((Ret          & 0x40) == 0);
  BOOST_STATIC_ASSERT((CtxSwitch    & 0x40) == 0);
  BOOST_STATIC_ASSERT((Load         & 0x40) != 0);
  BOOST_STATIC_ASSERT((Store        & 0x40) != 0);
  BOOST_STATIC_ASSERT((Allocation   & 0x40) == 0);
  BOOST_STATIC_ASSERT((RegionMarker & 0x40) == 0);
  BOOST_STATIC_ASSERT((ResolvedSymbolicPtr & 0x40) == 0);

  //
  // A single entry in an execution trace
  //

  class Entry {
  private:
    uint32_t            _type;
    klee::ThreadUid     _tuid;
    klee::KInstruction *_kinst;

    // For Assertion, the constraint
    // For ControlTransfer, the branch condition (null for unconditional)
    // For MemoryAccess, the offset
    klee::ref<klee::Expr> _e;

    // For MemoryAccess, the number of bytes touched by the access
    klee::ref<klee::Expr> _nbytes;

    // For MemoryAccess,Allocation
    const klee::MemoryObject* _mo;

    // For ControlTransfer, the value of hbGraph->getMaxNodeId() when this instr executed
    uint32_t _number;

    // For ControlTransfer
    // After executing <_tuid,_kinst>, control transfers to <_targetTuid,_targetKinst>
    klee::ThreadUid     _targetTuid;
    klee::KInstruction *_targetKinst;

    // For ResolvedSymbolicPtr
    klee::ref<klee::SymbolicPtrResolution> _ptrResolution;

    // For Assertion,ControlTransfer
    // Rather than using !condition() as the flipped path constraint,
    // we should use this set of conditions as the "flipped" alternatives.
    // This maps alternate conditions to target basic blocks (targets are
    // non-NULL at branch/switch instr only).
    FlippedConditionsMapTy _flippedConditions;

    // For CtxSwitch
    ThreadSetTy _otherCtxSwitches;

    // Specification of the '_type' field
    enum {
      UseFlippedConditions  = (1<<31),             // bit 31
      HasFlippableCtxSwitch = (1<<30),             // bit 30
      ForkClassMask  = ((1<<8) - 1),               // bits 12-19
      ForkClassShift = 12,
      KindMask       = ((1<<ForkClassShift) - 1)   // bits 0-11
    };

    static uint32_t MakeType(Kind kind, klee::ForkClass forkclass) {
      return kind | (forkclass << ForkClassShift);
    }

    static uint32_t ChangeTypeKind(uint32_t type, Kind newkind) {
      return newkind | (type & ~KindMask);
    }

    static uint32_t ChangeTypeForkClass(uint32_t type, klee::ForkClass newclass) {
      return (newclass << ForkClassShift) | (type & ~(ForkClassMask << ForkClassShift));
    }

    BOOST_STATIC_ASSERT((unsigned)KindMask > (unsigned)ResolvedSymbolicPtr);
    BOOST_STATIC_ASSERT((unsigned)ForkClassMask > (unsigned)klee::KLEE_FORK_MAXCLASS);
    friend class PrecondSliceTrace;

  public:
    // Converts this 'Branch' entry into an Assertion entry by stripping
    // out the 'target' and chaning the 'kind'
    // REQUIRES: kind == Branch
    Entry convertBranchToAssertion() const;

  public:
    Entry(Kind kind, klee::ForkClass forkclass, klee::ExecutionState *kstate);

    Kind kind() const {
      return Kind(_type & KindMask);
    }

    klee::ThreadUid tuid() const {
      return _tuid;
    }

    klee::KInstruction* kinst() const {
      return _kinst;
    }

    // Always safe to call

    bool mightHaveCondition() const {
      return kind() == Assertion || kind() == Branch || kind() == Call;
    }

    bool hasCondition() const {
      return mightHaveCondition() && condition().get() != NULL;
    }

    // ControlTransfer,Assertion

    klee::ForkClass forkclass() const {
      return klee::ForkClass((_type >> ForkClassShift) & ForkClassMask);
    }

    const klee::ref<klee::Expr>& condition() const {
      assert(mightHaveCondition());
      return _e;
    }

    bool useFlippedConditions() const {
      assert(mightHaveCondition());
      return (_type & UseFlippedConditions) != 0;
    }

    FlippedConditionsMapTy flippedConditions() const {
      assert(mightHaveCondition());
      return _flippedConditions;
    }

    klee::hbnode_id_t maxHbNodeId() const {
      assert(mightHaveCondition());
      return _number;
    }

    // ControlTransfer

    klee::ThreadUid targetTuid() const {
      assert(IsControlTransfer(kind()));
      return _targetTuid;
    }

    klee::KInstruction* targetKinst() const {
      assert(IsControlTransfer(kind()));
      return _targetKinst;
    }

    ThreadSetTy otherCtxSwitches() const {
      assert(kind() == CtxSwitch);
      return _otherCtxSwitches;
    }

    bool hasOtherCtxSwitches() const {
      return (_type & HasFlippableCtxSwitch) != 0;
    }

    // MemoryAccess

    const klee::MemoryObject* memoryobject() const {
      assert(IsMemoryAccess(kind()) || kind() == Allocation);
      return _mo;
    }

    const klee::ref<klee::Expr>& offset() const {
      assert(IsMemoryAccess(kind()));
      return _e;
    }

    const klee::ref<klee::Expr>& nbytes() const {
      assert(IsMemoryAccess(kind()));
      return _nbytes;
    }

    // ResolvedSymbolicPtr

    const klee::SymbolicPtrResolution* ptrResolution() const {
      assert(kind() == ResolvedSymbolicPtr);
      assert(_ptrResolution.get());
      return _ptrResolution.get();
    }
  };

public:
  typedef ForkableTrace<Entry> trace_ty;

  typedef trace_ty::const_iterator          const_iterator;
  typedef trace_ty::const_reverse_iterator  const_reverse_iterator;

private:
  // A complete trace
  trace_ty _trace;

public:
  // Add an entry
  void pushAssertion(klee::ExecutionState *kstate,
                     klee::ref<klee::Expr> condition,
                     klee::ForkClass forkclass);

  void pushControlTransfer(klee::ExecutionState *kstate,
                           klee::ThreadUid targetTuid,
                           klee::KInstruction *targetKinst,
                           Kind kind);

  void pushMemoryAccess(klee::ExecutionState *kstate,
                        const klee::MemoryObject* mo,
                        klee::ref<klee::Expr> offset,
                        klee::ref<klee::Expr> nbytes,
                        Kind kind);

  void pushAllocation(klee::ExecutionState *kstate,
                      const klee::MemoryObject* mo);

  void pushRegionMarker(klee::ExecutionState *kstate,
                        klee::ThreadUid tuid);

  void pushResolvedSymbolicPtr(klee::ExecutionState *kstate,
                               klee::ref<klee::SymbolicPtrResolution> res);

  void pushEntry(const Entry &e) {
    _trace.push_back(e);
  }

  // Attach flipped conditions to back()
  // REQUIRES: trace.back() is an assertion or branch
  void attachFlippedConditions(const FlippedConditionsMapTy &set);

  // Change the condition of back()
  // REQUIRES: trace.back() is an assertion or branch
  void attachNewCondition(klee::ref<klee::Expr> cond);

  // Attach a set of otherCtxSwitches to back()
  // REQUIRES: trace.back() is a CtxSwitch
  void attachOtherCtxSwitches(const ThreadSetTy &set);

  // Whole-trace ops
  void clear() { _trace.clear(); }
  void reverse() { _trace.reverse(); }
  void swap(PrecondSliceTrace &rhs) { _trace.swap(rhs._trace); }

  bool empty() const { return _trace.empty(); }
  size_t size() const { return _trace.size(); }

  // For iteration
  const trace_ty& trace() const { return _trace; }

  const_iterator begin() const { return _trace.begin(); }
  const_iterator end() const { return _trace.end(); }
  const_reverse_iterator rbegin() const { return _trace.rbegin(); }
  const_reverse_iterator rend() const { return _trace.rend(); }

  const Entry& front() const { return _trace.front(); }
  const Entry& back() const { return _trace.back(); }

  // Debugging
  void sanityCheckTrace();
  void dumpTrace(cloud9::worker::JobManager* jobManager, const char* filename);
};


}  // namespace ics

#endif
