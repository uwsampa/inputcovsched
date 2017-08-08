/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * Representation of a happens-before graph
 *
 * TODO: store graph with multiple tables (a la program scopes) to minimize copying
 * TODO: handle cross-process edges
 */

#ifndef ICS_HBGRAPH_H
#define ICS_HBGRAPH_H

#include <vector>
#include <tr1/unordered_map>

#include "klee/SymbolicObjectTable.h"
#include "klee/Threading.h"
#include "klee/Internal/ADT/ImmutableMap.h"
#include "klee/Internal/ADT/ImmutableSet.h"
#include "klee/Internal/Module/KInstruction.h"

#include "llvm/ADT/SmallVector.h"

#include "ics/ForkableTrace.h"

namespace klee {
class Executor;
class ExecutionState;
class SymbolicObject;
class Thread;
}

namespace ics {

using klee::hbnode_id_t;

class HBGraph;
class HBGraphRefCounted;
typedef klee::ref<HBGraphRefCounted> HBGraphRef;

//-----------------------------------------------------------------------------
// A single node in a happens-before graph
//-----------------------------------------------------------------------------

class HBNode {
public:
  // Assumption: the common case is one thread-local edge and one cross-thread edge
  typedef llvm::SmallVector<hbnode_id_t, 2> IncomingListTy;

private:
  friend class HBGraph;

  // Starts at 1 and increments for each sync op, per-thread
  unsigned _threadOrder;

  // Location of the sync op
  klee::ThreadUid _tuid;
  const klee::CallingContext *_callctx;

  // For SymbolicInput nodes
  const klee::SymbolicObject *_inputObj;

  // Incoming HB edges
  IncomingListTy _incoming;

public:
  HBNode() : _threadOrder(0), _callctx(NULL) {}

  unsigned threadOrder() const { return _threadOrder; }
  klee::ThreadUid tuid() const { return _tuid; }
  klee::KInstruction* kinst() const { return _callctx->youngest(); }
  const klee::CallingContext* callctx() const { return _callctx; }
  const klee::SymbolicObject* inputObj() const { return _inputObj; }
  const IncomingListTy& incoming() const { return _incoming; }
  bool operator==(const HBNode &other) const;
  bool operator!=(const HBNode &other) const;

  void addEdgeFrom(hbnode_id_t from) { _incoming.push_back(from); }
};

//-----------------------------------------------------------------------------
// A happens-before graph
//-----------------------------------------------------------------------------

class HBGraph {
private:
  typedef klee::ImmutableSet<hbnode_id_t> NodeSetTy;
  typedef klee::ImmutableMap<hbnode_id_t, NodeSetTy> GroupMapTy;
  typedef ForkableTrace<HBNode> NodeListTy;

  // The state this HBGraph is contained in
  // If this is NULL, then this HBGraph is considered immutable
  // and mutator methods will assert-fail when called.
  klee::ExecutionState *_kstate;

  // Info for this HBGraph
  NodeListTy  _nodes;
  GroupMapTy  _groupMap;

  typedef klee::ImmutableMap<klee::ThreadUid, unsigned> ThreadOrderMapTy;
  ThreadOrderMapTy _threadOrders;

  // Info for the HBGraph being replayed
  HBGraphRef  _replayedGraph;
  hbnode_id_t _replayedGraphMaxId;    // replay up to this node id

private:
  // Add a new node
  // Node ids start at 1 and increase monotonically ("0" is an invalid id)
  hbnode_id_t addNode(klee::ThreadUid tuid,
                      const klee::CallingContext *callctx,
                      const klee::SymbolicObject *inputObj,
                      hbnode_id_t lastNode);

  hbnode_id_t addNode(klee::ThreadUid tuid,
                      const klee::CallingContext *callctx,
                      hbnode_id_t lastNode) {
    return addNode(tuid, callctx, NULL, lastNode);
  }

  // Get a mutable node
  // REQUIRES: the HBGraph has not been forked since the node was added
  HBNode* getMutableNode(hbnode_id_t id);

