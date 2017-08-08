//===-- SyncInvariantsUtil.h ------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_SYNCINVARIANTSUTIL_H
#define KLEE_SYNCINVARIANTSUTIL_H

#include "klee/Expr.h"
#include "klee/util/Ref.h"
#include "klee/util/ExprHashMap.h"
#include "klee/Internal/ADT/ImmutableSet.h"
#include "klee/Internal/ADT/ImmutableMap.h"
#include "klee/Threading.h"

#include <vector>
#include <set>

namespace llvm {
class DSAA;
class Value;
class Loop;
}

namespace klee {
class ExprVisitor;
class ExecutionState;

//-----------------------------------------------------------------------------
// Describes the address of a synchronization object
//-----------------------------------------------------------------------------

struct SyncObjectAddress {
public:
  ref<Expr> address;       // precise address, for must-alias (can be NULL)
  const llvm::Value *ptr;  // approximate address, for may-alias

public:
  SyncObjectAddress() : ptr(NULL) {}
  SyncObjectAddress(ref<Expr> a, const llvm::Value *p) : address(a), ptr(p) {}

  bool hasAddress() const {
    return address.get() != NULL;
  }

  bool operator==(const SyncObjectAddress &other) const {
    return (ptr == other.ptr)
        && (hasAddress() == other.hasAddress())
        && (!hasAddress() || (address == other.address));
  }
  bool operator!=(const SyncObjectAddress &other) const {
    return !(*this == other);
  }
  bool operator<(const SyncObjectAddress &other) const {
    if (!hasAddress() && other.hasAddress())
      return true;
    if (hasAddress() && !other.hasAddress())
      return false;
    if (hasAddress() && other.hasAddress()) {
      int cmp = address->compare(*other.address);
      if (cmp < 0) return true;
      if (cmp > 0) return false;
    }
    return ptr < other.ptr;
  }

  bool mayAlias(llvm::DSAA *dsaa, const SyncObjectAddress &other) const;
  bool mustAlias(const SyncObjectAddress &other) const;

  bool mayAliasViaSolver(ExecutionState &state, const SyncObjectAddress &other) const;
  bool mustAliasViaSolver(ExecutionState &state, const SyncObjectAddress &other) const;
};

//-----------------------------------------------------------------------------
// Describes the set of locks held by a thread
//-----------------------------------------------------------------------------

struct Lockset {
public:
  typedef ImmutableSet<SyncObjectAddress> SetTy;
  SetTy locks;  // TODO: need a counter to handle recursive locks?

private:
  struct RemoveIfMustAlias;
  bool _includesAll;

public:
  Lockset() : _includesAll(false) {}

  bool operator==(const Lockset &other) const { return locks == other.locks && _includesAll == other._includesAll; }
  bool operator!=(const Lockset &other) const { return !(*this == other); }

  void add(const SyncObjectAddress &lock);
  void add(ref<Expr> lockAddress, const llvm::Value *lockPointer);

  // Set to an empty locket
  void clear();

  // If true, this lockset includes all possible locks
  bool includesAll() const { return _includesAll; }
  void setIncludesAll();

  // Meaningless if "includesAll()" is true
  typedef SetTy::iterator iterator;
  iterator begin() const { return locks.begin(); }
  iterator end() const   { return locks.end(); }

  // Removes at most *one* lock from the set
  // Returns true if something was removed
  bool removeIfMustAlias(const SyncObjectAddress &lock);
  bool removeIfMustAlias(ref<Expr> lockAddress, const llvm::Value *lockPointer);

  bool removeIfMustAliasViaSolver(ExecutionState &state,
                                  const SyncObjectAddress &lock);
  bool removeIfMustAliasViaSolver(ExecutionState &state,
                                  ref<Expr> lockAddress, const llvm::Value *lockPointer);

  void unionWith(const Lockset &other);
  void intersectWith(const Lockset &other);
  void dump(const char *indent) const;
};

//-----------------------------------------------------------------------------
// Describes the set of locks held by all threads
//-----------------------------------------------------------------------------

typedef std::map<ThreadUid, Lockset> LocksetMapTy;

//-----------------------------------------------------------------------------
// Describes the set of initial counts for each barrier
//-----------------------------------------------------------------------------

struct BarrierInitCounts {
public:
  typedef ImmutableMap<SyncObjectAddress, ImmutableExprSet> MapTy;
  MapTy barriers;

public:
  BarrierInitCounts() {}

  bool operator==(const BarrierInitCounts &other) const { return barriers == other.barriers; }
  bool operator!=(const BarrierInitCounts &other) const { return !(*this == other); }

  void add(const SyncObjectAddress &barrier, ref<Expr> count);
  void add(ref<Expr> barrierAddress, const llvm::Value *barrierPointer, ref<Expr> count);
  void clear();

  typedef MapTy::iterator iterator;
  iterator begin() const { return barriers.begin(); }
  iterator end() const   { return barriers.end(); }
  bool empty() const     { return barriers.empty(); }

