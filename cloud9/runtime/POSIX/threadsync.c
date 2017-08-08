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
#include <stdlib.h>
#include <string.h>

#include "multiprocess.h"

FORCE_LINKAGE(threadsync)

// N.B.: Static initializers (PTHREAD_MUTEX_INITIALIZER, etc) do an incomplete
// job: we must do a bit of dynamic initialization (mostly to ask klee for a
// fresh wlist id).  By design, the pthreads static initializers for mutexes,
// condvars, barriers, and rwlocks all assign wlists to fiels 0.  This means
// that all sync objects will have "wlist == 0" if they were initialized with
// a static initializer.  Symbolic synchronization objects are more complicated;
// those are handled by klee_fork_for_concrete_initialization().

// N.B.: We could perhaps find more bugs by writing a magic number into the
// struct for each sync op (this would check for bugs where an uninitialized
// sync object is passed to a sync op).

// N.B.: We inline all static functions in this file so that only the model
// functions remain.  This ensures that klee can check if it's executing in
// a model function by simply examining the function's name (otherwise, klee
// would need to know the names of all static functions defined in this file).

// N.B.: We attempt to minimize the number of LLVM branch instructions generated
// for this file.  To do this, we construct branch conditions as klee expressions
// (via klee_expr_*), then branch on that given expression.

// N.B.: Mutexes and condvars maintain a "queued" field that tracks the number
// of threads waiting in the queue.  Protocol is for the NOTIFIER to decrement
// this field after notifying a thread.  This avoids a bug that occurs when the
// waiter decrements this field after waking (the bug: T0 sleeps, T1 notifies,
// T2 notifies, T0 wakes; since T0 is not scheduled until after T2, the "queued"
// field will have an incorrect value when T2 runs; doing the descrement in T1
// fixes this).

////////////////////////////////////////////////////////////////////////////////
// Uclibc wrappers
////////////////////////////////////////////////////////////////////////////////

__attribute__((noinline)) int __uClibc_wrap_pthread_mutex_lock(pthread_mutex_t *m) {
  return pthread_mutex_lock(m);
}
__attribute__((noinline)) int __uClibc_wrap_pthread_mutex_trylock(pthread_mutex_t *m) {
  return pthread_mutex_trylock(m);
}
__attribute__((noinline)) int __uClibc_wrap_pthread_mutex_unlock(pthread_mutex_t *m) {
  return pthread_mutex_unlock(m);
}

////////////////////////////////////////////////////////////////////////////////
// Utils
////////////////////////////////////////////////////////////////////////////////

static inline enum InitState should_init_symbolic(enum SyncOpType type, hbnode_id_t *hbnode) {
#ifdef DISABLE_KLEE_SYNC_INVARIANTS
  // This effectively disables sync invariants
  return AlreadyInit;
#else
  // See definition of InitState for a description of the return value
  return klee_fork_for_concrete_initialization(type, hbnode, *hbnode);
#endif
}

// These wrap the check_init functions

static inline void startInternal() {
  klee_set_option(KLEE_OPTION_BRANCH_COUNTER_ENABLED, 0);
}

static inline void endInternal() {
  klee_set_option(KLEE_OPTION_BRANCH_COUNTER_ENABLED, 1);
}

////////////////////////////////////////////////////////////////////////////////
// POSIX Mutex Attributes
// FIXME: support these again
////////////////////////////////////////////////////////////////////////////////

DEFINE_PTHREAD_MODEL(int, pthread_mutexattr_init, pthread_mutexattr_t *attr) {
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_mutexattr_destroy, pthread_mutexattr_t *attr) {
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_mutexattr_setpshared, pthread_mutexattr_t *attr, int pshared) {
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_mutexattr_settype, pthread_mutexattr_t *attr, int type) {
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_mutexattr_getpshared, pthread_mutexattr_t *attr, int *pshared) {
  *pshared = PTHREAD_PROCESS_PRIVATE;
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_mutexattr_gettype, pthread_mutexattr_t *attr, int *type) {
  *type = PTHREAD_MUTEX_NORMAL;
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
// POSIX Mutexes
// TODO: bring back recursive locks?
////////////////////////////////////////////////////////////////////////////////

__attribute__((always_inline))
static void mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
  mutex->wlist = klee_get_wlist();
  mutex->last_hbnode = 0;
  mutex->taken = 0;
  mutex->owner = 0; // don't care since it's not taken
  mutex->queued = 0;
}

