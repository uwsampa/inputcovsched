//===-- Constraints.cpp ---------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Constraints.h"

#include "klee/util/ExprPPrinter.h"
#include "klee/util/ExprVisitor.h"

#include <iostream>
#include <map>

namespace klee {

//-----------------------------------------------------------------------------
// Simple Constructors
//-----------------------------------------------------------------------------

ConstraintManager::ConstraintManager(const std::vector< ref<Expr> > &constraints) {
  clear();
  for (std::vector< ref<Expr> >::const_iterator
       it = constraints.begin(); it != constraints.end(); ++it) {
    pushConstraint(*it);
  }
}

ConstraintManager::ConstraintManager(const ref<Expr> &summaryExpr) {
  clear();
  _summary = summaryExpr;
  for (iterator it = begin(); it != end(); ++it) {
    ++_size;
  }
}

void ConstraintManager::pushConstraint(ref<Expr> e) {
  _summary = AndExpr::create(e, _summary);
  _size++;
}

//-----------------------------------------------------------------------------
// Expr Simplification
//-----------------------------------------------------------------------------

class ExprReplaceVisitor2 : public ExprVisitor {
private:
  const std::map< ref<Expr>, ref<Expr> > &replacements;

public:
  ExprReplaceVisitor2(const std::map< ref<Expr>, ref<Expr> > &_replacements) 
    : ExprVisitor(true),
      replacements(_replacements) {}

  Action visitExprPost(const Expr &e) {
    std::map< ref<Expr>, ref<Expr> >::const_iterator it =
      replacements.find(ref<Expr>(const_cast<Expr*>(&e)));
    if (it!=replacements.end()) {
      return Action::changeTo(it->second);
    } else {
      return Action::doChildren();
    }
  }
};

// TODO: Ideally when rewriting constraints, if we can choose between two
// expressions e1==e2, we should choose the "least complex" of the two.

bool ConstraintManager::rewriteConstraints(ExprVisitor &visitor, bool canFail) {
  bool changed = false;

  ConstraintManager old(*this);
  clear();

  for (iterator it = old.begin(), ie = old.end(); it != ie; ++it) {
    ref<Expr> ce = *it;
    ref<Expr> e = visitor.visit(ce);

    if (e != ce) {
      if (!addConstraintWithSimplification(e, canFail)) // enable further reductions
        return false;
      changed = true;
    } else {
      pushConstraint(ce);
    }
  }

  if (!changed) {
    // The above loop actually inverted the order of terms.
    // On no change, keep the prior ordering to maximize sharing
    // across states.
    *this = old;
  }

  return true;
}

ref<Expr> ConstraintManager::simplifyExpr(ref<Expr> e) const {
  if (isa<ConstantExpr>(e))
    return e;

  std::map< ref<Expr>, ref<Expr> > equalities;
  
  for (iterator it = begin(), ie = end(); it != ie; ++it) {
    if (const EqExpr *ee = dyn_cast<EqExpr>(*it)) {
      if (isa<ConstantExpr>(ee->left)) {
        equalities.insert(std::make_pair(ee->right, ee->left));
      } else {
        equalities.insert(std::make_pair(*it, Expr::True));
      }
    } else {
      equalities.insert(std::make_pair(*it, Expr::True));
    }
  }

  return ExprReplaceVisitor2(equalities).visit(e);
}

bool ConstraintManager::addConstraintWithSimplification(ref<Expr> e, bool canFail) {
  // XXX should profile the effects of this and the overhead.
  // traversing the constraints looking for equalities is hardly the
  // slowest thing we do, but it is probably nicer to have a
  // ConstraintSet ADT which efficiently remembers obvious patterns
  // (byte-constant comparison).

  switch (e->getKind()) {
  case Expr::Constant:
    if (!cast<ConstantExpr>(e)->isTrue()) {
      if (canFail)
        return false;
      std::cerr << "Constraints:\n" << _summary << "\n";
      assert(0 && "attempt to add invalid (false) constraint");
    }
    break;

  case Expr::And: {
    // Split to enable finer grained independence and other optimizations
    BinaryExpr *be = cast<BinaryExpr>(e);
    if (!addConstraintWithSimplification(be->left, canFail) ||
        !addConstraintWithSimplification(be->right, canFail))
      return false;
    break;
  }

  case Expr::Eq: {
    // Rewrite any known equalities 
    BinaryExpr *be = cast<BinaryExpr>(e);
    if (isa<ConstantExpr>(be->left)) {
      ExprReplaceVisitor visitor(be->right, be->left);
      if (!rewriteConstraints(visitor, canFail))
        return false;
    }
    pushConstraint(e);
    break;
  }

  default:
    pushConstraint(e);
    break;
  }

  return true;
}

void ConstraintManager::addConstraint(ref<Expr> e) {
  e = simplifyExpr(e);
  addConstraintWithSimplification(e, false);
}

bool ConstraintManager::addConstraintCanFail(ref<Expr> e) {
  e = simplifyExpr(e);
  return addConstraintWithSimplification(e, true);
}

void ConstraintManager::addConstraintNoSimplify(ref<Expr> e) {
  pushConstraint(e);
}

}  // namespace klee