  void mergeWith(const BarrierInitCounts &other);
  void dump(const char *indent) const;
};

//-----------------------------------------------------------------------------
// Describes a sequence of barrier operations
//-----------------------------------------------------------------------------

struct BarrierSequence {
private:
  struct Loop;
  struct SeqNode {
    SeqNode() {}
    explicit SeqNode(ref<Loop> l) : loop(l) {}
    explicit SeqNode(const SyncObjectAddress &b) : barrier(b) {}
    // If (loop != NULL), this is a nested loop.
    // Otherwise it is a single barrier operation.
    ref<Loop> loop;
    SyncObjectAddress barrier;
  };

  typedef std::vector<SeqNode> Seq;
  struct Loop {
    Loop() : refCount(0), theLoop(NULL), parent(NULL) {}
    unsigned refCount;
    // When computing a BarrierSequence for a function, the top-level
    // function entry is represented by one of these Loops, but with
    // tripCount=null and theLoop=NULL.
    const llvm::Loop* theLoop;
    Loop* parent;         // the containing loop (N.B.: not a ref<> to avoid circularity)
    ref<Expr> tripCount;  // loop's symbolic trip count, or null if unknown
    Seq sequence;         // sequence of barrier operations in this loop
  };

private:
  ref<Loop> _root;
  ref<Loop> _end;

  static bool equals(ref<Loop> left, ref<Loop> right, const bool ignoreEmptyLoops);
  static void replaceExprs(ExprVisitor &visitor, ref<Loop> loop);

  static void getBarriers(std::set<SyncObjectAddress> *barriers, ref<Loop> loop);
  static void getTripCounts(ExprHashSet *tripCounts, ref<Loop> loop);
  static bool allValidTripCounts(ref<Loop> loop);
  static bool isNonEmpty(ref<Loop> loop);

  void copyFrom(const BarrierSequence &other, ref<Loop> from, ref<Loop> to);
  void copyFrom(const BarrierSequence &from);
  void dump(const char *indent, ref<Loop> loop) const;

public:
  BarrierSequence();

  // Assignment makes a deep copy!
  BarrierSequence(const BarrierSequence &rhs);
  BarrierSequence& operator=(const BarrierSequence &rhs);

  bool isValid() const {
    if (_root.get()) {
      assert(_end.get());
      assert(!_root->theLoop);
      return true;
    } else {
      return false;
    }
  }

  void makeInvalid() {
    _root = NULL;
    _end = NULL;
  }

  // Check deep structural equality
  bool operator==(const BarrierSequence &rhs) const;
  bool operator!=(const BarrierSequence &rhs) const;

  // Returns true iff this and rhs are both valid and structurally equivalent.
  // This checks deep structural equality like operator==, except this ignores empty loops.
  bool isCompatibleWith(const BarrierSequence &rhs) const;

  // Return true if this sequence is valid and has at least one operation
  bool isNonEmpty() const;

  // Enumerate all barriers by address
  void getBarriers(std::set<SyncObjectAddress> *barriers) const;

  // Enumerate all loop trip counts
  void getTripCounts(ExprHashSet *tripCounts) const;

  // Return true if all loop trip counts are non-null
  // Ignores _root and all empty loops
  bool allValidTripCounts() const;

  // Enumerate the stack of loops from youngest to oldest
  // Does not include the _root loop (which has a null theLoop)
  void getCurrLoopStack(std::vector<const llvm::Loop*> *loopstack) const;

  // Get the youngest loop on the loopstack
  // REQUIRES: a valid BarrierSequence
  const llvm::Loop* getCurrLoop() const;

  // Get the trip count of the youngest loop on the loopstack
  ref<Expr> getCurrLoopTripCount() const;

  // Returns true iff the youngest loop has nested barrier operations
  // REQUIRES: a valid BarrierSequence
  bool isCurrLoopNonEmpty() const;

  // Append a barrier or a sequence
  // Nops if the loop is invalid
  void append(const SyncObjectAddress &barrier);
  void append(ref<Expr> barrierAddress, const llvm::Value *barrierPointer);
  void append(const BarrierSequence &rhs);

  // Push/pop the current loop
  // Nops if !isValid
  void pushLoop(const llvm::Loop *theLoop);
  void popLoop();

  // Update the tripCount of the current loop
  // Nop if !isValid
  void setTripCountForCurrLoop(ref<Expr> tripCount);

  // Update the tripCount of the given loop, which much be in getCurrLoopStack()
  // Nop if !isValid
  void setTripCountForLoop(const llvm::Loop *theLoop, ref<Expr> tripCount);

  // Make invalid if this != rhs
  void intersectWith(const BarrierSequence &rhs);

  // Translate all exprs with the given visitor by modifying
  // this BarrierSequence in-place
  void replaceExprs(ExprVisitor &visitor);

  // DEBUG
  void dump(const char *indent) const;
};

}

#endif