__attribute__((always_inline))
static void mutex_init_symbolic(pthread_mutex_t *mutex, const int unique) {
  klee_debug("############### MUTEX INIT SYMBOLIC ################\n");

  // Invoked to initialize a mutex into a purely symbolic state
  // mutex->wlist stays symbolic

  if (unique) {
    mutex->last_hbnode = 0;   // the HB graph "begins here"
  }

  // Invariants
  // (a) on taken and owner (see klee.h and impl of klee_enumerate_possible_lock_owners)
  unsigned int owners = klee_enumerate_possible_lock_owners(mutex, mutex->taken, mutex->owner);
  klee_print_expr_only("OWNERS:", owners);
  if (!owners) {
    mutex->taken = 0;
  }

  // (b) NumMustBeWaiting <= queued <= NumMayBeWaiting
  unsigned may, must;
  __enumerate_possible_waiters(&mutex->wlist, SyncOpMutex, &may, &must);
  klee_assume(must <= mutex->queued);
  klee_assume(mutex->queued <= may);
  klee_print_expr_only("MAY:", may);
  klee_print_expr_only("MUST:", must);
}

__attribute__((always_inline))
static void mutex_check_init(pthread_mutex_t *mutex) {
  startInternal();
  switch (should_init_symbolic(SyncOpMutex, &mutex->last_hbnode)) {
  case AlreadyInit:
    if (!klee_is_symbolic(mutex->wlist) && mutex->wlist == 0)
      mutex->wlist = klee_get_wlist();  // statically initialized mutexes need a wlist
    break;
  case NeedsSymbolicInitAliased:
    mutex_init_symbolic(mutex, 0);
    break;
  case NeedsSymbolicInitUnique:
    mutex_init_symbolic(mutex, 1);
    break;
  }
  endInternal();
}

__attribute__((always_inline))
static int atomic_mutex_lock(pthread_mutex_t *mutex, char try) {
  mutex_check_init(mutex);

  hbnode_id_t hbnode = __thread_make_hbnode_from(mutex->last_hbnode);

  const char mustWait = klee_expr_or(mutex->taken != 0, mutex->queued > 0);
  if (mustWait) {
    if (try)
      return EBUSY;

    mutex->queued++;  // notifier will decrement
    __thread_sleep(&mutex->wlist, SyncOpMutex);
    mutex_check_init(mutex);
    hbnode = klee_last_hbnode();
  }

  mutex->taken = 1;
  mutex->owner = pthread_self();
  mutex->last_hbnode = hbnode;
  klee_lockset_add(mutex);

  return 0;
}

__attribute__((always_inline))
static int atomic_mutex_unlock(pthread_mutex_t *mutex) {
  mutex_check_init(mutex);

  const char isDataRace = klee_expr_or(mutex->taken == 0, mutex->owner != pthread_self());
  if (isDataRace) {
    // NOTE: This is a data race!
    return EPERM;
  }

  mutex->taken = 0;
  mutex->last_hbnode = __thread_make_hbnode();
  klee_lockset_remove(mutex);

  if (mutex->queued > 0) {
    __thread_notify_one_with_expected(&mutex->wlist, SyncOpMutex, mutex->queued);
    mutex->queued--;
  }

  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_mutex_init, pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
  mutex_init(mutex, attr);
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_mutex_destroy, pthread_mutex_t *mutex) {
  // nop
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_mutex_lock, pthread_mutex_t *mutex) {
  int res = atomic_mutex_lock(mutex, 0);
  if (res == 0)
    __thread_preempt(0);
  return res;
}

DEFINE_PTHREAD_MODEL(int, pthread_mutex_trylock, pthread_mutex_t *mutex) {
  int res = atomic_mutex_lock(mutex, 1);
  if (res == 0)
    __thread_preempt(0);
  return res;
}

DEFINE_PTHREAD_MODEL(int, pthread_mutex_unlock, pthread_mutex_t *mutex) {
  int res = atomic_mutex_unlock(mutex);
#ifdef ENABLE_KLEE_PTHREAD_ALWAYS_PREEMPT_UNLOCK
  __thread_preempt(1);
#else
  __thread_preempt(0);
#endif
  return res;
}

