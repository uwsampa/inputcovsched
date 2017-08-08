//===-- Constraints.h -------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_CONSTRAINTS_H
#define KLEE_CONSTRAINTS_H

#include "klee/Expr.h"
#include "klee/util/ExprUtil.h"

// FIXME: Currently we use ConstraintManager for two things: to pass
// sets of constraints around, and to optimize constraints. We should
// move the first usage into a separate data structure
// (ConstraintSet?) which ConstraintManager could embed if it likes.
namespace klee {
class ExprVisitor;
  
class ConstraintManager {
public:
  // N.B.: our summary AddExpr acts like a stack -- younger expressions
  // are closer to the top -- so this iterates constraints in FIFO order
  typedef ExprNaryIterator iterator;

public:
  ConstraintManager() { clear(); }

  ConstraintManager(const ConstraintManager &cs)
    : _summary(cs._summary), _size(cs._size) {}

  // create from constraints with no optimization
  explicit ConstraintManager(const std::vector< ref<Expr> > &constraints);

  // create from a summary expr with no optimization
  explicit ConstraintManager(const ref<Expr> &summaryExpr);

  ref<Expr> simplifyExpr(ref<Expr> e) const;
  ref<Expr> getSummaryExpr() const { return _summary; }

  void addConstraint(ref<Expr> e);            // add with simplification
  bool addConstraintCanFail(ref<Expr> e);     // add with simplification, return false if invalid
  void addConstraintNoSimplify(ref<Expr> e);  // add w/out simplification (used in special cases)

  void clear() {
    _summary = Expr::True;
    _size = 0;
  }
  
  bool empty() const {
    return _summary == Expr::True;
  }
  iterator begin() const {
    return empty() ? end() : ExprNaryIterator(_summary, Expr::And);
  }
  iterator end() const {
    return ExprNaryIterator();
  }
  size_t size() const {
    return _size;
  }

  bool operator==(const ConstraintManager &rhs) const {
    return _summary == rhs._summary;
  }
  bool operator!=(const ConstraintManager &rhs) const {
    return !(*this == rhs);
  }

  ConstraintManager& operator=(const ConstraintManager &rhs) {
    _summary = rhs._summary;
    _size = rhs._size;
    return *this;
  }

private:
  ref<Expr> _summary;
  size_t _size;
  friend class PushConstraints;

  // add without simplification
  void pushConstraint(ref<Expr> e);

  // returns false on failure
  bool addConstraintWithSimplification(ref<Expr> e, bool canFail);
  bool rewriteConstraints(ExprVisitor &visitor, bool canFail);
};

////////////////////////////////////////////////////////////////////////
// A way to push/pop constraints in a scoped manner.
// Example:
//   {
//      // Save constraints (will be restored when "saved" goes out-of-scope)
//      PushConstraints saved(state.globalConstraints);
//      state.addConstraint(..)
//      ...
//   }

class PushConstraints {
private:
  // DO NOT IMPLEMENT
  PushConstraints(const PushConstraints&);
  void operator=(const PushConstraints&);

  ConstraintManager &_cm;
  ConstraintManager _original;
  bool _popped;

public:
  explicit PushConstraints(ConstraintManager &cm)
    : _cm(cm), _original(cm), _popped(false) {}

  ~PushConstraints() {
    if (!_popped)
      pop();
  }

  void pop() {
    assert(!_popped);
    _cm = _original;
    _popped = true;
  }

  const ConstraintManager& getOriginal() const {
    return _original;
  }
};

}

#endif /* KLEE_CONSTRAINTS_H */
