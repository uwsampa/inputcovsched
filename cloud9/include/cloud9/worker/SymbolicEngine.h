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

#ifndef SYMBOLICENGINE_H_
#define SYMBOLICENGINE_H_

#include "klee/ForkTag.h"
#include "klee/Expr.h"
#include "klee/Threading.h"

#include <set>
#include <string>

namespace klee {
class ExecutionState;
class Searcher;
class KModule;
class Interpreter;
class MemoryObject;
class SymbolicPtrResolution;
}

namespace llvm {
class Function;
class Value;
}

namespace cloud9 {

namespace worker {

enum ControlFlowEvent {
	STEP,
	BRANCH,
	CALL,
	RETURN,
  UNWIND,
  CONTEXT_SWITCH
};

class StateEventHandler {
public:
	StateEventHandler() {};
	virtual ~StateEventHandler() {};

public:
	virtual bool onStateBranching(klee::ExecutionState *state, klee::ForkTag forkTag) = 0;
	virtual void onStateBranched(klee::ExecutionState *state,
			klee::ExecutionState *parent, int index, klee::ForkTag forkTag) = 0;
	virtual void onUpdateSearcherAfterBranch(const std::vector<klee::ExecutionState*> &states) = 0;
	virtual void onStateDestroy(klee::ExecutionState *state, bool silenced) = 0;
	virtual void onControlFlowEvent(klee::ExecutionState *state, ControlFlowEvent event,
      klee::ThreadUid targetTuid, klee::KInstruction *targetKinst,
      llvm::Function *externalCall) = 0;
	virtual void onDebugInfo(klee::ExecutionState *state, const std::string &message) = 0;
	virtual void onEvent(klee::ExecutionState *state, klee::EventClass type, uint64_t value) = 0;
	virtual void onOutOfResources(klee::ExecutionState *destroyedState) = 0;
	virtual void onMemoryAccess(klee::ExecutionState *state, const klee::MemoryObject *mo,
      klee::ref<klee::Expr> offset, klee::ref<klee::Expr> nbytes, bool isStore, bool isAlloc) = 0;
	virtual void onConstraintAdded(klee::ExecutionState *state,
      klee::ref<klee::Expr> condition, klee::ForkClass reason) = 0;
  virtual void onIcsBegin(klee::ExecutionState *state) = 0;
  virtual void onIcsMaybeEndRegion(klee::ExecutionState *state, klee::ThreadUid tuid,
      bool force) = 0;
  virtual void onResolvedSymbolicPtr(klee::ExecutionState *kstate,
      klee::ref<klee::SymbolicPtrResolution> res) = 0;
};

class SymbolicEngine {
private:
	typedef std::vector<StateEventHandler*> handlers_t;
	handlers_t seHandlers;

	template <class Handler>
	void fireHandler(Handler handler) {
	  for (handlers_t::iterator it = seHandlers.begin(); it != seHandlers.end(); it++) {
      StateEventHandler *h = *it;
      handler(h);
    }
	}

	void fireControlFlowEventHandler(klee::ExecutionState *state, ControlFlowEvent event,
      klee::ThreadUid targetTuid, klee::KInstruction *targetKinst,
      llvm::Function *externalCall);

public:
	bool fireStateBranching(klee::ExecutionState *state, klee::ForkTag forkTag);
	void fireStateBranched(klee::ExecutionState *state, klee::ExecutionState *parent,
      int index, klee::ForkTag forkTag);
  void fireUpdateSearcherAfterBranch(const std::vector<klee::ExecutionState*> &states);
	void fireStateDestroy(klee::ExecutionState *state, bool silenced);
	void fireDebugInfo(klee::ExecutionState *state, const std::string &message);
	void fireOutOfResources(klee::ExecutionState *destroyedState);
	void fireEvent(klee::ExecutionState *state, klee::EventClass type, uint64_t value);
	void fireMemoryAccess(klee::ExecutionState *state, const klee::MemoryObject *mo,
      klee::ref<klee::Expr> offset, klee::ref<klee::Expr> nbytes, bool isStore, bool isAlloc);
	void fireMemoryAccess(klee::ExecutionState *state, const klee::MemoryObject *mo,
      klee::ref<klee::Expr> offset, klee::Expr::Width width, bool isStore, bool isAlloc);
	void fireConstraintAdded(klee::ExecutionState *state, klee::ref<klee::Expr> condition,
      klee::ForkClass reason);
	void fireIcsBegin(klee::ExecutionState *state);
	void fireIcsMaybeEndRegion(klee::ExecutionState *state, klee::ThreadUid tuid, bool force);
  void fireResolvedSymbolicPtr(klee::ExecutionState *kstate,
      klee::ref<klee::SymbolicPtrResolution> res);

	// All of these trigger onControlFlowEvent()

	// state->prevPC() is the control flow instruction
	// state->pc() is the target instruction
	void fireControlFlowEvent(klee::ExecutionState *state, ControlFlowEvent event);

	// state->prevPC() is the control flow instruction
	// f is the callee
  // event is CALL or RETURN
	void fireExternalCallEvent(klee::ExecutionState *state, ControlFlowEvent event, llvm::Function *f);

	// state->prevPC() is the last instruction executed in the old thread
	// target{Tuid,Kinst} is the first instruction to execute in the new thread
	void fireContextSwitchEvent(klee::ExecutionState *state, klee::ThreadUid newTuid,
      klee::KInstruction *newKinst);

public:
	SymbolicEngine() {};
	virtual ~SymbolicEngine() {};

	virtual klee::ExecutionState *createRootState(llvm::Function *f) = 0;
	virtual void initRootState(klee::ExecutionState *state, int argc,
			char **argv, char **envp) = 0;

	virtual void stepInState(klee::ExecutionState *state) = 0;

	virtual void destroyState(klee::ExecutionState *state) = 0;

	virtual void destroyStates() = 0;

	virtual klee::Searcher *initSearcher(klee::Searcher *base) = 0;

	virtual klee::KModule *getModule() = 0;

	void registerStateEventHandler(StateEventHandler *handler);
	void deregisterStateEventHandler(StateEventHandler *handler);

  static bool classof(const klee::Interpreter* interpreter){ return true; }
};

}

}

#endif /* SYMBOLICENGINE_H_ */