////////////////////////////////////////////////////////////////////////////////
// POSIX Condition Variables
////////////////////////////////////////////////////////////////////////////////

__attribute__((always_inline))
static void cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
  cond->wlist = klee_get_wlist();
  cond->mutex = NULL;
  cond->queued = 0;
  cond->fake_initialized = 0;  // set to a constant so we don't invoke init_symbolic()
}

__attribute__((always_inline))
static void cond_init_symbolic(pthread_cond_t *cond, const int unique) {
  klee_debug("############### COND INIT SYMBOLIC ################\n");
  // Invoked to initialize a condvar into a purely symbolic state
  // cond->wlist stays symbolic

  if (unique) {
    cond->fake_initialized = 0;  // so we don't initialize again
  }

  // Invariants
  // (a) NumMustBeWaiting <= queued <= NumMayBeWaiting
  unsigned may, must;
  __enumerate_possible_waiters(&cond->wlist, SyncOpCondvar, &may, &must);
  klee_assume(must <= cond->queued);
  klee_assume(cond->queued <= may);
}

__attribute__((always_inline))
static void cond_check_init(pthread_cond_t *cond) {
  startInternal();
  switch (should_init_symbolic(SyncOpCondvar, &cond->fake_initialized)) {
  case AlreadyInit:
    if (!klee_is_symbolic(cond->wlist) && cond->wlist == 0)
      cond->wlist = klee_get_wlist();  // statically initialized conds need a wlist
    break;
  case NeedsSymbolicInitAliased:
    cond_init_symbolic(cond, 0);
    break;
  case NeedsSymbolicInitUnique:
    cond_init_symbolic(cond, 1);
    break;
  }
  endInternal();
}

__attribute__((always_inline))
static int atomic_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  cond_check_init(cond);

  if (cond->queued > 0) {
    // Pthread spec: using a different mutex is undefined, so we will
    // simply fail if the mutexes do not match.
    klee_assert(cond->mutex == mutex);
  } else {
    cond->mutex = mutex;
  }

  if (atomic_mutex_unlock(mutex) != 0)
    return EPERM;

  cond->queued++;
  __thread_sleep(&cond->wlist, SyncOpCondvar);
  cond_check_init(cond);

  if (atomic_mutex_lock(mutex, 0) != 0)
    return EPERM;

  return 0;
}

__attribute__((always_inline))
static int atomic_cond_notify(pthread_cond_t *cond, char all) {
  cond_check_init(cond);

  uint64_t hbnode = klee_add_hbnode();
  if (cond->queued > 0) {
    if (all) {
      __thread_notify_all_with_extras(&cond->wlist, SyncOpCondvar, hbnode, 0, cond->queued, 0);
      cond->queued = 0;
    } else {
      __thread_notify_one_with_extras(&cond->wlist, SyncOpCondvar, hbnode, 0, cond->queued, 0);
      cond->queued--;
    }
  }

  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_cond_init, pthread_cond_t *cond, const pthread_condattr_t *attr) {
  cond_init(cond, attr);
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_cond_destroy, pthread_cond_t *cond) {
  // nop
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_cond_timedwait, pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime) {
  FUNCTION_NOT_IMPLEMENTED;
}

DEFINE_PTHREAD_MODEL(int, pthread_cond_wait, pthread_cond_t *cond, pthread_mutex_t *mutex) {
  int res = atomic_cond_wait(cond, mutex);
  if (res == 0)
    __thread_preempt(0);
  return res;
}

DEFINE_PTHREAD_MODEL(int, pthread_cond_broadcast, pthread_cond_t *cond) {
  int res = atomic_cond_notify(cond, 1);
  if (res == 0)
    __thread_preempt(0);
  return res;
}

DEFINE_PTHREAD_MODEL(int, pthread_cond_signal, pthread_cond_t *cond) {
  int res = atomic_cond_notify(cond, 0);
  if (res == 0)
    __thread_preempt(0);
  return res;
}

////////////////////////////////////////////////////////////////////////////////
// POSIX Barriers
////////////////////////////////////////////////////////////////////////////////

__attribute__((always_inline))
static void barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr,
                         unsigned int count) {
  barrier->wlist = klee_get_wlist();
  barrier->init_count = count;
  barrier->left = count;
  barrier->fake_initialized = 0;  // set to a constant so we don't invoke init_symbolic()
}

