/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * A rope-like data structure for storing an append-only trace that can be "forked"
 * into multiple versions, forming a tree, where a single version of the trace is
 * represented by a single path through the tree.  The segments of the trace (i.e.,
 * the nodes of the tree) are reference counted.
 *
 * To fork, use the copy constructor.
 */

#ifndef ICS_FORKABLETRACE_H
#define ICS_FORKABLETRACE_H

#include "klee/util/Ref.h"
#include <iterator>
#include <vector>
#include <sstream>

namespace ics {

//-----------------------------------------------------------------------------
// ForkableTrace Declaration
//-----------------------------------------------------------------------------

template<class T>
class ForkableTrace {
public:
  //
  // A single node in the list
  // refCount is used by klee::ref<>
  //
  struct Node {
    unsigned        refCount;
    std::vector<T>  data;

    Node() : refCount(0) {}
  };

public:
  //
  // A bidirectional iterator over a ForkableTrace
  // Has the same lifetime guarantees as std::vector::iterator
  //
  template<class TraceT, class NodeIter, class ElemIter, class ElemT>
  class Iterator : public std::iterator<std::bidirectional_iterator_tag, ElemT> {
  private:
    TraceT   *_trace;
    NodeIter  _nodeIt;
    ElemIter  _elemIt;

    typedef Iterator<TraceT,NodeIter,ElemIter,ElemT> this_type;
    friend class ForkableTrace<T>;

  private:
    static this_type makeFromBegin(TraceT *trace);
    static this_type makeFromEnd(TraceT *trace);

    void skipForwardOverEmptyNodes();
    void skipBackwardOverEmptyNodes();

  public:
    Iterator()
      : _trace(NULL)
    {}
    Iterator(const this_type &rhs)
      : _trace(rhs._trace), _nodeIt(rhs._nodeIt), _elemIt(rhs._elemIt)
    {}

    this_type& operator=(const this_type &rhs) {
      _trace = rhs._trace;
      _nodeIt = rhs._nodeIt;
      _elemIt = rhs._elemIt;
      return *this;
    }

    bool operator==(const this_type &rhs) const {
      return _trace == rhs._trace &&
             _nodeIt == rhs._nodeIt &&
             _elemIt == rhs._elemIt;
    }
    bool operator!=(const this_type &rhs) const {
      return !(*this == rhs);
    }

    ElemT& operator*() const;
    ElemT* operator->() const;
    this_type& operator++();     // preincrement
    this_type& operator--();     // preincrement
    this_type  operator++(int);  // postincrement
    this_type  operator--(int);  // postincrement
  };

  friend class Iterator<ForkableTrace<T>,
                        typename std::vector<klee::ref<Node> >::iterator,
                        typename std::vector<T>::iterator,
                        T>;

  friend class Iterator<const ForkableTrace<T>,
                        typename std::vector<klee::ref<Node> >::const_iterator,
                        typename std::vector<T>::const_iterator,
                        T>;

public:
  //
  // ForkableTrace types
  //
  typedef size_t    size_type;
  typedef ptrdiff_t difference_type;
  typedef T         value_type;
  typedef T&        reference;
  typedef const T&  const_reference;
  typedef T*        pointer;
  typedef const T*  const_pointer;

  typedef Iterator<ForkableTrace<T>,
                   typename std::vector<klee::ref<Node> >::iterator,
                   typename std::vector<T>::iterator,
                   T>
          iterator;

  typedef Iterator<const ForkableTrace<T>,
                   typename std::vector<klee::ref<Node> >::const_iterator,
                   typename std::vector<T>::const_iterator,
                   const T>
          const_iterator;

  typedef std::reverse_iterator<iterator>        reverse_iterator;
  typedef std::reverse_iterator<const_iterator>  const_reverse_iterator;

private:
  //
  // Members of ForkableTrace
  //
  mutable std::vector<klee::ref<Node> > _nodes;
  size_t _size;

  // not defined
  ForkableTrace& operator=(const ForkableTrace<T> &rhs);

public:
  ForkableTrace();
  ForkableTrace(const ForkableTrace<T> &rhs);

  size_t size() const { return _size; }
  bool empty() const { return _size == 0; }

  iterator begin()             { return iterator::makeFromBegin(this); }
  iterator end()               { return iterator::makeFromEnd(this); }
  const_iterator begin() const { return const_iterator::makeFromBegin(this); }
  const_iterator end() const   { return const_iterator::makeFromEnd(this); }

