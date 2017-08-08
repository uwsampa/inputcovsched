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

#ifndef THREADS_H_
#define THREADS_H_

#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>

#include "common.h"

#define DEFAULT_THREAD  0

#define DEFAULT_PROCESS 2
#define DEFAULT_PARENT  1
#define DEFAULT_UMASK   (S_IWGRP | S_IWOTH)

#define PID_TO_INDEX(pid)   ((pid) - 2)
#define INDEX_TO_PID(idx)   ((idx) + 2)

////////////////////////////////////////////////////////////////////////////////
// OLD SEMAPHORE STUFF: System Wide Data Structures
////////////////////////////////////////////////////////////////////////////////

#ifdef ENABLE_KLEE_SEMAPHORES
typedef struct {
  // Taken from the semaphore specs
  unsigned short semval;
  unsigned short semzcnt;
  unsigned short semncnt;
  pid_t sempid;
} sem_t;

typedef struct {
  struct semid_ds descriptor;
  sem_t *sems;

  char allocated;
} sem_set_t;

extern sem_set_t __sems[MAX_SEMAPHORES];
#endif

////////////////////////////////////////////////////////////////////////////////
// OLD SEMAPHORE STUFF: Process-local Structures
////////////////////////////////////////////////////////////////////////////////

#ifdef ENABLE_KLEE_SEMAPHORES
typedef struct {
  wlist_id_t wlist;

  int count;
  char allocated;
  int thread_level;
  pid_t owner;
} sem_data_t;
#endif

////////////////////////////////////////////////////////////////////////////////
// API shorthands
////////////////////////////////////////////////////////////////////////////////

#define inline inline __attribute__((always_inline))

/*
 * Wrapper over the klee_thread_preempt() call.
 * XXX: This was done to simulate checking for received
 * signals when being preempted.
 */
static inline void __thread_preempt(int yield) {
  klee_thread_preempt(yield);
}

/*
 * Synchronization object kinds for klee_thread_sleep
 * and klee_thread_notify.  Describes the reason for
 * sleeping.
 */

enum SyncOpType {
  SyncOpOther = 0,
  SyncOpJoinProc,
  SyncOpJoinThread,
  SyncOpJoinAllThreads,
  SyncOpMutex,
  SyncOpCondvar,
  SyncOpBarrier,
  SyncOpRwlock,
  SyncOpSem,
};

/*
 * There used to be a SyncOpType that did not have unique pointers to
 * each wlist (as many objects would refer to the same wlist).  That
 * is no longer true, so this function is obsolete, but we keep it in
 * case it becomes needed again...
 */
static inline void* __canonical_wlist_addr(uint64_t *wlist_addr,
                                           enum SyncOpType wlist_type) {
  return wlist_addr;
}

/*
 * Wrapper over the klee_thread_sleep() call.
 * XXX: This was done to simulate checking for received
 * signals when being preempted.
 */
static inline void __thread_sleep(uint64_t *wlist_addr,
                                  enum SyncOpType wlist_type) {
  klee_thread_sleep(*wlist_addr, __canonical_wlist_addr(wlist_addr, wlist_type),
                    (uint64_t)wlist_type);
}

/*
 * Wrappers over the klee_thread_notify() call.
 * Hides some ugly details.
 *     with_hbedge   => notify + build hbedges
 *     with_expected => notify + tie queue size to a program variable
 *     with_extras   => full power of klee_thread_notify()
 */

static inline void __thread_notify_one(uint64_t *wlist_addr,
                                       enum SyncOpType wlist_type) {
  klee_thread_notify(*wlist_addr, __canonical_wlist_addr(wlist_addr, wlist_type),
                     (uint64_t)wlist_type, 0, 0, 0, -1, 0);
}

static inline void __thread_notify_all(uint64_t *wlist_addr,
                                       enum SyncOpType wlist_type) {
  klee_thread_notify(*wlist_addr, __canonical_wlist_addr(wlist_addr, wlist_type),
                     (uint64_t)wlist_type, 1, 0, 0, -1, 0);
}

static inline void __thread_notify_one_with_hbedges(uint64_t *wlist_addr,
                                                    enum SyncOpType wlist_type,
                                                    uint64_t source,
                                                    uint64_t sink) {
  klee_thread_notify(*wlist_addr, __canonical_wlist_addr(wlist_addr, wlist_type),
                     (uint64_t)wlist_type, 0, source, sink, -1, 0);
}

static inline void __thread_notify_all_with_hbedges(uint64_t *wlist_addr,
                                                    enum SyncOpType wlist_type,
                                                    uint64_t source,
                                                    uint64_t sink) {
  klee_thread_notify(*wlist_addr, __canonical_wlist_addr(wlist_addr, wlist_type),
                     (uint64_t)wlist_type, 1, source, sink, -1, 0);
}

static inline void __thread_notify_one_with_expected(uint64_t *wlist_addr,
                                                   enum SyncOpType wlist_type,
                                                   int64_t expected_queue_size) {
  klee_thread_notify(*wlist_addr, __canonical_wlist_addr(wlist_addr, wlist_type),
                     (uint64_t)wlist_type, 0, 0, 0,
                     expected_queue_size, 0);
}

static inline void __thread_notify_all_with_expected(uint64_t *wlist_addr,
                                                   enum SyncOpType wlist_type,
                                                   int64_t expected_queue_size) {
  klee_thread_notify(*wlist_addr, __canonical_wlist_addr(wlist_addr, wlist_type),
                     (uint64_t)wlist_type, 1, 0, 0,
                     expected_queue_size, 0);
}

static inline void __thread_notify_one_with_extras(uint64_t *wlist_addr,
                                                   enum SyncOpType wlist_type,
                                                   uint64_t source,
                                                   uint64_t sink,
                                                   int64_t expected_queue_size,
                                                   int ics_region_marker) {
  klee_thread_notify(*wlist_addr, __canonical_wlist_addr(wlist_addr, wlist_type),
                     (uint64_t)wlist_type, 0, source, sink,
                     expected_queue_size, ics_region_marker);
}

static inline void __thread_notify_all_with_extras(uint64_t *wlist_addr,
                                                   enum SyncOpType wlist_type,
                                                   uint64_t source,
                                                   uint64_t sink,
                                                   int64_t expected_queue_size,
                                                   int ics_region_marker) {
  klee_thread_notify(*wlist_addr, __canonical_wlist_addr(wlist_addr, wlist_type),
                     (uint64_t)wlist_type, 1, source, sink,
                     expected_queue_size, ics_region_marker);
}

/*
 * Wrapper over klee_enumerate_possible_waiters()
 */
static inline void __enumerate_possible_waiters(wlist_id_t *wlist_addr,
                                                enum SyncOpType wlist_type,
                                                unsigned *mayCount,
                                                unsigned *mustCount) {
  klee_enumerate_possible_waiters(*wlist_addr, __canonical_wlist_addr(wlist_addr, wlist_type),
                                  (uint64_t)wlist_type, mayCount, mustCount);
}

/*
 * Make a new HB Node with a one (default) processor-order HB edge.
 */
static inline uint64_t __thread_make_hbnode(void) {
  return klee_add_hbnode();
}
/*
 * Make a new HB Node with a one (default) processor-order HB edge
 * and a second cross-thread HB edge.
 */
static inline uint64_t __thread_make_hbnode_from(int from) {
  uint64_t n = klee_add_hbnode();
  klee_add_hbedge(from, n);
  return n;
}

#undef inline

#endif /* THREADS_H_ */
