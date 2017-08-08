//===-- ExprVisitor.h -------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_EXPRVISITOR_H
#define KLEE_EXPRVISITOR_H

#include "ExprHashMap.h"

namespace klee {
  class ExprVisitor {
  protected:
    // typed variant, but non-virtual for efficiency
    class Action {
    public:
      enum Kind { SkipChildren, DoChildren, ChangeTo };

    private:
      //      Action() {}
      Action(Kind _kind)
        : kind(_kind), argument(ConstantExpr::alloc(0, Expr::Bool)) {}
      Action(Kind _kind, const ref<Expr> &_argument)
        : kind(_kind), argument(_argument) {}

      friend class ExprVisitor;

    public:
      Kind kind;
      ref<Expr> argument;

      static Action changeTo(const ref<Expr> &expr) {
        return Action(ChangeTo,expr);
      }
      static Action doChildren() { return Action(DoChildren); }
      static Action skipChildren() { return Action(SkipChildren); }
    };

  protected:
    explicit
    ExprVisitor(bool _recursive=false) : recursive(_recursive) {}
    virtual ~ExprVisitor() {}

    virtual Action visitExpr(const Expr&);
    virtual Action visitExprPost(const Expr&);

    virtual Action visitNotOptimized(const NotOptimizedExpr&);
    virtual Action visitRead(const ReadExpr&);
    virtual Action visitSelect(const SelectExpr&);
    virtual Action visitConcat(const ConcatExpr&);
    virtual Action visitExtract(const ExtractExpr&);
    virtual Action visitZExt(const ZExtExpr&);
    virtual Action visitSExt(const SExtExpr&);
    virtual Action visitAdd(const AddExpr&);
    virtual Action visitSub(const SubExpr&);
    virtual Action visitMul(const MulExpr&);
    virtual Action visitUDiv(const UDivExpr&);
    virtual Action visitSDiv(const SDivExpr&);
    virtual Action visitURem(const URemExpr&);
    virtual Action visitSRem(const SRemExpr&);
    virtual Action visitNot(const NotExpr&);
    virtual Action visitAnd(const AndExpr&);
    virtual Action visitOr(const OrExpr&);
    virtual Action visitXor(const XorExpr&);
    virtual Action visitShl(const ShlExpr&);
    virtual Action visitLShr(const LShrExpr&);
    virtual Action visitAShr(const AShrExpr&);
    virtual Action visitEq(const EqExpr&);
    virtual Action visitNe(const NeExpr&);
    virtual Action visitUlt(const UltExpr&);
    virtual Action visitUle(const UleExpr&);
    virtual Action visitUgt(const UgtExpr&);
    virtual Action visitUge(const UgeExpr&);
    virtual Action visitSlt(const SltExpr&);
    virtual Action visitSle(const SleExpr&);
    virtual Action visitSgt(const SgtExpr&);
    virtual Action visitSge(const SgeExpr&);
    virtual Action visitImplies(const ImpliesExpr&);
    virtual Action visitQVar(const QVarExpr&);
    virtual Action visitForall(const ForallExpr&);

  private:
    typedef ExprHashMap< ref<Expr> > visited_ty;
    visited_ty visited;
    bool recursive;

    ref<Expr> visitActual(const ref<Expr> &e);

  public:
    // use to enable re-visiting expressions across multiple
    // calls to visit()
    void clearVisited() { visited.clear(); }

    // apply the visitor to the expression and return a possibly
    // modified new expression.
    ref<Expr> visit(const ref<Expr> &e);

    // shorthand
    ref<Expr> visitFresh(const ref<Expr> &e) {
      clearVisited();
      return visit(e);
    }
  };

  class ExprReplaceVisitor : public ExprVisitor {
  private:
    ref<Expr> src, dst;
    const bool visitPost;

  public:
    //
    // Use visitPost=false when the following two conditions hold:
    //  (a) the replacement (dst) may contain the original expression (src), and
    //  (b) it might be true that src!=dst in the current context
    //
    // Examples:
    //   o To replace (a+b) with 0, use visitPost=true to enable replacements
    //     like (a+(b+(a+b))) => (a+b) => 0.  In contrast, with visitPost=false,
    //     the replacements would take the first step, (a+(b+(a+b))) => (a+b),
    //     but not continue further because that final (a+b) was discovered
    //     during the post-traversal.
    //
    //   o To replace (a+b) with ((a+b)+c), use visitPost=false to prevent
    //     double replacement, i.e., to prevent (a+b) => ((a+b)+c) => (((a+b)+c)+c)
    //
    ExprReplaceVisitor(ref<Expr> _src, ref<Expr> _dst, bool _visitPost = true)
      : src(_src), dst(_dst), visitPost(_visitPost) {}

    Action visitExpr(const Expr &e);
    Action visitExprPost(const Expr &e);
  };

  class ExprReplaceAllVisitor : public ExprVisitor {
  public:
    typedef ExprHashMap< ref<Expr> > MapTy;
    MapTy replacements;

  public:
    ExprReplaceAllVisitor() {}
    Action visitExpr(const Expr &e);
  };

  class ExprSearchVisitor : public ExprVisitor {
  private:
    const ref<Expr> target;
    bool found;
    Action visitExpr(const Expr &e);

  public:
    ExprSearchVisitor(ref<Expr> _target) : target(_target) {}

    // Returns true iff target \in src
    bool findIn(const ref<Expr> &src);
  };

}

#endif