  reverse_iterator rbegin()             { return reverse_iterator(end()); }
  reverse_iterator rend()               { return reverse_iterator(begin()); }
  const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
  const_reverse_iterator rend() const   { return const_reverse_iterator(begin()); }

  reference front()              { return *begin(); }
  const_reference front() const  { return *begin(); }

  reference back()              { return *rbegin(); }
  const_reference back() const  { return *rbegin(); }

  void push_back(const T& x)    { _nodes.back()->data.push_back(x); ++_size; }
  void clear();
  void swap(ForkableTrace<T> &rhs);

  // Expensive operation: collapses the trace into a single node
  void reverse();

  // Returns a non-mutable reference to the element at the specified index.
  // This is O(F) where F is the number of times the trace has been forked.
  // REQUIRES: idx < size()
  const T& getElementAt(size_t idx) const;

  // Returns a mutable ptr to the element at the specified index.
  // Returns NULL iff we do NOT have a unique copy of the specified element.
  // This is O(F) where F is the number of times the trace has been forked.
  // REQUIRES: idx < size()
  T* getMutableElementPtr(size_t idx);

  // DEBUGGING
  void collectStats(size_t *nodes, std::string *nodeSizes,
                    size_t *maxElemPerNode, size_t *minElemPerNode) const;
};

//-----------------------------------------------------------------------------
// ForkableTrace::Iterator Definition
//-----------------------------------------------------------------------------

template<class T>
template<class TraceT, class NodeIter, class ElemIter, class ElemT>
ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>
ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>::makeFromBegin(TraceT *trace) {
  assert(trace && "constructing from null trace");
  this_type it;
  it._trace = trace;
  it._nodeIt = trace->_nodes.begin();
  it.skipForwardOverEmptyNodes();
  return it;
}

template<class T>
template<class TraceT, class NodeIter, class ElemIter, class ElemT>
ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>
ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>::makeFromEnd(TraceT *trace) {
  assert(trace && "constructing from null trace");
  this_type it;
  it._trace = trace;
  it._nodeIt = trace->_nodes.end();
  it._elemIt = ElemIter();
  return it;
}

template<class T>
template<class TraceT, class NodeIter, class ElemIter, class ElemT>
void ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>::skipForwardOverEmptyNodes() {
  while (_nodeIt != _trace->_nodes.end() && (*_nodeIt)->data.empty())
    ++_nodeIt;

  if (_nodeIt == _trace->_nodes.end())
    _elemIt = ElemIter();
  else
    _elemIt = (*_nodeIt)->data.begin();
}

template<class T>
template<class TraceT, class NodeIter, class ElemIter, class ElemT>
void ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>::skipBackwardOverEmptyNodes() {
  assert(_nodeIt != _trace->_nodes.end());
  while (_nodeIt != _trace->_nodes.begin() && (*_nodeIt)->data.empty())
    --_nodeIt;

  if ((*_nodeIt)->data.empty())
    // oops, we skipped past _trace->begin()
    skipForwardOverEmptyNodes();
  else
    // set to back of the node
    _elemIt = (*_nodeIt)->data.end() - 1;
}

// dereference

template<class T>
template<class TraceT, class NodeIter, class ElemIter, class ElemT>
ElemT& ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>::operator*() const {
  assert(_trace && "dereferencing null iterator");
  return *_elemIt;
}

template<class T>
template<class TraceT, class NodeIter, class ElemIter, class ElemT>
ElemT* ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>::operator->() const {
  assert(_trace && "dereferencing null iterator");
  return &*_elemIt;
}

// preincrement

template<class T>
template<class TraceT, class NodeIter, class ElemIter, class ElemT>
ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>&
ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>::operator++() {
  assert(_trace && "incrementing null iterator");
  assert(_nodeIt != _trace->_nodes.end() && "incrementing end iterator");

  if (_elemIt != (*_nodeIt)->data.end()) {
    ++_elemIt;
  }

  if (_elemIt == (*_nodeIt)->data.end()) {
    ++_nodeIt;
    skipForwardOverEmptyNodes();
  }

  return *this;
}

template<class T>
template<class TraceT, class NodeIter, class ElemIter, class ElemT>
ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>&
ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>::operator--() {
  assert(_trace && "decrementing null iterator");

  if (_nodeIt == _trace->_nodes.end() || _elemIt == ((*_nodeIt)->data.begin())) {
    assert(_nodeIt != _trace->_nodes.begin() && "decrementing begin iterator");
    --_nodeIt;
    skipBackwardOverEmptyNodes();
  } else {
    --_elemIt;
  }

  return *this;
}

// postincrement

template<class T>
template<class TraceT, class NodeIter, class ElemIter, class ElemT>
ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>
ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>::operator++(int) {
  this_type old = *this;
  ++(*this);
  return old;
}

template<class T>
template<class TraceT, class NodeIter, class ElemIter, class ElemT>
ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>
ForkableTrace<T>::Iterator<TraceT,NodeIter,ElemIter,ElemT>::operator--(int) {
  this_type old = *this;
  --(*this);
  return old;
}

//-----------------------------------------------------------------------------
// ForkableTrace Definition
// Invariant: !_nodes.empty() && !_nodes[x].isNull() for valid x
//-----------------------------------------------------------------------------

template<class T>
ForkableTrace<T>::ForkableTrace() {
  clear();
}

template<class T>
ForkableTrace<T>::ForkableTrace(const ForkableTrace<T> &rhs) {
  // If the current tail is non-empty, push an empty node on the tail
  if (!rhs._nodes.back()->data.empty()) {
    rhs._nodes.push_back(new Node);
  }

  // Dup rhs into this, but replace the tail with a new empty node
  _size = rhs._size;
  _nodes = rhs._nodes;
  _nodes.back() = new Node;
}

template<class T>
void ForkableTrace<T>::clear() {
  _nodes.clear();
  _nodes.push_back(new Node);
  _size = 0;
}

template<class T>
void ForkableTrace<T>::swap(ForkableTrace<T> &rhs) {
  std::swap(_size, rhs._size);
  std::swap(_nodes, rhs._nodes);
}

template<class T>
void ForkableTrace<T>::reverse() {
  klee::ref<Node> n(new Node);
  n->data.reserve(_size);

  // Iterate in reverse, copy reverse ranges
  for (typename std::vector<klee::ref<Node> >::reverse_iterator
       it = _nodes.rbegin(); it != _nodes.rend(); ++it) {
    n->data.insert(n->data.end(), (*it)->data.rbegin(), (*it)->data.rend());
  }

  _nodes.clear();
  _nodes.push_back(n);
  assert(n->data.size() == _size);
}

template<class T>
const T& ForkableTrace<T>::getElementAt(size_t idx) const {
  assert(idx < size());

  for (size_t k = 0; ; ++k) {
    Node *node = _nodes[k].get();
    size_t sz = node->data.size();
    if (idx < sz)
      return node->data[idx];
    idx -= sz;
  }
}

template<class T>
T* ForkableTrace<T>::getMutableElementPtr(size_t idx) {
  assert(idx < size());

  // Careful: don't take a local ref<> to the nodes
  // Doing so would artificially make a node appear shared
  for (size_t k = 0; ; ++k) {
    Node *node = _nodes[k].get();
    size_t sz = node->data.size();
    if (idx < sz)
      return (node->refCount == 1) ? &node->data[idx] : NULL;
    idx -= sz;
  }
}

template<class T>
void ForkableTrace<T>::collectStats(size_t *nodes, std::string *nodeSizes,
                                    size_t *maxElemPerNode, size_t *minElemPerNode) const {
  std::ostringstream os;
  *nodes = _nodes.size();
  *maxElemPerNode = 0;
  *minElemPerNode = size_t(-1);

  for (typename std::vector<klee::ref<Node> >::const_iterator
       it = _nodes.begin(); it != _nodes.end(); ++it) {
    const Node *n = it->get();

    if (n->data.size() > *maxElemPerNode) *maxElemPerNode = n->data.size();
    if (n->data.size() < *minElemPerNode) *minElemPerNode = n->data.size();

    os << n->data.size() << " ";
  }

  *nodeSizes = os.str();
}

//-----------------------------------------------------------------------------
// ForkableTrace Operators
//-----------------------------------------------------------------------------

template<class T>
bool operator==(const ForkableTrace<T> &lhs, const ForkableTrace<T> &rhs) {
  if (lhs.size() != rhs.size())
    return false;
  return std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

template<class T>
bool operator!=(const ForkableTrace<T> &lhs, const ForkableTrace<T> &rhs) {
  return !(lhs == rhs);
}


}  // namespace ics

#endif
