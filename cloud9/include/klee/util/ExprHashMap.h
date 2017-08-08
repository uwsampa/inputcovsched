//===-- ExprHashMap.h -------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_EXPRHASHMAP_H
#define KLEE_EXPRHASHMAP_H

#include "klee/Expr.h"
#include "klee/Internal/ADT/ImmutableMap.h"
#include "klee/Internal/ADT/ImmutableSet.h"
#include <tr1/unordered_map>
#include <tr1/unordered_set>

namespace klee {

  namespace util {
    struct ExprHash  {
      unsigned operator()(const ref<Expr> e) const {
        return e->hash();
      }
    };

    struct ExprCmp {
      bool operator()(const ref<Expr> &a, const ref<Expr> &b) const {
        return a==b;
      }
    };
  }

  template<class T>
  class ExprHashMap :
    public std::tr1::unordered_map<ref<Expr>,
           T,
           klee::util::ExprHash,
           klee::util::ExprCmp>
  {};

  template<class T>
  class ImmutableExprMap : public ImmutableMap<ref<Expr>, T> {
  public:
    typedef ImmutableMap<ref<Expr>,T> super_type;

    using super_type::operator=;
    using super_type::operator==;
    using super_type::empty;
    using super_type::count;
    using super_type::lookup;
    using super_type::lookup_previous;
    using super_type::min;
    using super_type::max;
    using super_type::size;
    using super_type::insert;
    using super_type::replace;
    using super_type::remove;
    using super_type::popMin;
    using super_type::popMax;
    using super_type::begin;
    using super_type::end;
    using super_type::find;
    using super_type::lower_bound;
    using super_type::upper_bound;
  };

  typedef std::tr1::unordered_set<ref<Expr>,
          klee::util::ExprHash,
          klee::util::ExprCmp> ExprHashSet;

  typedef ImmutableSet< ref<Expr> >  ImmutableExprSet;

}

namespace std {
namespace tr1 {
  // For sets of sets
  template <>
  struct hash<klee::ref<klee::Expr> > {
    size_t operator()(const klee::ref<klee::Expr> &e) const {
      return e->hash();
    }
  };
}
}

#endif
