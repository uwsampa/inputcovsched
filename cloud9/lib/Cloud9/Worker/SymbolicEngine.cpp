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

#include "cloud9/worker/SymbolicEngine.h"
#include "cloud9/instrum/InstrumentationManager.h"
#include "klee/ExecutionState.h"

#include <boost/bind.hpp>

namespace cloud9 {

namespace worker {

void SymbolicEngine::registerStateEventHandler(StateEventHandler *handler) {
  if (std::find(seHandlers.begin(), seHandlers.end(), handler) == seHandlers.end())
    seHandlers.push_back(handler);
}

void SymbolicEngine::deregisterStateEventHandler(StateEventHandler *handler) {
  handlers_t::iterator it = std::find(seHandlers.begin(), seHandlers.end(), handler);
  if (it != seHandlers.end())
    seHandlers.erase(it);
}

bool SymbolicEngine::fireStateBranching(klee::ExecutionState *state, klee::ForkTag forkTag) {
  if (seHandlers.size() == 0)
    return false;

  int result = true;
  for (handlers_t::iterator it = seHandlers.begin(); it != seHandlers.end(); it++) {
    StateEventHandler *h = *it;
    result = result && h->onStateBranching(state, forkTag);
  }

  return result;
}

void SymbolicEngine::fireStateBranched(klee::ExecutionState *state,
                                       klee::ExecutionState *parent,
                                       int index, klee::ForkTag forkTag) {

  fireHandler(boost::bind(&StateEventHandler::onStateBranched, _1,
                          state, parent, index, forkTag));

  if (state) {
    cloud9::instrum::theInstrManager.incStatistic(cloud9::instrum::TotalForkedStates);
    cloud9::instrum::theInstrManager.incStatistic(cloud9::instrum::CurrentStateCount);
  }
}

void SymbolicEngine::fireUpdateSearcherAfterBranch(const std::vector<klee::ExecutionState*> &states) {
  fireHandler(boost::bind(&StateEventHandler::onUpdateSearcherAfterBranch, _1, states));
}

void SymbolicEngine::fireStateDestroy(klee::ExecutionState *state, bool silenced) {
  fireHandler(boost::bind(&StateEventHandler::onStateDestroy, _1, state, silenced));

  cloud9::instrum::theInstrManager.incStatistic(cloud9::instrum::TotalFinishedStates);
  cloud9::instrum::theInstrManager.decStatistic(cloud9::instrum::CurrentStateCount);
}

void SymbolicEngine::fireControlFlowEventHandler(klee::ExecutionState *state,
                                                 ControlFlowEvent event,
                                                 klee::ThreadUid targetTuid,
                                                 klee::KInstruction *targetKinst,
                                                 llvm::Function *f) {
  fireHandler(boost::bind(&StateEventHandler::onControlFlowEvent, _1,
                          state, event, targetTuid, targetKinst, f));
}

void SymbolicEngine::fireControlFlowEvent(klee::ExecutionState *state,
                                          ControlFlowEvent event) {
  fireControlFlowEventHandler(state, event, state->crtThreadUid(), state->pc(), NULL);
}

void SymbolicEngine::fireExternalCallEvent(klee::ExecutionState *state,
                                           ControlFlowEvent event,
                                           llvm::Function *f) {
  fireControlFlowEventHandler(state, event, state->crtThreadUid(), state->pc(), f);
}

void SymbolicEngine::fireContextSwitchEvent(klee::ExecutionState *state,
                                            klee::ThreadUid newTuid,
                                            klee::KInstruction *newKinst) {
  fireControlFlowEventHandler(state, CONTEXT_SWITCH, newTuid, newKinst, NULL);
}

void SymbolicEngine::fireDebugInfo(klee::ExecutionState *state, const std::string &message) {
  fireHandler(boost::bind(&StateEventHandler::onDebugInfo, _1, state, message));
}

void SymbolicEngine::fireOutOfResources(klee::ExecutionState *destroyedState) {
  fireHandler(boost::bind(&StateEventHandler::onOutOfResources, _1, destroyedState));
}

void SymbolicEngine::fireEvent(klee::ExecutionState *state,
                               klee::EventClass type,
                               uint64_t value) {
  fireHandler(boost::bind(&StateEventHandler::onEvent, _1, state, type, value));
}

void SymbolicEngine::fireMemoryAccess(klee::ExecutionState *state,
                                      const klee::MemoryObject *mo,
                                      klee::ref<klee::Expr> offset,
                                      klee::ref<klee::Expr> nbytes,
                                      bool isStore,
                                      bool isAlloc) {
  fireHandler(boost::bind(&StateEventHandler::onMemoryAccess, _1,
                          state, mo, offset, nbytes, isStore, isAlloc));
}

void SymbolicEngine::fireMemoryAccess(klee::ExecutionState *state,
                                      const klee::MemoryObject *mo,
                                      klee::ref<klee::Expr> offset,
                                      klee::Expr::Width width,
                                      bool isStore,
                                      bool isAlloc) {
  fireMemoryAccess(state, mo, offset,
                   klee::Expr::createPointer(klee::Expr::getMinBytesForWidth(width)),
                   isStore, isAlloc);
}

void SymbolicEngine::fireConstraintAdded(klee::ExecutionState *state,
                                         klee::ref<klee::Expr> condition,
                                         klee::ForkClass reason) {
  fireHandler(boost::bind(&StateEventHandler::onConstraintAdded, _1,
                          state, condition, reason));
}

void SymbolicEngine::fireIcsBegin(klee::ExecutionState *state) {
  fireHandler(boost::bind(&StateEventHandler::onIcsBegin, _1, state));
}

void SymbolicEngine::fireIcsMaybeEndRegion(klee::ExecutionState *state,
                                           klee::ThreadUid tuid,
                                           bool force) {
  fireHandler(boost::bind(&StateEventHandler::onIcsMaybeEndRegion, _1, state, tuid, force));
}

void SymbolicEngine::fireResolvedSymbolicPtr(klee::ExecutionState *state,
                                             klee::ref<klee::SymbolicPtrResolution> res) {
  fireHandler(boost::bind(&StateEventHandler::onResolvedSymbolicPtr, _1, state, res));
}


}

}
