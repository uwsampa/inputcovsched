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

#ifndef THREADING_H_
#define THREADING_H_

#include "klee/Expr.h"
#include "klee/Internal/Module/KInstIterator.h"
#include "../../lib/Core/AddressSpace.h"
#include "cloud9/Logger.h"

#include <map>

namespace ics {
class HBGraph;
class DepthFirstICS;
}

namespace klee {

class KFunction;
class KInstruction;
class ExecutionState;
class Process;
class CallPathNode;
struct Cell;

typedef uint32_t thread_id_t;
typedef uint32_t process_id_t;
typedef uint32_t hbnode_id_t;
typedef uint64_t wlist_id_t;
typedef uint64_t wlist_syncop_t;

struct ThreadUid {
  thread_id_t tid;
  process_id_t pid;

  ThreadUid() : tid(0), pid(0) {}
  ThreadUid(thread_id_t t, process_id_t p) : tid(t), pid(p) {}
};

struct WaitingListKey {
  ref<Expr> id;    // value of the wlist id, or NULL for "not a waiting list"
  ref<Expr> addr;  // address of the wlist id
  wlist_syncop_t syncop;  // type of syncop being waited on
  ref<Expr> wlistSlot;  // for initial waiters, the symbolic variable that represents
                        // the thread's order in the waitlist (NULL if not an initial waiter)

  // Initialize to everything to null
  WaitingListKey() : syncop(0) {}
};


struct StackFrame {
  KInstIterator caller;
  KFunction *kf;
  CallPathNode *callPathNode;

  std::vector<const MemoryObject*> allocas;
  Cell *locals;

  /// Minimum distance to an uncovered instruction once the function
  /// returns. This is not a good place for this but is used to
  /// quickly compute the context sensitive minimum distance to an
  /// uncovered instruction. This value is updated by the StatsTracker
  /// periodically.
  unsigned minDistToUncoveredOnReturn;

  // For vararg functions: arguments not passed via parameter are
  // stored (packed tightly) in a local (alloca) memory object. This
  // is setup to match the way the front-end generates vaarg code (it
  // does not pass vaarg through as expected). VACopy is lowered inside
  // of intrinsic lowering.
  MemoryObject *varargs;

  StackFrame(KInstIterator caller, KFunction *kf);
  StackFrame(const StackFrame &s);

  StackFrame& operator=(const StackFrame &sf);
  ~StackFrame();
};


class Thread {
  friend class Executor;
  friend class ExecutionState;
  friend class Process;
  friend class SpecialFunctionHandler;
  friend class ::ics::HBGraph;
  friend class ::ics::DepthFirstICS;
private:

  KInstIterator pc, prevPC;
  unsigned incomingBBIndex;

  std::vector<StackFrame> stack;

  bool enabled;
  ThreadUid tuid;
  hbnode_id_t lastHbNode;
  WaitingListKey currWaitingList;

public:
  Thread(thread_id_t tid, process_id_t pid, KFunction *start_function);

  bool isEnabled() const { return enabled; }
  bool isOnWaitingList() const { return currWaitingList.id.get() != NULL; }

  ThreadUid getTuid() const { return tuid; }
  thread_id_t getTid() const { return tuid.tid; }
  process_id_t getPid() const { return tuid.pid; }

  KInstruction* getPC() const { return pc; }
  KInstruction* getPrevPC() const { return prevPC; }
  void advancePC() { prevPC = pc; ++pc; }

  std::vector<StackFrame>& getStack() { return stack; }
  const std::vector<StackFrame>& getStack() const { return stack; }

  // Should only be called to set up an initial program context for a
  // thread blocked in klee_thread_sleep (e.g., during partial eval)
  void updateWaitingListAtSleep(std::vector<ref<Expr> > &arguments, ref<Expr> wlistSlot) {
    assert(arguments.size() == 3);  // see args for klee_thread_sleep()
    assert(isa<klee::ConstantExpr>(arguments[2]));
    assert(currWaitingList.syncop == cast<klee::ConstantExpr>(arguments[2])->getZExtValue());
    currWaitingList.id = arguments[0];
    currWaitingList.addr = arguments[1];
    currWaitingList.wlistSlot = wlistSlot;
  }
};

}

// For convenience
#include "klee/util/ThreadingUtil.h"

#endif /* THREADING_H_ */
