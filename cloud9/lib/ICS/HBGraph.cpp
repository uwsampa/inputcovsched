/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * Representation of a happens-before graph
 *
 */

#include "ics/HBGraph.h"
#include "ics/SearchStrategy.h"

#include "klee/Executor.h"
#include "klee/ExecutionState.h"
#include "klee/Internal/Module/InstructionInfoTable.h"

#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>

using klee::ThreadUid;
using klee::Thread;
using klee::CallingContext;
using cloud9::worker::JobManager;

using namespace llvm;

namespace {
cl::opt<bool> AlwaysReplayInputs("ics-always-replay-inputs-via-hb-graphs", cl::init(true));
cl::opt<bool> DbgHbGraphs("ics-debug-hb-graphs", cl::init(false));
}

namespace ics {

//-----------------------------------------------------------------------------
// HBNode
//-----------------------------------------------------------------------------

bool HBNode::operator==(const HBNode &other) const {
  return _threadOrder == other._threadOrder
      && _tuid == other._tuid
      && _callctx == other._callctx
      && _inputObj == other._inputObj
      && _incoming == other._incoming;
}

bool HBNode::operator!=(const HBNode &other) const {
  return !(*this == other);
}

//-----------------------------------------------------------------------------
// HBGraph
//-----------------------------------------------------------------------------

void HBGraph::clear() {
  assert(_kstate);

  _nodes.clear();
  _groupMap = GroupMapTy();
  _threadOrders = ThreadOrderMapTy();
  _replayedGraph = NULL;

  // Reset lastHbNode for each thread
  for (klee::ExecutionState::threads_ty::iterator
       t = _kstate->threads.begin(); t != _kstate->threads.end(); ++t) {
    t->second.lastHbNode = 0;
  }
}

HBGraphRef HBGraph::makeImmutableClone() {
  HBGraphRef tmp(new HBGraphRefCounted(*this));
  tmp->_kstate = NULL;  // make the returned HBGraph immutable
  tmp->_replayedGraph = NULL;
  return tmp;
}

bool HBGraph::equalRange(const HBGraph &aGraph, hbnode_id_t aFirst,
                         const HBGraph &bGraph, hbnode_id_t bFirst, hbnode_id_t nnodes) {
  if (aFirst + nnodes - 1 > aGraph.getMaxNodeId() ||
      bFirst + nnodes - 1 > bGraph.getMaxNodeId())
    return false;

  for (hbnode_id_t k = 0; k < nnodes; ++k)
    if (aGraph.getNode(aFirst+k) != bGraph.getNode(bFirst+k))
      return false;

  return true;
}

bool HBGraph::operator==(const HBGraph &other) const {
  if (_nodes.size() != other._nodes.size())
    return false;

  for (hbnode_id_t k = 1; k <= getMaxNodeId(); ++k)
    if (getNode(k) != other.getNode(k))
      return false;

  return true;
}

bool HBGraph::operator!=(const HBGraph &other) const {
  return !(*this == other);
}

//-----------------------------------------------------------------------------
// Replaying graphs
// TODO: replay inputs
//-----------------------------------------------------------------------------

void HBGraph::startReplay(HBGraphRef toReplay, hbnode_id_t maxId) {
  assert(_replayedGraph.isNull());
  _replayedGraph = toReplay;
  _replayedGraphMaxId = maxId;
}

void HBGraph::checkNodeForReplay(const HBNode &curr, hbnode_id_t id) {
  if (!AlwaysReplayInputs)
    return;

  if (_replayedGraph.isNull() || id > _replayedGraphMaxId)
    return;

  // Compare with the replayed graph
  const HBNode &old = _replayedGraph->getNode(id);
  if (curr.threadOrder() != old.threadOrder() ||
      curr.tuid() != old.tuid() ||
      curr.callctx() != old.callctx() ||
      curr.incoming().size() > old.incoming().size()) {
    goto bad;
  }
  for (size_t k = 0; k < curr.incoming().size(); ++k) {
    if (curr.incoming()[k] != old.incoming()[k])
      goto bad;
  }

  // OK
  return;

bad:
  std::cerr << "ERROR: HBGraph replay failed for node " << id
            << " (maxId:" << _replayedGraphMaxId << ")\n";
  dumpTxtThisAndReplayed(std::cerr);
  assert(0);
}

hbnode_id_t HBGraph::replayNextInput(klee::Thread *thread) {
  const hbnode_id_t nextId = _nodes.size() + 1;
  if (_replayedGraph.isNull() || nextId > _replayedGraphMaxId)
    return 0;

  const HBNode &old = _replayedGraph->getNode(nextId);
  if (!old.inputObj()) {
    if (!AlwaysReplayInputs)
      return 0;
    std::cerr << "ERROR: HBGraph replay failed for node " << nextId << ": INPUT EXPECTED!\n";
    dumpTxtThisAndReplayed(std::cerr);
    assert(0);
  }

  const hbnode_id_t id = addNodeForInput(thread, old.inputObj());
  assert(id == nextId);
  return id;
}

//-----------------------------------------------------------------------------
// Retrieving nodes
//-----------------------------------------------------------------------------

// N.B.: HBNode ids are 1-based, so to index into _nodes[], we used id-1.

// N.B.: getMutableNode() succeeds only if the HBNode has just 1 reference in
// the trace, i.e., if the HBGraph has not been forked since the node was added.
// This should always be the case because we always know the full set of incoming
// edges for a node when it is created, so we should never need to fork between
// creating a node and adding an incoming edge to that node.  (Note, however,
// that it is often convenient to enumerate those edges via multiple klee calls,
// so we don't enforce this requirement via the HBGraph API.)

// TODO FIXME: The above is actually not the case for barriers, where the first
// N-1 threads to arrive at the barrier create nodes Ni, then when the final
// thread arrives, it creates new nodes A and B and adds edges A->Ni->B for each
// Ni (where the node id for A will be less-than those for each Ni)

HBNode* HBGraph::getMutableNode(hbnode_id_t id) {
  assert(_kstate);
  assert(id);
  assert(id-1 < _nodes.size());
  HBNode *node = _nodes.getMutableElementPtr(id - 1);
  if (!node) {
    llvm::errs() << "ERROR: getMutableNode(" << id << ") failed!\n"
                 << "  callctx:\n"
                 << klee::CallingContextPrinter(getNode(id).callctx(), "    ");
    assert(0);
  }
  return node;
}

const HBNode& HBGraph::getNode(hbnode_id_t id) const {
  assert(id);
  assert(id-1 < _nodes.size());
  return _nodes.getElementAt(id - 1);
}

//-----------------------------------------------------------------------------
// Adding nodes
//-----------------------------------------------------------------------------

hbnode_id_t HBGraph::addNode(ThreadUid tuid,
                             const klee::CallingContext *callctx,
                             const klee::SymbolicObject *inputObj,
                             hbnode_id_t lastNode) {
  assert(_kstate);
  assert(!callctx->empty());

  // Skip ignorable nodes
  const llvm::Function *f = callctx->youngest()->inst->getParent()->getParent();
  if (IsIgnorableSyncOpCaller(f)) {
    if (DbgHbGraphs) {
      klee::KInstruction *kinst = callctx->youngest();
      llvm::errs() << "HBGraph.addNode tuid=" << tuid << " SKIPPING\n"
                   << "   DST: " << kinst->info->file << ":" << kinst->info->line << "\n"
                   << "   DST: " << (*kinst->inst) << "\n";
    }
    return lastNode;
  }

  // Strip uClibc wrappers from the callctx
  // We don't do this in GetCallCxtForNode as that would prevent the above check.
  callctx = callctx->removeUclibcWrapperFunctions();
  assert(!callctx->empty());

  // Build the node
  const hbnode_id_t id = _nodes.size() + 1;
  HBNode node;
  node._tuid = tuid;
  node._callctx = callctx;
  node._inputObj = inputObj;

  if (lastNode) {
    node.addEdgeFrom(lastNode);
  }

  // Thread-ordering id
  const ThreadOrderMapTy::value_type *currOrder = _threadOrders.lookup(tuid);
  if (currOrder) {
    node._threadOrder = currOrder->second + 1;
  } else {
    node._threadOrder = 1;
  }
  _threadOrders = _threadOrders.replace(std::make_pair(tuid, node._threadOrder));

  // Done
  _nodes.push_back(node);
  checkNodeForReplay(node, id);

  if (DbgHbGraphs) {
    klee::KInstruction *kinst = node.kinst();
    llvm::errs() << "HBGraph.addNode tuid=" << tuid << "\n"
                 << "   EDGE: " << lastNode << " -> " << id << "\n"
                 << "   DST: " << kinst->info->file << ":" << kinst->info->line << "\n"
                 << "   DST: " << (*kinst->inst) << "\n";
  }

  return id;
}

const CallingContext* HBGraph::GetCallCxtForNode(const Thread &thread) {
  // Start with the complete ctx, then remove all stack frames that are in "pthreads" calls.
  const CallingContext *callctx = CallingContext::FromThreadStackWithPC(thread);
  callctx = callctx->removePthreadsFunctions();

  // Should NOT be empty
  assert(!callctx->empty());
  return callctx;
}

hbnode_id_t HBGraph::addNodeForInput(Thread *thread, const klee::SymbolicObject *inputObj) {
  assert(thread);
  assert(inputObj);
  thread->lastHbNode = addNode(thread->tuid,
                               GetCallCxtForNode(*thread),
                               inputObj,
                               thread->lastHbNode);
  return thread->lastHbNode;
}

hbnode_id_t HBGraph::addNodeForThread(Thread *thread) {
  assert(thread);
  thread->lastHbNode = addNode(thread->tuid, GetCallCxtForNode(*thread), thread->lastHbNode);
  return thread->lastHbNode;
}

hbnode_id_t HBGraph::addNodeForNewThread(Thread *thread, Thread *creator) {
  assert(thread);
  if (creator) {
    creator->lastHbNode = addNode(creator->tuid, GetCallCxtForNode(*creator), creator->lastHbNode);
    thread->lastHbNode = addNode(thread->tuid, GetCallCxtForNode(*thread), creator->lastHbNode);
  } else {
    thread->lastHbNode = addNode(thread->tuid, GetCallCxtForNode(*thread), 0);
  }
  return thread->lastHbNode;
}

//-----------------------------------------------------------------------------
// Adding edges
//-----------------------------------------------------------------------------

void HBGraph::addEdge(hbnode_id_t from, hbnode_id_t to) {
  if (!from || !to || from == to)
    return;

  HBNode *toNode = getMutableNode(to);

  // Skip duplicate edges
  for (size_t k = 0; k < toNode->incoming().size(); ++k) {
    if (toNode->incoming()[k] == from)
      return;
  }

  // Not a duplicate
  toNode->addEdgeFrom(from);
  checkNodeForReplay(*toNode, to);

  if (DbgHbGraphs) {
    llvm::errs() << "HBGraph.addEdge\n"
                 << "   EDGE: " << from << " -> " << to << "\n";
  }
}

void HBGraph::addConservativeEdge(hbnode_id_t to) {
  if (!to)
    return;

  HBNode *toNode = getMutableNode(to);

  // FIXME: shouldn't have to walk back over the whole graph
  std::set<klee::ThreadUid> visited;
  visited.insert(toNode->tuid());

  // Make a link from all other threads
  for (hbnode_id_t from = getMaxNodeId(); from > 0; --from) {
    if (visited.insert(getNode(from).tuid()).second) {
      toNode->addEdgeFrom(from);
      if (DbgHbGraphs)
        llvm::errs() << "HBGraph.addConservativeEdge\n"
                     << "   EDGE: " << from << " -> " << to << "\n";
    }
  }

  checkNodeForReplay(*toNode, to);
}

void HBGraph::addSourceSinkEdges(ThreadUid tuid, hbnode_id_t source, hbnode_id_t sink) {
  // See header file for description
  assert(_kstate);
  assert(_kstate->threads.count(tuid));
  Thread *thread = &(_kstate->threads.find(tuid)->second);

  // (1) Nop
  if (!source && !sink)
    return;

  // (2) A->t
  if (source && !sink) {
    addEdge(source, addNodeForThread(thread));
    return;
  }

  // (3) t->A
  if (!source && sink) {
    addEdge(addNodeForThread(thread), sink);
    return;
  }

  // (4) t0->A->t1
  if (source && sink && source == sink) {
    hbnode_id_t t0 = addNodeForThread(thread);
    hbnode_id_t t1 = addNodeForThread(thread);
    addEdge(t0, source);
    addEdge(source, t1);
    return;
  }

  // (5) A->t, t->B
  if (source && sink && source != sink) {
    hbnode_id_t t = addNodeForThread(thread);
    addEdge(source, t);
    addEdge(t, sink);
    return;
  }

  assert(0);
}

void HBGraph::addSourceSinkEdges(const std::set<ThreadUid> &tuids,
                                 hbnode_id_t source,
                                 hbnode_id_t sink) {
  // Nop?
  if (!source && !sink)
    return;

  // One thread at at time
  for (std::set<ThreadUid>::const_iterator
       it = tuids.begin(); it != tuids.end(); ++it) {
    addSourceSinkEdges(*it, source, sink);
  }
}

//-----------------------------------------------------------------------------
// Adding groups and edges
//-----------------------------------------------------------------------------

void HBGraph::addNodeToGroup(hbnode_id_t node, hbnode_id_t group) {
  const GroupMapTy::value_type *g = _groupMap.lookup(group);
  NodeSetTy nodes;
  if (g != NULL) {
    nodes = g->second.insert(node);
  } else {
    nodes = NodeSetTy().insert(node);
  }
  _groupMap = _groupMap.replace(std::make_pair(group, nodes));
}

void HBGraph::removeGroup(hbnode_id_t group) {
  _groupMap = _groupMap.remove(group);
}

void HBGraph::addEdgesFromGroup(hbnode_id_t groupFrom, hbnode_id_t nodeTo) {
  assert(_kstate);
  const GroupMapTy::value_type *g = _groupMap.lookup(groupFrom);
  if (g != NULL) {
    for (NodeSetTy::iterator n = g->second.begin(); n != g->second.end(); ++n)
      addEdge(*n, nodeTo);
  }
}

void HBGraph::addEdgesToGroup(hbnode_id_t nodeFrom, hbnode_id_t groupTo) {
  assert(_kstate);
  const GroupMapTy::value_type *g = _groupMap.lookup(groupTo);
  if (g != NULL) {
    for (NodeSetTy::iterator n = g->second.begin(); n != g->second.end(); ++n)
      addEdge(nodeFrom, *n);
  }
}

//-----------------------------------------------------------------------------
// Debug
//-----------------------------------------------------------------------------

void HBGraph::dumpGraph(std::ostream &os) const {
  os << "digraph G {\n";

  // Dump edges
  size_t id = 1;
  for (NodeListTy::const_iterator n = _nodes.begin(); n != _nodes.end(); ++n, ++id) {
    const HBNode& node = *n;

    for (HBNode::IncomingListTy::const_iterator
         i = node.incoming().begin(); i != node.incoming().end(); ++i) {
      os << "  n" << (*i) << " -> n" << id << ";\n";
    }
  }

  // Dump node labels
  id = 1;
  for (NodeListTy::const_iterator n = _nodes.begin(); n != _nodes.end(); ++n, ++id) {
    const HBNode& node = *n;
    const std::string& file = node.kinst()->info->file;

    os << "  n" << id << " [label=\""
       << file.substr(file.find_last_of('/') + 1) << ":" << node.kinst()->info->line
       << "\\n" << node.tuid();
    if (node.inputObj())
       os << "\\nINPUT";
    os << "\"];\n";
  }

  os << "}" << std::endl;
}

void HBGraph::dumpTxt(std::ostream &os) const {
  // Dump nodes with predecessor lists
  size_t id = 1;
  for (NodeListTy::const_iterator n = _nodes.begin(); n != _nodes.end(); ++n, ++id) {
    const HBNode& node = *n;
    os << "Node id:" << id << " order:" << node.threadOrder() << " tuid:" << node.tuid();
    if (node.inputObj()) {
       os << " input:" << node.inputObj()->array->name << " (" << node.inputObj() << ")\n";
    } else {
       os << " noinput\n";
    }

    os << "  preds: [";
    for (size_t k = 0; k < node.incoming().size(); ++k) {
      if (k != 0) os << ", ";
      os << node.incoming()[k];
    }
    os << "]\n";
    os << "  callctx\n";
    os << klee::CallingContextPrinter(node.callctx(), "    ");
  }
}

void HBGraph::dumpTxtThisAndReplayed(std::ostream &os) const {
  std::cerr << "==== This graph ====\n";
  dumpTxt(os);
  std::cerr << "==== Replayed graph ====\n";
  if (_replayedGraph.get()) {
    _replayedGraph->dumpTxt(os);
  } else {
    os << "<null>\n";
  }
}

}  // namespace ics
