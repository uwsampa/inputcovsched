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

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "multiprocess.h"

FORCE_LINKAGE(threads)

////////////////////////////////////////////////////////////////////////////////
// Symbolic vs concrete threads ids usage
//
// klee_get_symbolic_thread_context()
//   * use for pthread_self
//
// klee_get_symbolic_thread_context_for(concrete_tid)
//   * use for pthread_create
//
// klee_get_concrete_thread_context()
// klee_get_concrete_thread_context_for(symbolic_tid)
//   * use for indexing into __tsync
//   * returns symbolic if the concrete thing cannot be determined precisely
//
// FIXME: Accesses to __tsync[i] really should all go through the symbolic tids.
// The last bullet point above gets at an ugly (and probably technically incorrect)
// feature.  Also see the ugly "on failure" case in handleGetConcreteThreadContext.
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// The PThreads API
////////////////////////////////////////////////////////////////////////////////

DEFINE_PTHREAD_MODEL(pthread_t, pthread_self, void) {
  return klee_get_symbolic_thread_context();
}

DEFINE_PTHREAD_MODEL(int, pthread_create, pthread_t *thread, const pthread_attr_t *attr,
                     void *(*start_routine)(void*), void *arg) {
  // Serialize thread operations in the HBGraph
  __tsync.last_hbnode = __thread_make_hbnode_from(__tsync.last_hbnode);

  // Look for an available thread
  unsigned int newIdx;
  STATIC_LIST_ALLOC(__tsync.threads, newIdx);

  if (newIdx == KLEE_MAX_THREADS)
    return EAGAIN;

  // Create the new thread
  thread_data_t *tdata = &__tsync.threads[newIdx].data;
  tdata->terminated = 0;
  tdata->joinable = 1; // TODO: Read this from an attribute
  tdata->hasjoiner = 0;
  tdata->wlist = klee_get_wlist();
  __tsync.live_threads++;

  klee_thread_create(newIdx, start_routine, arg);

  *thread = klee_get_symbolic_thread_context_for(newIdx);

  return 0;
}

DEFINE_PTHREAD_MODEL(void, pthread_exit, void *value_ptr) {
  const uint64_t idx = klee_get_concrete_thread_context();
  thread_data_t *tdata = &__tsync.threads[idx].data;

#ifdef DISABLE_KLEE_PTHREAD_SYMBOLIC_CLEANUP_ON_EXIT
  while (!klee_is_symbolic(tdata->cleanup_stack) && tdata->cleanup_stack)
#else
  while (tdata->cleanup_stack)
#endif
  {
    struct _pthread_cleanup_buffer *b = tdata->cleanup_stack;
    b->__routine(b->__arg);
    tdata->cleanup_stack = b->__prev;
  }

  // Serialize thread operations in the HBGraph
  __tsync.last_hbnode = __thread_make_hbnode_from(__tsync.last_hbnode);

  // Exit
  KLEE_PUSH_BRANCH_COUNTER_DISABLED;
  if (tdata->joinable) {
    tdata->terminated = 1;
    tdata->return_value = value_ptr;
    KLEE_POP_BRANCH_COUNTER_DISABLED;
    __thread_notify_all_with_hbedges(&tdata->wlist, SyncOpJoinThread, __tsync.last_hbnode, 0);
  } else {
    KLEE_POP_BRANCH_COUNTER_DISABLED;
    STATIC_LIST_CLEAR(__tsync.threads, idx);
  }

  // Notify for pthread_joinall()
  __tsync.live_threads--;
  __thread_notify_all_with_hbedges(&__tsync.joinall_wlist, SyncOpJoinAllThreads,
                                   __tsync.last_hbnode, 0);

  // Does not return
  klee_thread_terminate();
}

