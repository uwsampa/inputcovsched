//===-- ImmutableList.h -----------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef __UTIL_IMMUTABLELIST_H__
#define __UTIL_IMMUTABLELIST_H__

#include <klee/util/Ref.h>
#include <iterator>

namespace klee {
  template<class T>
  class ImmutableList {
  public:
    typedef T value_type;

    template<class ElemT>
    class Iterator : public std::iterator<std::forward_iterator_tag, ElemT> {
    private:
      ref<typename ImmutableList<ElemT>::Node> _curr;

      void advance() {
        assert(!_curr.isNull());
        _curr = _curr->next;
      }

      typedef Iterator<ElemT> this_type;

    public:
      Iterator() {}
      Iterator(const ImmutableList &b) : _curr(b._head) {}

      this_type& operator=(const this_type &rhs) { _curr = rhs._curr; return *this; }
      bool operator==(const this_type &rhs) { return _curr.get() == rhs._curr.get(); }
      bool operator!=(const this_type &rhs) { return !(*this == rhs); }

      ElemT& operator*() const { return *_curr; };
      ElemT* operator->() const { return _curr.get(); };
      // preincrement
      this_type& operator++() const { advance(); return *this; }
      // postincrement
      this_type  operator++(int) const { this_type k(*this); k.advance(); return *this; }
    };

    typedef Iterator<T> iterator;
    friend class Iterator<T>;

  private:
    struct Node {
      T data;
      ref<Node> next;
      unsigned refCount;

      Node() : refCount(0) {}
      Node(const T &d, ref<Node> n) : data(d), next(n), refCount(0) {}
    };

    ref<Node> _head;
    explicit ImmutableList(ref<Node> h) : _head(h) {}

  public:
    ImmutableList() {}
    ImmutableList(const ImmutableList &b) : _head(b._head) {}
    ~ImmutableList() {}

    ImmutableList &operator=(const ImmutableList &b) { _head = b._head; return *this; }

    bool empty() const {
      return _head.isNull();
    }
    size_t size() const {
      size_t k = 0;
      for (ref<Node> n = _head; n.get(); n = n->next)
        ++k;
      return k;
    }

    ImmutableList push_front(const T &value) const {
      return ImmutableList(new Node(value, _head));
    }
    ImmutableList pop_front() const {
      assert(!empty());
      return ImmutableList(_head->next);
    }

    T& front() const {
      assert(!empty());
      return _head->data;
    }

    iterator begin() const {
      return iterator(*this);
    }
    iterator end() const {
      return iterator();
    }

    bool operator==(const ImmutableList &rhs) const { return _head == rhs._head; }
    bool operator!=(const ImmutableList &rhs) const { return _head != rhs._head; }
  };

}

#endif