__attribute__((always_inline))
static void barrier_init_symbolic(pthread_barrier_t *barrier, const int unique) {
  // Invoked to initialize a barrier into a purely symbolic state
  // barrier->wlist stays symbolic

  if (unique) {
    barrier->fake_initialized = 0;  // so we don't initialize again
  }

  // Invariants
  // (a) on init_count (see klee.h and impl of klee_enumerate_possible_barrier_init_counts)
  unsigned uniqueCount = 1;
  unsigned counts = klee_enumerate_possible_barrier_init_counts(barrier, barrier->init_count, &uniqueCount);
  if (counts == 1) {
    barrier->init_count = uniqueCount;
  }

  // (b) (init_count - NumMayBeWaiting) <= left <= (init_count - NumMustBeWaiting)
  unsigned may, must;
  __enumerate_possible_waiters(&barrier->wlist, SyncOpBarrier, &may, &must);
  klee_assume((barrier->init_count - must) <= barrier->left);
  klee_assume(barrier->left <= (barrier->init_count - may));
}

__attribute__((always_inline))
static void barrier_check_init(pthread_barrier_t *barrier) {
  startInternal();
  switch (should_init_symbolic(SyncOpBarrier, &barrier->fake_initialized)) {
  case AlreadyInit:
    if (!klee_is_symbolic(barrier->wlist) && barrier->wlist == 0)
      barrier->wlist = klee_get_wlist();  // statically initialized barriers need a wlist
    break;
  case NeedsSymbolicInitAliased:
    barrier_init_symbolic(barrier, 0);
    break;
  case NeedsSymbolicInitUnique:
    barrier_init_symbolic(barrier, 1);
    break;
  }
  endInternal();
}