DEFINE_PTHREAD_MODEL(int, pthread_join, pthread_t thread, void **value_ptr) {
  // Serialize thread operations in the HBGraph
  __tsync.last_hbnode = __thread_make_hbnode_from(__tsync.last_hbnode);

  // XXX: ugly case: see comments at top
  const uint64_t idx = klee_get_concrete_thread_context_for(thread);

  // N.B.: We have to be careful when loading fields from tdata.
  // If we're not careful, the "thread >= KLEE_MAX_THREADS" in-bounds check
  // will be superceded by a load of tdata, which will assume that "thread"
  // is in-bounds (effectively neutering our in-bounds check).

  // Gross offset arithmetic
  thread_data_t *tdata = &__tsync.threads[idx].data;
  const uintptr_t tdataOffset = (uintptr_t)tdata - (uintptr_t)&__tsync;
#define LOAD_TDATA(fieldname) \
  klee_load8_no_boundscheck(&__tsync, tdataOffset + offsetof(thread_data_t, fieldname))
  const char isAllocated = LOAD_TDATA(allocated);
  const char isJoinable  = LOAD_TDATA(joinable);
  const char hasJoiner   = LOAD_TDATA(hasjoiner);
#undef LOAD_TDATA

  // Error cases
  int retval =
    klee_expr_select32(thread >= KLEE_MAX_THREADS, ESRCH,
    klee_expr_select32(thread == pthread_self(), EDEADLK,
    klee_expr_select32(!isAllocated, ESRCH,
    klee_expr_select32(klee_expr_or(!isJoinable, hasJoiner), EINVAL, 0))));

  if (retval != 0) {
    klee_debug("PTHREAD_JOIN :: ERROR CASE ..........\n");
    //klee_print_expr_concretizations("PTHREAD_JOIN :: error", retval, 8);
    return retval;
  }

  tdata->hasjoiner = 1;

  // XXX
  //klee_debug("PTHREAD_JOIN :: JOINING ..........\n");
  //klee_print_expr_only("thread:", thread);
  //klee_print_expr_only("tdata->terminated:", tdata->terminated);

  if (!tdata->terminated) {
    klee_debug("PTHREAD_JOIN :: SLEEPING!!!!\n");
    __thread_sleep(&tdata->wlist, SyncOpJoinThread);
    klee_debug("PTHREAD_JOIN :: WOKEN FROM SLEEP!!!!\n");
  } else {
    klee_debug("PTHREAD_JOIN :: ALREADY EXITED\n");
    klee_add_hbedge_from_threads_last_hbnode(thread, __thread_make_hbnode());
  }

  if (value_ptr) {
    //klee_debug("PTHREAD_JOIN :: saving return value ...\n");
    *value_ptr = tdata->return_value;
    //klee_debug("PTHREAD_JOIN :: saved!\n");
  }

  //klee_debug("PTHREAD_JOIN :: clearing threads[] ...\n");
  STATIC_LIST_CLEAR(__tsync.threads, idx);
  klee_debug("PTHREAD_JOIN :: done!\n");

  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_joinall, void) {
  // Serialize thread operations in the HBGraph
  __tsync.last_hbnode = __thread_make_hbnode_from(__tsync.last_hbnode);

  // Wait for all other threads to exit
  // It's an unchecked error for multiple threads to call this concurrently
  while (__tsync.live_threads > 1) {
    __thread_sleep(&__tsync.joinall_wlist, SyncOpJoinAllThreads);
  }

  return 0;
}

// FIXME: merge error cases as in pthread_join
DEFINE_PTHREAD_MODEL(int, pthread_detach, pthread_t thread) {
  if (thread >= KLEE_MAX_THREADS)
    return ESRCH;

  // Serialize thread operations in the HBGraph
  __tsync.last_hbnode = __thread_make_hbnode_from(__tsync.last_hbnode);

  // XXX: ugly case: see comments at top
  const uint64_t idx = klee_get_concrete_thread_context_for(thread);

  // Attempt to detach
  thread_data_t *tdata = &__tsync.threads[idx].data;

  if (!tdata->allocated)
    return ESRCH;

  if (!tdata->joinable)
    return EINVAL;

  if (tdata->terminated) {
    STATIC_LIST_CLEAR(__tsync.threads, idx);
  } else {
    tdata->joinable = 0;
  }

  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_attr_init, pthread_attr_t *attr) {
  klee_warning("pthread_attr_init does nothing");
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_attr_destroy, pthread_attr_t *attr) {
  klee_warning("pthread_attr_destroy does nothing");
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_once, pthread_once_t *once_control, void (*init_routine)(void)) {
  if (*once_control == 0) {
    // Block other threads from continuing while the routine executes
    *once_control = (pthread_once_t)-1;
    init_routine();
    // Store a completion hbnode id in the sync object
    *once_control = (pthread_once_t)__thread_make_hbnode();
  } else {
    // Wait for the routine to finish
    while (*once_control == (pthread_once_t)-1)
      __thread_preempt(1);
    __thread_make_hbnode_from(*once_control);
  }

  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_equal, pthread_t thread1, pthread_t thread2) {
  return thread1 == thread2;
}

