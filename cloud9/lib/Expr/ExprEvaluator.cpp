//===-- ExprEvaluator.cpp -------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/util/ExprEvaluator.h"

using namespace klee;

ExprVisitor::Action ExprEvaluator::evalRead(const UpdateList &ul,
                                            const unsigned index,
                                            const bool expectIsSymbolicPtr /* = false */) {
  const ref<ConstantExpr> indexExpr = ConstantExpr::create(index, ul.root->getDomain());
  const Array *root = ul.root;

  for (const UpdateNode *un = ul.head; un; un = un->next) {
    ref<Expr> cond = visit(un->getGuardForIndex(indexExpr));

    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(cond)) {
      // if CE->isTrue, we should return the value at this UpdateNode
      // if CE->isFalse, we should check the next UpdateNode
      if (CE->isTrue()) {
        if (un->isMultiByte()) {
          root = un->getSrcArray();
          goto HaveRoot;
        } else {
          assert(un->getByteValue().get());
          return Action::changeTo(visit(un->getByteValue()));
        }
      }
    } else {
      // Update index is unknown, so may or may not be index.
      // We cannot guarantee value.  We can rewrite to a read
      // at this version though (mostly for debugging).
      return Action::changeTo(ReadExpr::create(UpdateList(ul.root, un), indexExpr, expectIsSymbolicPtr));
    }
  }

HaveRoot:
  assert(root);

  if (root->type == Array::Symbolic || hasInitialValue(*root, index)) {
    return Action::changeTo(getInitialValue(*root, index, _inDenominator));
  } else {
    return Action::changeTo(visit(root->getValueAtIndex(indexExpr, expectIsSymbolicPtr)));
  }
}

ExprVisitor::Action ExprEvaluator::visitExpr(const Expr &e) {
  // Evaluate all constant expressions here, in case they weren't folded in
  // construction. Don't do this for reads though, because we want them to go to
  // the normal rewrite path.
  unsigned N = e.getNumKids();
  if (!N || isa<ReadExpr>(e))
    return Action::doChildren();

  for (unsigned i = 0; i != N; ++i)
    if (!isa<ConstantExpr>(e.getKid(i)))
      return Action::doChildren();

  ref<Expr> Kids[3];
  for (unsigned i = 0; i != N; ++i) {
    assert(i < 3);
    Kids[i] = e.getKid(i);
  }

  return Action::changeTo(e.rebuild(Kids));
}

ExprVisitor::Action ExprEvaluator::visitForall(const ForallExpr &re) {
  // TODO: Should we do something better here?
  // i.e., we could instantiate all foralls before evaluating
  // TODO: For now, we'll just assume all foralls are valid
  return Action::changeTo(Expr::True);
}

ExprVisitor::Action ExprEvaluator::visitRead(const ReadExpr &re) {
  ref<Expr> v = visit(re.index);

  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(v)) {
    return evalRead(re.updates, CE->getZExtValue(), re.isSymbolicPointer);
  } else {
    return Action::doChildren();
  }
}

// we need to check for div by zero during partial evaluation,
// if this occurs then simply ignore the 0 divisor and use the
// original expression.
ExprVisitor::Action ExprEvaluator::protectedDivOperation(const BinaryExpr &e) {
  ref<Expr> kids[2] = { visit(e.left),
                        visitDenominator(e.right) };

  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(kids[1]))
    if (CE->isZero())
      kids[1] = e.right;

  if (kids[0]!=e.left || kids[1]!=e.right) {
    return Action::changeTo(e.rebuild(kids));
  } else {
    return Action::skipChildren();
  }
}

ExprVisitor::Action ExprEvaluator::visitUDiv(const UDivExpr &e) {
  return protectedDivOperation(e);
}
ExprVisitor::Action ExprEvaluator::visitSDiv(const SDivExpr &e) {
  return protectedDivOperation(e);
}
ExprVisitor::Action ExprEvaluator::visitURem(const URemExpr &e) {
  return protectedDivOperation(e);
}
ExprVisitor::Action ExprEvaluator::visitSRem(const SRemExpr &e) {
  return protectedDivOperation(e);
}

ref<Expr> ExprEvaluator::visitDenominator(const ref<Expr> &e) {
  const bool old = _inDenominator;
  _inDenominator = true;
  ref<Expr> res = visit(e);
  _inDenominator = old;
  return res;
}
