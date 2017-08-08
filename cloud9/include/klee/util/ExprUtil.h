//===-- ExprUtil.h ----------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_EXPRUTIL_H
#define KLEE_EXPRUTIL_H

#include "klee/Expr.h"
#include "klee/Context.h"

#include <map>
#include <iterator>
#include <vector>

namespace klee {

/// Find all ReadExprs used in the expression DAG. If visitUpdates
/// is true then this will including those reachable by traversing
/// update lists. Note that this may be slow and return a large
/// number of results.
void findReads(ref<Expr> e,
               bool visitUpdates,
               Expr::CachedReadsTy &results);

/// Return a list of all unique symbolic objects referenced by the given
/// expression, where "OutputSet" is any type supported by the results
/// type for findSymbolicObjects().
template<typename OutputSet>
void findSymbolicObjects(ref<Expr> e,
                         OutputSet &results);

/// Return a list of all unique symbolic objects referenced by the
/// given expression range.  These are instantiated for set<ref<Expr>>
/// and vector<ref<Expr>> input iterators.
template<typename InputIterator>
void findSymbolicObjects(InputIterator begin,
                         InputIterator end,
                         std::set<const Array*> &results);

template<typename InputIterator>
void findSymbolicObjects(InputIterator begin,
                         InputIterator end,
                         std::vector<const Array*> &results);

/// Returns true if the given expression uses and of the given symbols
bool usesSymbolicObjects(ref<Expr> e,
                         const std::set<const Array*> &used);


/// Iterates over all terms in an n-ary expression of the given kind.
/// For example, given the expression tree (a & (b & (c & d))), we
/// iterates over the sequence a,b,c,d.  If the root term is not of the
/// given kind, we iterate over a sequence of length 1.  As stated in
/// Expr.h, we assume expressions are unbalanced to the right.
/// REQUIRES: "kind" is a BinaryExpr or a Concat
/// TODO: Does this work for any BinaryExprs except for AndExpr, OrExpr, and ConcatExpr?
/// (These are the kinds for which create() enforces "unbalanced to the right")
class ExprNaryIterator : public std::iterator<std::input_iterator_tag, ref<Expr> > {
private:
  ref<Expr> _pos;
  const Expr::Kind _kind;

private:
  void next() {
    assert(!done());
    if (_pos->getKind() == _kind) {
      _pos = _pos->getKid(1);
    } else {
      _pos = NULL;
    }
  }

public:
  ExprNaryIterator(ref<Expr> root, Expr::Kind kind)
    : _pos(root), _kind(kind)
  {}
  ExprNaryIterator()
    : _pos(NULL), _kind(Expr::InvalidKind)
  {}

  bool done() const {
    return _pos.isNull();
  }

  bool operator==(const ExprNaryIterator &rhs) const {
    if (done() != rhs.done())
      return false;
    if (done())
      return true;
    return _kind == rhs._kind && _pos == rhs._pos;
  }
  bool operator!=(const ExprNaryIterator &rhs) const {
    return !(*this == rhs);
  }

  ref<Expr> operator*() const {
    if (_pos->getKind() == _kind) {
      return _pos->getKid(0);
    } else {
      return _pos;
    }
  }

  Expr* get() const {
    if (_pos->getKind() == _kind) {
      return _pos->getKid(0).get();
    } else {
      return _pos.get();
    }
  }

  ExprNaryIterator& operator++() {
    // preincrement
    next();
    return *this;
  }
  ExprNaryIterator operator++(int) {
    // postincrement
    ExprNaryIterator tmp = *this;
    next();
    return tmp;
  }
};

// Sort a collection of ref<ReadExpr> so that (easily) provably adjacent
// ReadExprs are adjacent in the returned list.  ReadExprs that are not
// (easily) provable adjacent are ordered arbitrarily.  For example, given
//   [Read(a,3), Read(b,3), Read(a,2), Read(b,1)]
//
// This can return
//   [Read(a,2), Read(a,3), Read(b,3), Read(b,1)]
//
// Big endian: MSB first
// Little endian: LSB first

struct AdjacentReadExprCmp {
  // Comparison function used by SortReadExprsAdjacent
  bool operator()(const ref<ReadExpr> &a, const ref<ReadExpr> &b) {
    assert(a->updates == b->updates);
    ref<ConstantExpr> diff;
    if (klee::Context::get().isLittleEndian()) {
      diff = dyn_cast<ConstantExpr>(SubExpr::create(a->index, b->index));
    } else {
      diff = dyn_cast<ConstantExpr>(SubExpr::create(b->index, a->index));
    }
    assert(diff.get());
    return diff->getAPValue().isNegative();
  }
};

template<class InputTy>
std::vector< ref<ReadExpr> > SortReadExprsAdjacent(const InputTy &exprs) {
  typedef typename std::vector< std::vector< ref<ReadExpr> > > BucketTy;
  typedef typename std::map<UpdateList, BucketTy> BucketsMapTy;
  BucketsMapTy buckets;

  // Partition into sortable buckets
  // Two level partition: first by UpdateLists, then by groups that are comparable
  for (typename InputTy::const_iterator it = exprs.begin(); it != exprs.end(); ++it) {
    BucketTy &b = buckets[(*it)->updates];
    size_t k;
    for (k = 0; k < b.size(); ++k) {
      assert(!b[k].empty());
      if (isa<ConstantExpr>(SubExpr::create(b[k][0]->index, (*it)->index)))
        break;
    }
    if (k == b.size()) {
      b.push_back(typename BucketTy::value_type());
    }
    b[k].push_back(*it);
  }

  // Sort each bucket and serialize output
  std::vector< ref<ReadExpr> > output;
  for (typename BucketsMapTy::iterator b = buckets.begin(); b != buckets.end(); ++b) {
    for (size_t k = 0; k < b->second.size(); ++k) {
      std::sort(b->second[k].begin(), b->second[k].end(), AdjacentReadExprCmp());
      for (size_t i = 0; i < b->second[k].size(); ++i)
        output.push_back(b->second[k][i]);
    }
  }

  return output;
}

}  // namespace klee

#endif
