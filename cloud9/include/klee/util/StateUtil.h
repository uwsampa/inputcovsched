//===-- StateUtil.h ---------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_STATEUTIL_H
#define KLEE_STATEUTIL_H

#include "klee/ExecutionState.h"

namespace klee {

// There are times we need to fake a thread stack to appear
// as if it's in a specific frame.  This adjusts stack to match
// the specified, then restores the stack to its state before
// the adjustment.  Restoring happens explicitly or on destruction.
class StackSaver {
private:
  typedef ExecutionState::stack_ty StackTy;
  StackTy &_stack;
  StackTy  _savedStack;
  bool _restored;

public:
  StackSaver(StackTy &stack, int frameno)
    : _stack(stack), _restored(false) {
    // Save
    StackTy::iterator saveStart = _stack.begin() + frameno + 1;
    StackTy::iterator saveEnd   = _stack.end();

    _savedStack.assign(saveStart, saveEnd);
    _stack.erase(saveStart, saveEnd);
  }

  ~StackSaver() {
    if (!_restored)
      restore();
  }

  void restore() {
    assert(!_restored);
    _stack.insert(_stack.end(), _savedStack.begin(), _savedStack.end());
    _restored = true;
  }

  StackTy& getSaved() {
    return _savedStack;
  }

  size_t getFullSize() const {
    return _stack.size() + _savedStack.size();
  }
};

// Like above, but saves/restores ExecutionState::crtThreadIt.
class CrtThreadSaver {
private:
  ExecutionState &_state;
  ExecutionState::threads_ty::iterator _old;

public:
  CrtThreadSaver(ExecutionState &state, Thread &next)
    : _state(state) {
    _old = _state.crtThreadIt;
    _state.crtThreadIt = _state.threads.find(next.getTuid());
    // Doesn't support switching across processes
    assert(_old->first.pid == _state.crtThreadIt->first.pid);
    assert(_state.crtThreadIt != _state.threads.end());
  }

  ~CrtThreadSaver() {
    _state.crtThreadIt = _old;
  }
};

// Like above, but saves/restores ExecutionState::pc/prevPC
class CrtPCSaver {
private:
  ExecutionState &_state;
  const KInstIterator _oldPC;
  const KInstIterator _oldPrevPC;
  bool _restored;

public:
  explicit CrtPCSaver(ExecutionState &state)
    : _state(state),
      _oldPC(state.pc()),
      _oldPrevPC(state.prevPC()),
      _restored(false)
  {}

  ~CrtPCSaver() {
    if (!_restored)
      restore();
  }

  void restore() {
    assert(!_restored);
    _state.pc() = _oldPC;
    _state.prevPC() = _oldPrevPC;
    _restored = true;
  }
};

}  // namespace klee

#endif