DEFINE_PTHREAD_MODEL(int, pthread_barrier_init, pthread_barrier_t *barrier, const pthread_barrierattr_t *attr, unsigned int count) {
  if (count == 0)
    return EINVAL;

  barrier_init(barrier, attr, count);
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_barrier_destroy, pthread_barrier_t *barrier) {
  // nop
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_barrier_wait, pthread_barrier_t *barrier) {
  barrier_check_init(barrier);
  int result = 0;

  klee_print_expr_concretizations("INIT BARRIER.INIT: ", barrier->init_count, 5);
  if (barrier->init_count == 0)
    return EINVAL;

  --barrier->left;

  if (barrier->left == 0) {
    barrier->left = barrier->init_count;

    // Expected number of waiting threads
    int expected = barrier->init_count - 1;

    // HB node for the serial thread
    uint64_t serial = klee_add_hbnode();
    // N-1 hb edges for barrier arrival
    // N-1 hb edges for barrier departure
    __thread_notify_all_with_extras(&barrier->wlist, SyncOpBarrier, serial, serial,
                                    expected, 1 /* this is a region marker */);
    barrier_check_init(barrier);
    result = PTHREAD_BARRIER_SERIAL_THREAD;
  } else {
    __thread_sleep(&barrier->wlist, SyncOpBarrier);
    barrier_check_init(barrier);
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
// POSIX Read Write Locks
// TODO: recheck this implementation
////////////////////////////////////////////////////////////////////////////////

__attribute__((always_inline))
static void rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr) {
  rwlock->wlist_readers = klee_get_wlist();
  rwlock->wlist_writers = klee_get_wlist();
  rwlock->last_reader_hbgroup = 0;
  rwlock->last_writer_hbnode = 0;
  rwlock->nr_readers = 0;
  rwlock->nr_readers_queued = 0;
  rwlock->nr_writers_queued = 0;
  rwlock->writer = 0;
}

// XXX
__attribute__((always_inline))
static void rwlock_init_symbolic(pthread_rwlock_t *rwlock, const int unique) {
  // Invoked to initialize a rwlock into a purely symbolic state
  rwlock->wlist_readers = klee_get_wlist();
  rwlock->wlist_writers = klee_get_wlist();

  if (unique) {
    rwlock->last_reader_hbgroup = 0;
    rwlock->last_writer_hbnode = 0;
  }

  // Invariants
  // XXX TODO
}

__attribute__((always_inline))
static void rwlock_check_init(pthread_rwlock_t *rwlock) {
  startInternal();
  switch (should_init_symbolic(SyncOpRwlock, &rwlock->last_reader_hbgroup)) {
  case AlreadyInit:
    if (!klee_is_symbolic(rwlock->wlist_readers) && rwlock->wlist_readers == 0) {
      rwlock->wlist_readers = klee_get_wlist();  // statically initialized rwlocke need wlists
      rwlock->wlist_writers = klee_get_wlist();
    }
    break;
  case NeedsSymbolicInitAliased:
    rwlock_init_symbolic(rwlock, 0);
    break;
  case NeedsSymbolicInitUnique:
    rwlock_init_symbolic(rwlock, 1);
    break;
  }
  endInternal();
}

// XXX: support symbolic rwlocks (need lockset update, ...)
__attribute__((always_inline))
static int atomic_rwlock_rdlock(pthread_rwlock_t *rwlock, char try) {
  rwlock_check_init(rwlock);

  if (rwlock->writer != 0 || rwlock->nr_writers_queued != 0) {
    if (try)
      return EBUSY;

    if (++rwlock->nr_readers_queued == 0) {
      --rwlock->nr_readers_queued;
      return EAGAIN;
    }

    __thread_sleep(&rwlock->wlist_readers, SyncOpRwlock);
    rwlock_check_init(rwlock);
  }

  if (++rwlock->nr_readers == 0) {
    --rwlock->nr_readers;
    return EAGAIN;
  }

  __thread_make_hbnode_from(rwlock->last_writer_hbnode);

  return 0;
}

// XXX: support symbolic rwlocks (need lockset update, ...)
// XXX: minimize branches as in atomic_mutex_lock
__attribute__((always_inline))
static int atomic_rwlock_wrlock(pthread_rwlock_t *rwlock, char try) {
  rwlock_check_init(rwlock);

  if (rwlock->writer != 0 || rwlock->nr_readers != 0) {
    if (try != 0)
      return EBUSY;

    if (++rwlock->nr_writers_queued == 0) {
      --rwlock->nr_writers_queued;
      return EAGAIN;
    }

    __thread_sleep(&rwlock->wlist_writers, SyncOpRwlock);
    rwlock_check_init(rwlock);
  }

  // N.B.: Can't write the writer's thread id because 0 is a valid thread id
  rwlock->writer = 1;

  // Take incoming HB edges from the last readers, if any, otherwise from the last writer
  hbnode_id_t n = __thread_make_hbnode();
  if (rwlock->last_reader_hbgroup) {
    klee_add_hbedges_from_hbgroup(rwlock->last_reader_hbgroup, n);
    klee_remove_hbgroup(rwlock->last_reader_hbgroup);
    rwlock->last_reader_hbgroup = 0;
  } else {
    klee_add_hbedge(rwlock->last_writer_hbnode, n);
  }

  return 0;
}

// XXX: support symbolic rwlocks (need lockset update, ...)
__attribute__((always_inline))
static int atomic_rwlock_unlock(pthread_rwlock_t *rwlock) {
  if (rwlock->writer == 0 && rwlock->nr_readers == 0)
    return EPERM;

  // Unlock
  if (rwlock->writer) {
    rwlock->writer = 0;
    rwlock->last_writer_hbnode = __thread_make_hbnode();
  } else {
    --rwlock->nr_readers;

    hbnode_id_t n = __thread_make_hbnode();
    if (rwlock->last_reader_hbgroup == 0) {
      rwlock->last_reader_hbgroup = n;
    }
    klee_add_hbnode_to_hbgroup(n, rwlock->last_reader_hbgroup);
  }

  // Notify waiters
  if (rwlock->nr_readers == 0 && rwlock->nr_writers_queued) {
    __thread_notify_one_with_expected(&rwlock->wlist_writers, SyncOpRwlock, rwlock->nr_writers_queued);
    rwlock->nr_writers_queued--;
  } else if (rwlock->nr_readers_queued) {
    __thread_notify_all_with_expected(&rwlock->wlist_readers, SyncOpRwlock, rwlock->nr_readers_queued);
    rwlock->nr_readers_queued = 0;
  }

  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_rwlock_init, pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr) {
  rwlock_init(rwlock, attr);
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_rwlock_destroy, pthread_rwlock_t *rwlock) {
  // nop
  return 0;
}

DEFINE_PTHREAD_MODEL(int, pthread_rwlock_rdlock, pthread_rwlock_t *rwlock) {
  int res = atomic_rwlock_rdlock(rwlock, 0);
  if (res == 0)
    __thread_preempt(0);
  return res;
}

DEFINE_PTHREAD_MODEL(int, pthread_rwlock_tryrdlock, pthread_rwlock_t *rwlock) {
  int res = atomic_rwlock_rdlock(rwlock, 1);
  if (res == 0)
    __thread_preempt(0);
  return res;
}

DEFINE_PTHREAD_MODEL(int, pthread_rwlock_wrlock, pthread_rwlock_t *rwlock) {
  int res = atomic_rwlock_wrlock(rwlock, 0);
  if (res == 0)
    __thread_preempt(0);
  return res;
}

DEFINE_PTHREAD_MODEL(int, pthread_rwlock_trywrlock, pthread_rwlock_t *rwlock) {
  int res = atomic_rwlock_wrlock(rwlock, 1);
  if (res == 0)
    __thread_preempt(0);
  return res;
}

DEFINE_PTHREAD_MODEL(int, pthread_rwlock_unlock, pthread_rwlock_t *rwlock) {
  int res = atomic_rwlock_unlock(rwlock);
  if (res == 0)
    __thread_preempt(0);
  return res;
}

////////////////////////////////////////////////////////////////////////////////
// POSIX Semaphores
////////////////////////////////////////////////////////////////////////////////

// TODO: need some sort of static analysis for semaphores
// TODO: if #must-be-waiting-on-wlist > 1, then count must be == 0?

__attribute__((always_inline))
static void sem_init_symbolic(sem_t *sem, const int unique) {
  klee_debug("############### SEM INIT SYMBOLIC ################\n");

  if (unique) {
    sem->last_hbnode = 0;   // the HB graph "begins here"
  }

  klee_assume(sem->count < SEM_VALUE_MAX);
}

__attribute__((always_inline))
static void sem_check_init(sem_t *sem) {
  startInternal();
  switch (should_init_symbolic(SyncOpSem, &sem->last_hbnode)) {
  case AlreadyInit:
    break;
  case NeedsSymbolicInitAliased:
    sem_init_symbolic(sem, 0);
    break;
  case NeedsSymbolicInitUnique:
    sem_init_symbolic(sem, 1);
    break;
  }
  endInternal();
}

__attribute__((always_inline))
static int atomic_sem_down(sem_t *sem, char try) {
  sem_check_init(sem);

  hbnode_id_t hbnode = __thread_make_hbnode_from(sem->last_hbnode);

  if (sem->count == 0) {
    if (try) {
      errno = EAGAIN;
      return -1;
    }

    // TODO: make FIFO?
    __thread_sleep(&sem->wlist, SyncOpSem);
    sem_check_init(sem);
    hbnode = klee_last_hbnode();
  }

  sem->count--;
  sem->last_hbnode = hbnode;

  return 0;
}

__attribute__((always_inline))
static int atomic_sem_up(sem_t *sem) {
  sem_check_init(sem);

  sem->count++;
  sem->last_hbnode = __thread_make_hbnode_from(sem->last_hbnode);
  __thread_notify_one(&sem->wlist, SyncOpSem);
  return 0;
}

DEFINE_PTHREAD_MODEL(int, sem_init, sem_t *sem, int pshared, unsigned int value) {
  if(value > SEM_VALUE_MAX) {
    errno = EINVAL;
    return -1;
  }

  sem->wlist = klee_get_wlist();
  sem->last_hbnode = 0;
  sem->count = value;
  return 0;
}

DEFINE_PTHREAD_MODEL(int, sem_destroy, sem_t *sem) {
  return 0;
}

DEFINE_PTHREAD_MODEL(int, sem_wait, sem_t *sem) {
  int res = atomic_sem_down(sem, 0);
  if (res == 0)
    __thread_preempt(0);
  return res;
}

DEFINE_PTHREAD_MODEL(int, sem_trywait, sem_t *sem) {
  int res = atomic_sem_down(sem, 1);
  if (res == 0)
    __thread_preempt(0);
  return res;
}

DEFINE_PTHREAD_MODEL(int, sem_post, sem_t *sem) {
  int res = atomic_sem_up(sem);
#ifdef ENABLE_KLEE_PTHREAD_ALWAYS_PREEMPT_UNLOCK
  __thread_preempt(1);
#else
  __thread_preempt(0);
#endif
  return res;
}
