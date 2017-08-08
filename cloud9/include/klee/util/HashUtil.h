//===-- HashUtil.h ----------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_HASHUTIL_H
#define KLEE_HASHUTIL_H

#include <tr1/unordered_map>
#include <tr1/unordered_set>
#include <set>
#include <utility>

namespace std {
namespace tr1 {
  // Dumb dumb dumb that this isn't part of the standard
  template <>
  template <class T, class U>
  struct hash<std::pair<T,U> > {
    size_t operator()(const std::pair<T,U> &p) const {
      return hash<T>()(p.first) ^ hash<U>()(p.second);
    }
  };

  // For sets of sets
  template <>
  template <class T>
  struct hash<std::set<T> > {
    size_t operator()(const std::set<T> &s) const {
      size_t h = 0;
      for (typename std::set<T>::const_iterator
           it = s.begin(), itEnd = s.end(); it != itEnd; ++it)
        h ^= hash<T>()(*it);
      return h;
    }
  };
}
}

#endif