  // Checks that a node matches the replayed graph
  void checkNodeForReplay(const HBNode &node, hbnode_id_t id);

  // Invokes dumpTxt() on "this" and "_replayedGraph"
  void dumpTxtThisAndReplayed(std::ostream &os) const;

private:
  static const klee::CallingContext* GetCallCxtForNode(const klee::Thread &thread);

public:
  explicit HBGraph(klee::ExecutionState *s)
    : _kstate(s), _replayedGraphMaxId(0) {}

  void clear();

  bool empty() const { return _nodes.empty() && _groupMap.empty(); }
  hbnode_id_t getMaxNodeId() const { return _nodes.size(); }
  const HBNode& getNode(hbnode_id_t id) const;
  size_t getThreadCount() const { return _threadOrders.size(); }

  // Compares the node ranges [aFirst, aFirst+nnodes) and [bFirst, bFirst+nnodes)
  static bool equalRange(const HBGraph &a, hbnode_id_t aFirst,
                         const HBGraph &b, hbnode_id_t bFirst, hbnode_id_t nnodes);

  // Compares all nodes for equality
  bool operator==(const HBGraph &other) const;
  bool operator!=(const HBGraph &other) const;

  // Called from ExecutionState::branch()
  void setExecutionState(klee::ExecutionState &kstate) { _kstate = &kstate; }

  // Make an immutable copy of this graph
  // The returned graph is reference counted and will have _kstate == NULL
  HBGraphRef makeImmutableClone();

  //
  // Replaying other HBGraphs
  //

  // If you call startReplay(g, maxid), then we check that each subsequent
  // mutation updates this HBGraph in a way that matches "g".  A divergence
  // from "g" will cause an assertion failure.
  void startReplay(HBGraphRef toReplay, hbnode_id_t maxId);

  // Attempts to replay the next input for "thread".
  // If such a "node" can be found in _replayedHbGraph, then we invoke
  // addNodeForInput(thread, node) and return its value.  Otherwise, we
  // return 0 ("invalid id") to signify that there is nothing to replay.
  hbnode_id_t replayNextInput(klee::Thread *thread);

  //
  // Creates a new HB node for a thread, including a default incoming edge:
  //   addNodeForInput     - default is a thread-local edge
  //   addNodeForThread    - default is a thread-local edge
  //   addNodeForNewThread - default is an edge from the creator
  //

  hbnode_id_t addNodeForInput(klee::Thread *thread, const klee::SymbolicObject *inputObj);
  hbnode_id_t addNodeForThread(klee::Thread *thread);
  hbnode_id_t addNodeForNewThread(klee::Thread *thread, klee::Thread *creator);

  void addEdge(hbnode_id_t from, hbnode_id_t to);
  void addConservativeEdge(hbnode_id_t to);

  //
  // addSourceSinkEdges() creates new HB edges in the following way:
  //   (1) src=null, sink=null :: no edges created
  //   (2) src=A,    sink=null :: let t = new hbnode for the thread
  //                              make edge A->t
  //   (3) src=null, sink=A    :: let t = new hbnode for the thread
  //                              make edge t->A
  //   (4) src=A,    sink=A    :: let t0 = new hbnode for the thread
  //                              let t1 = new hbnode for the thread
  //                              make edge t0->A
  //                              make edge A->t1
  //   (5) src=A,    sink=B    :: let t = new hbnode for the thread
  //                              make edges A->t, t->B
  //
  // The first variant does this for the specified t, and the second
  // variant does this for all t in T.  These are used to implement
  // klee_thread_notify() (see notes at declaration in klee.h).
  //

  void addSourceSinkEdges(klee::ThreadUid tuid,
                          hbnode_id_t source,
                          hbnode_id_t sink);
  void addSourceSinkEdges(const std::set<klee::ThreadUid> &tuids,
                          hbnode_id_t source,
                          hbnode_id_t sink);

