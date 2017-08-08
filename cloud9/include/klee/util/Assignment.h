//===-- Assignment.h --------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_UTIL_ASSIGNMENT_H
#define KLEE_UTIL_ASSIGNMENT_H

#include <map>
#include <iostream>
#include <algorithm>

#include "klee/Internal/ADT/ImmutableMap.h"
#include "klee/util/ExprEvaluator.h"

namespace klee {
class Array;

//-----------------------------------------------------------------------------
// This describes a sparse assignment of constant values to bytes in Arrays.
// Unconstrained array indices can have any value.  Internally, we use
// ImmutableMaps so that copying Assignments is basically free.
//-----------------------------------------------------------------------------

class Assignment {
private:
  class Binding {
  public:
    typedef ImmutableMap<unsigned, unsigned char> SparseMapTy;
    SparseMapTy _sparse;

  public:
    void add(unsigned index, unsigned char value) {
      _sparse = _sparse.replace(SparseMapTy::value_type(index, value));
    }

    unsigned char lookup(unsigned index, bool *found = NULL) const {
      if (const SparseMapTy::value_type *it = _sparse.lookup(index)) {
        if (found) *found = true;
        return it->second;
      } else {
        if (found) *found = false;
        return 0;
      }
    }

    unsigned int minIndex() const {
      assert(!empty());
      return _sparse.min().first;
    }

    unsigned int maxIndex() const {
      assert(!empty());
      return _sparse.max().first;
    }

    bool empty() const {
      return _sparse.empty();
    }

    bool operator<(const Binding &other) const {
      return _sparse < other._sparse;
    }
  };

  // All known bindings for this assignment
  // Unbound (array,index) pairs are evaluated to (ConstantExpr 0)
  typedef ImmutableMap<const Array*, Binding> BindingsTy;
  BindingsTy _bindings;

public:
  Assignment() {}

  void addBinding(const Array *array, unsigned index, unsigned char value);
  bool hasBinding(const Array *mo, unsigned index) const;
  void clear();

  ref<Expr> evaluate(const Array *mo, unsigned index, bool defaultToZero = true) const;
  ref<Expr> evaluate(ref<Expr> e) const;

  ref<Expr> buildConstraints() const;

  typedef std::vector<std::pair<std::string, std::vector<unsigned char> > > SolutionListTy;
  void buildSolutionList(SolutionListTy *out) const;

  template<typename InputIterator>
  bool satisfies(InputIterator begin, InputIterator end) const;

  void dump(std::ostream &out, const char *prefix = "") const;

  bool operator<(const Assignment &other) const { return _bindings < other._bindings; }
};

//-----------------------------------------------------------------------------
// Evaluates an Expr via a given Assignment
//-----------------------------------------------------------------------------

class AssignmentEvaluator : public ExprEvaluator {
private:
  const Assignment &a;

protected:
  ref<Expr> getInitialValue(const Array &array, unsigned index, bool inDenom) {
    return a.evaluate(&array, index, !inDenom);
  }

  bool hasInitialValue(const Array &array, unsigned index) {
    return a.hasBinding(&array, index);
  }

public:
  explicit AssignmentEvaluator(const Assignment &_a) : a(_a) {}
};

//-----------------------------------------------------------------------------
// Assignment Inline Impl
//-----------------------------------------------------------------------------

template<typename InputIterator>
inline bool Assignment::satisfies(InputIterator begin, InputIterator end) const {
  AssignmentEvaluator v(*this);
  for (; begin!=end; ++begin) {
    if (!v.visit(*begin)->isTrue())
      return false;
  }
  return true;
}

}  // namespace klee

#endif
