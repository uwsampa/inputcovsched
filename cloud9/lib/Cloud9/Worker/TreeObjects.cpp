/*
 * Cloud9 Parallel Symbolic Execution Engine
 *
 * Copyright (c) 2011, Dependable Systems Laboratory, EPFL
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Dependable Systems Laboratory, EPFL nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE DEPENDABLE SYSTEMS LABORATORY, EPFL BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * All contributors are listed in CLOUD9-AUTHORS file.
 *
*/

#include "cloud9/worker/TreeObjects.h"
#include "cloud9/worker/TreeNodeInfo.h"

#include <stack>

namespace cloud9 {

namespace worker {

std::ostream &operator<< (std::ostream &os, const SymbolicState &state) {
	os << *(state.getKleeState());

	return os;
}

void WorkerNodeInfo::enumerateChildStates(WorkerTree::Node *node,
                                          std::vector<SymbolicState*> *children) {
  assert(node);
  assert(children);

  // Enumerate all direct children
  WorkerTree::Node* members[2];
  members[0] = node->getLeft(WORKER_LAYER_STATES);
  members[1] = node->getRight(WORKER_LAYER_STATES);

  for (size_t i = 0; i < 2; ++i) {
    node = members[i];
    if (!node)
      continue;
    SymbolicState *state = (**members[i]).getSymbolicState();
    if (state)
      children->push_back(state);
  }
}

SymbolicState* WorkerNodeInfo::getOtherChildState(WorkerTree::Node *node,
                                                  SymbolicState *state) {
  assert(node);
  assert(state);

  // Assuming "state" is a direct child of "node", return the other child, if any
  WorkerTree::Node *left = node->getLeft(WORKER_LAYER_STATES);
  WorkerTree::Node *right = node->getRight(WORKER_LAYER_STATES);

  if (left && (**left).getSymbolicState() != state)
    return (**left).getSymbolicState();

  if (right && (**right).getSymbolicState() != state)
    return (**right).getSymbolicState();

  return NULL;
}

}
}