  //
  // Node Groups are used to build many <-> one links, much like the
  // source/sink edges above.  The key to _groupMap[] is any number,
  // though usually it is the id of one arbitrarily chosen member of
  // the group.  To build links:
  //    addEdgesFromGroup(G, n) :: forall g in G, make edge g->n
  //    addEdgesToGroup(n, G)   :: forall g in G, make edge n->g
  //

  void addNodeToGroup(hbnode_id_t node, hbnode_id_t group);
  void removeGroup(hbnode_id_t group);

  void addEdgesFromGroup(hbnode_id_t groupFrom, hbnode_id_t nodeTo);
  void addEdgesToGroup(hbnode_id_t nodeFrom, hbnode_id_t groupTo);

  //
  // Debugging
  //
 
  void dumpGraph(std::ostream &os) const;  // as dot file
  void dumpTxt(std::ostream &os) const;    // as txt description

  //
  // Iterate over all nodes
  //

  class node_iterator {
  private:
    NodeListTy::const_iterator _it;

  public:
    node_iterator(NodeListTy::const_iterator it)
      : _it(it) {}

    const HBNode& operator*() { return *_it; }
    const HBNode* operator->() { return &*_it; }

    void operator++() { ++_it; }
    void operator++(int) { ++_it; }
    bool operator==(const node_iterator &rhs) { return _it == rhs._it; }
    bool operator!=(const node_iterator &rhs) { return !(*this == rhs); }
  };

  node_iterator nodesBegin() const { return _nodes.begin(); }
  node_iterator nodesEnd() const { return _nodes.end(); }

  //
  // Iterate over all nodes for a given thread
  //

  class thread_order_iterator {
  private:
    NodeListTy::const_iterator _it;
    NodeListTy::const_iterator _itEnd;
    klee::ThreadUid _tuid;

    void skipToMatchingThread() {
      while (_it != _itEnd && _it->tuid() != _tuid)
        ++_it;
    }

  public:
    thread_order_iterator(NodeListTy::const_iterator it,
                          NodeListTy::const_iterator end,
                          const klee::ThreadUid &tuid,
                          hbnode_id_t startId = 1)
      : _it(it), _itEnd(end), _tuid(tuid)
    {
      for (; startId > 1 && _it != _itEnd; --startId)
        ++_it;
      skipToMatchingThread();
    }

    const HBNode& operator*() { return *_it; }
    const HBNode* operator->() { return &*_it; }

    void operator++() { ++_it; skipToMatchingThread(); }
    void operator++(int) { ++(*this); }

    bool operator==(const thread_order_iterator &rhs) {
      return _it == rhs._it && _tuid == rhs._tuid;
    }
    bool operator!=(const thread_order_iterator &rhs) {
      return !(*this == rhs);
    }
  };

  // Iterate over all nodes
  thread_order_iterator threadOrderBegin(const klee::ThreadUid &tuid) const {
    return thread_order_iterator(_nodes.begin(), _nodes.end(), tuid);
  }
  thread_order_iterator threadOrderEnd(const klee::ThreadUid &tuid) const {
    return thread_order_iterator(_nodes.end(), _nodes.end(), tuid);
  }

  // Iterate over a range of nodes
  thread_order_iterator threadOrderBegin(const klee::ThreadUid &tuid, hbnode_id_t first) const {
    return thread_order_iterator(_nodes.begin(), _nodes.end(), tuid, first);
  }
  thread_order_iterator threadOrderEnd(const klee::ThreadUid &tuid, hbnode_id_t last) const {
    return thread_order_iterator(_nodes.begin(), _nodes.end(), tuid, last+1);
  }
};

//-----------------------------------------------------------------------------
// For reference counting
//-----------------------------------------------------------------------------

class HBGraphRefCounted : public HBGraph {
public:
  explicit HBGraphRefCounted(HBGraph &g) : HBGraph(g), refCount(0) {}

private:
  unsigned refCount;
  friend class klee::ref<HBGraphRefCounted>;

  // Immutable
  HBGraphRefCounted& operator=(const HBGraphRefCounted &);
};

}  // namespace ics

#endif
