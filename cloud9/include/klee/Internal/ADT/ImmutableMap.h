//===-- ImmutableMap.h ------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef __UTIL_IMMUTABLEMAP_H__
#define __UTIL_IMMUTABLEMAP_H__

#include <functional>

#include "ImmutableTree.h"

namespace klee {
  template<class V, class D>
  struct _Select1st {
    D &operator()(V &a) const { return a.first; }
    const D &operator()(const V &a) const { return a.first; }
  };

  template<class K, class D, class CMP=std::less<K> >
  class ImmutableMap {
  public:
    typedef K key_type;
    typedef std::pair<K,D> value_type;

    typedef ImmutableTree<K, value_type, _Select1st<value_type,key_type>, CMP> Tree;
    typedef typename Tree::iterator iterator;

  private:
    Tree elts;

    ImmutableMap(const Tree &b): elts(b) {}

  public:
    ImmutableMap() {}
    ImmutableMap(const ImmutableMap &b) : elts(b.elts) {}
    ~ImmutableMap() {}

    ImmutableMap &operator=(const ImmutableMap &b) { elts = b.elts; return *this; }

    bool empty() const {
      return elts.empty();
    }
    size_t count(const key_type &key) const {
      return elts.count(key);
    }
    const value_type *lookup(const key_type &key) const {
      return elts.lookup(key);
    }
    const value_type *lookup_previous(const key_type &key) const {
      return elts.lookup_previous(key);
    }
    const value_type &min() const {
      return elts.min();
    }
    const value_type &max() const {
      return elts.max();
    }
    size_t size() const {
      return elts.size();
    }

    ImmutableMap insert(const K &key, const D &data) const {
      return insert(value_type(key, data));
    }
    ImmutableMap insertNullData(const K &key) const {
      return insert(value_type(key, NULL));  // works only when D is a ptr type
    }
    ImmutableMap replace(const K &key, const D &data) const {
      return replace(value_type(key, data));
    }

    ImmutableMap insert(const value_type &value) const {
      return elts.insert(value);
    }
    ImmutableMap replace(const value_type &value) const {
      return elts.replace(value);
    }
    ImmutableMap remove(const key_type &key) const {
      return elts.remove(key);
    }
    ImmutableMap popMin(value_type &valueOut) const {
      return elts.popMin(valueOut);
    }
    ImmutableMap popMax(value_type &valueOut) const {
      return elts.popMax(valueOut);
    }

    template<class Predicate>
    ImmutableMap remove_if(const Predicate &pred) const {
      return elts.remove_if(pred);
    }

    template<class MergeFunc>
    ImmutableMap merge_with(const ImmutableMap &other, const MergeFunc &mergefn) const {
      return elts.merge_with(other.elts, mergefn);
    }

    template<class MapFunc>
    ImmutableMap map_values(const MapFunc &mapfn) const {
      return elts.map_values(mapfn);
    }

    iterator begin() const {
      return elts.begin();
    }
    iterator end() const {
      return elts.end();
    }
    iterator find(const key_type &key) const {
      return elts.find(key);
    }
    iterator lower_bound(const key_type &key) const {
      return elts.lower_bound(key);
    }
    iterator upper_bound(const key_type &key) const {
      return elts.upper_bound(key);
    }

    bool operator==(const ImmutableMap &rhs) const { return elts == rhs.elts; }
    bool operator!=(const ImmutableMap &rhs) const { return elts != rhs.elts; }
    bool operator< (const ImmutableMap &rhs) const { return elts <  rhs.elts; }

    static size_t getAllocated() { return Tree::allocated; }
  };

}

#endif