DEFINE_PTHREAD_MODEL(int, pthread_yield, void) {
  __thread_preempt(1);
  return 0;
}

DEFINE_PTHREAD_MODEL(int, sched_yield, void) {
  __thread_preempt(1);
  return 0;
}

//
// Unsupported
//

DEFINE_PTHREAD_MODEL(int, pthread_setcancelstate, int state, int *oldstate) {
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_setcanceltype, int type, int *oldtype) {
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_cancel, pthread_t thread) {
  FUNCTION_NOT_IMPLEMENTED;
}

DEFINE_PTHREAD_MODEL(int, pthread_testcancel, void) {
  FUNCTION_NOT_IMPLEMENTED;
}

DEFINE_PTHREAD_MODEL(int, pthread_key_create, pthread_key_t *key, void (*dtor) (void *)) {
  FUNCTION_NOT_IMPLEMENTED;
}

DEFINE_PTHREAD_MODEL(int, pthread_key_delete, pthread_key_t key) {
  FUNCTION_NOT_IMPLEMENTED;
}

DEFINE_PTHREAD_MODEL(void*, pthread_getspecific, pthread_key_t key) {
  FUNCTION_NOT_IMPLEMENTED;
}

DEFINE_PTHREAD_MODEL(int, pthread_setspecific, pthread_key_t key, const void *ptr) {
  FUNCTION_NOT_IMPLEMENTED;
}

////////////////////////////////////////////////////////////////////////////////
// uClibc defines pthread_cleanup_* as macros that call these
////////////////////////////////////////////////////////////////////////////////

DEFINE_PTHREAD_MODEL(void, _pthread_cleanup_push, struct _pthread_cleanup_buffer *buffer, void (*routine)(void*), void *arg) {
  const uint64_t idx = klee_get_concrete_thread_context();
  thread_data_t *tdata = &__tsync.threads[idx].data;
  buffer->__routine = routine;
  buffer->__arg = arg;
  buffer->__prev = tdata->cleanup_stack;
  tdata->cleanup_stack = buffer;
}

DEFINE_PTHREAD_MODEL(void, _pthread_cleanup_pop, struct _pthread_cleanup_buffer *buffer, int execute) {
  const uint64_t idx = klee_get_concrete_thread_context();
  thread_data_t *tdata = &__tsync.threads[idx].data;
  if (execute) {
    buffer->__routine(buffer->__arg);
  }
  tdata->cleanup_stack = buffer->__prev;
}

// TODO: with cancellation, we also need to update the canceltype
DEFINE_PTHREAD_MODEL(void, _pthread_cleanup_push_defer, struct _pthread_cleanup_buffer *buffer, void (*routine)(void*), void *arg) {
  _pthread_cleanup_push(buffer, routine, arg);
}

// TODO: with cancellation, we also need to update the canceltype
DEFINE_PTHREAD_MODEL(void, _pthread_cleanup_pop_restore, struct _pthread_cleanup_buffer *buffer, int execute) {
  _pthread_cleanup_pop(buffer, execute);
}

////////////////////////////////////////////////////////////////////////////////
// Pthreads LibC initializer
////////////////////////////////////////////////////////////////////////////////

DEFINE_PTHREAD_MODEL(void, pthread_initialize, void) {
  // nop: this one is probably uclibc-specific
}

DEFINE_PTHREAD_MODEL(void, __pthread_initialize, void) {
  // nop
}

DEFINE_PTHREAD_MODEL(void, __pthread_initialize_minimal, void) {
  // nop
}
