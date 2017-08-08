//===-- ExprEvaluator.h -----------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_EXPREVALUATOR_H
#define KLEE_EXPREVALUATOR_H

#include "klee/Expr.h"
#include "klee/util/ExprVisitor.h"

namespace klee {
  class ExprEvaluator : public ExprVisitor {
  protected:
    Action evalRead(const UpdateList &ul, const unsigned index, const bool expectIsSymbolicPtr);
    Action visitRead(const ReadExpr &re);
    Action visitForall(const ForallExpr &re);
    Action visitExpr(const Expr &e);

    Action protectedDivOperation(const BinaryExpr &e);
    Action visitUDiv(const UDivExpr &e);
    Action visitSDiv(const SDivExpr &e);
    Action visitURem(const URemExpr &e);
    Action visitSRem(const SRemExpr &e);

  private:
    bool _inDenominator;
    ref<Expr> visitDenominator(const ref<Expr> &e);

  public:
    ExprEvaluator() : _inDenominator(false) {}

    /// getInitialValue - Return the initial value for a symbolic byte.
    ///
    /// This will be called for constant arrays only if the index is
    /// out-of-bounds.  If !hasInitialValue(i), meaning the value is
    /// unknown, then this may return ANYTHING.
    virtual ref<Expr> getInitialValue(const Array& array, unsigned index, bool inDenom) = 0;

    /// hastInitialValue - Return true if we have a known initial value
    /// for a symbolic byte.  If !hasInitialValue(i), then getInitialValue(i)
    /// may return anything.
    virtual bool hasInitialValue(const Array& array, unsigned index) = 0;
  };
}

#endif
