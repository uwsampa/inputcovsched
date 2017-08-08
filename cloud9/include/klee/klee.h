//===-- klee.h --------------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef __KLEE_H__
#define __KLEE_H__

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define KLEE_CONSTANTS_FROM_KLEE_H
#include "klee/Constants.h"

#ifdef __cplusplus
extern "C" {
#endif

  //////////////////////////////////////////////////////////////////////////////
  // These are defined in runtime/Instrinsic.  All klee functions not declared
  // in this section get handled by SpecialFunctionHandler.
  //////////////////////////////////////////////////////////////////////////////

  /// Fails if z == 0
  void klee_div_zero_check(long long z);


  //////////////////////////////////////////////////////////////////////////////
  // Debugging
  //////////////////////////////////////////////////////////////////////////////

  /// Print the tree associated w/ a given expression.  The second arg is the
  /// expression to print (we leave it at '...' here to avoid type-casting).
  ///   print_expr                 :: print with the current path constraints
  ///   print_expr_only            :: print without the current path constraints
  ///   print_expr_concretizations :: print up to N concretizations (N is param #3)
  void klee_print_expr(const char *msg, ...);
  void klee_print_expr_only(const char *msg, ...);
  void klee_print_expr_concretizations(const char *msg, ...);

  /// Print the range of concrete values an expression might have.
  void klee_print_range(const char *msg, uint64_t value);

  /// Print a debug message.  Because of limited support for calling external
  /// variadic functions, klee_debug accepts either a set of up to three 32-bit
  /// integers, or a single 64-bit value (char*).
  void klee_debug(const char *format, ...);

  /// Print a warning message
  void klee_warning(const char *message);
  void klee_warning_once(const char *message);

  /// Print a stack trace of all threads
  void klee_stack_trace(void);

  /// When klee constructs concretized test case, the concrete values chosen
  /// for "*object" will satisfy "condition", if possible.
  /// REQUIRES: the address is a constant
  void klee_prefer_cex(void *object, uintptr_t condition);


  //////////////////////////////////////////////////////////////////////////////
  // Constructing and Manipulating Symbolic Values
  //////////////////////////////////////////////////////////////////////////////

  /// Return true if the given value is symbolic (represented by an
  /// expression) in the current state. This is primarily for debugging
  /// and writing tests but can also be used to enable prints in replay mode.
  unsigned klee_is_symbolic(uintptr_t n);

  /// klee_make_symbolic - Make the contents of the object pointer to by \arg
  /// addr symbolic.  At runtime, the buffer will be filled with symbolic contents.
  ///
  /// \arg addr - The start of the object.
  /// \arg nbytes - The number of bytes to make symbolic (addr+nbytes-1 should be in-bounds)
  /// \arg name - An optional name, used for identifying the object in messages,
  /// output files, etc.
  void klee_make_symbolic(void *addr, size_t nbytes, const char *name);

  /// Behaves exactly like klee_make_symbolic except the object's contents are
  /// not initialized randomly at runtime.
  void klee_read_symbolic_input(void *addr, size_t nbytes, const char *name);

  /// Shorthand to create a symbolic integer with the given name.
  /// At runtime, we will return a random value.
  uint8_t klee_int8(const char *name);
  uint32_t klee_int32(const char *name);
  uint64_t klee_int64(const char *name);

  /// Shorthand to create a symbolic integer in the signed interval [begin,end).
  /// At runtime, we will return a random value.
  int8_t klee_range8(int8_t begin, int8_t end, const char *name);
  int32_t klee_range32(int32_t begin, int32_t end, const char *name);
  int64_t klee_range64(int32_t begin, int32_t end, const char *name);

  /// Allow for programmatic building of symbolic expressions: for example,
  /// to generate more complex invariants for klee_assume().  Arguments of
  /// type 'char' are booleans: caller MUST ensure that these are either
  /// 0 or 1, since we truncate them to one bit!
  uint8_t klee_expr_and(uint8_t a, uint8_t b, ...);
  uint8_t klee_expr_or(uint8_t a, uint8_t b, ...);
  uint8_t klee_expr_iff(uint8_t a, uint8_t b);
  uint8_t klee_expr_implies(uint8_t a, uint8_t b);
  int8_t  klee_expr_select8 (uint8_t a, int8_t b,  int8_t c);
  int32_t klee_expr_select32(uint8_t a, int32_t b, int32_t c);
  int64_t klee_expr_select64(uint8_t a, int64_t b, int64_t c);

  // Return a possible constant value for the input expression.
  // This allows programs to forcibly concretize values on their own.
#define KLEE_GET_VALUE_PROTO(suffix, type) type klee_get_value##suffix(type expr)

  KLEE_GET_VALUE_PROTO(f, float);
  KLEE_GET_VALUE_PROTO(d, double);
  KLEE_GET_VALUE_PROTO(l, long);
  KLEE_GET_VALUE_PROTO(ll, long long);
  KLEE_GET_VALUE_PROTO(_i32, int32_t);
  KLEE_GET_VALUE_PROTO(_i64, int64_t);

#undef KLEE_GET_VALUE_PROTO

  /// If the supplied value is a symbolic that has a unique concrete
  /// solution, return that concrete solution, and otherwise return
  /// the value unchanged.
  int8_t klee_to_unique8(int8_t n);
  int32_t klee_to_unique32(int32_t n);
  int64_t klee_to_unique64(int64_t n);

  /// Returns an expression equivalent to loading the value at address
  /// (base + offset).  Does not add bounds-check constraints to the
  /// path constraint (which an ordinary load would do).
  int8_t klee_load8_no_boundscheck(void *base, size_t offset);
  int32_t klee_load32_no_boundscheck(void *base, size_t offset);
  int64_t klee_load64_no_boundscheck(void *base, size_t offset);


  //////////////////////////////////////////////////////////////////////////////
  // Memory Configuration
  //////////////////////////////////////////////////////////////////////////////

  /// Ensure that memory in the range [address, address+size) is
  /// accessible to the program. If some byte in the range is not
  /// accessible an error will be generated and the state terminated. 
  /// REQUIRES: the address and size are constants
  /// NOTE: The above requirement makes this function less useful than it should be
  void klee_check_memory_access(const void *address, size_t size);

  /// Marks a private memory object shared between processes (if they know the address)
  /// REQUIRES: the address and size are constants
  void klee_make_shared(void *addr, size_t nbytes);

  /// For internal usage: set MemoryObject.isGlobal to true
  /// REQUIRES: the address is a constant
  void klee_mark_global(void *object);
  
  /// Add an accesible memory object at a user specified location. It is the
  /// user's responsibility to make sure that these memory objects do not overlap.
  /// These memory objects will also (obviously) not correctly interact with external
  /// function calls.
  /// REQUIRES: the address and size are constants
  void klee_define_fixed_object(void *addr, size_t nbytes);


  //////////////////////////////////////////////////////////////////////////////
  // Execution Control
  //////////////////////////////////////////////////////////////////////////////

  /// klee_is_running - Returns true (non-zero) inside klee's virtual machine
  /// and false (zero) outside of klee's virtual machine (i.e., in actual
  /// bare-metal execution).  Use this to instrument a program s.t. certain
  /// code runs (or doesn't run) inside klee only.
  unsigned klee_is_running(void);

  /// klee_silent_exit - Terminate the current path being executed by KLEE.
  /// Reports a generic error and does not generate a test file.
  __attribute__((noreturn))
  void klee_silent_exit(int status);

  /// klee_abort - Terminate the current path being executed by KLEE.
  /// Like klee_report_error(), but reports a generic error message.
  __attribute__((noreturn))
  void klee_abort(void);

  /// klee_report_error - Terminate the current path being executed by KLEE.
  /// Reports a user defined error.
  ///
  /// \arg file - The filename to report in the error message.
  /// \arg line - The line number to report in the error message.
  /// \arg message - A string to include in the error message.
  /// \arg suffix - The suffix to use for error files.
  __attribute__((noreturn))
  void klee_report_error(const char *file,
                         int line,
                         const char *message,
                         const char *suffix);

  /// Add a condition to the current path constraint
  /// REQUIRES: the condition may-be-true on the current path
  void klee_assume(uintptr_t condition);

  /// Inject an event into the execution (see KLEE_EVENT constants in Constants.h)
  /// REQUIRES: arguments are constants
  void klee_event(enum EventClass type, long int value);

  /// Set execution options
  /// REQUIRES: arguments are constants
  void klee_set_option(enum KleeExecOption option, unsigned enable);

  /// Retrieves execution options
  /// REQUIRES: arguments are constants
  unsigned klee_get_option(enum KleeExecOption option);

  /// For backwards compatibility
  static inline void klee_set_forking(unsigned enable) {
    klee_set_option(KLEE_OPTION_FORKING_ENABLED, enable);
  }

  /// Cause the symbolic engine to fork for the given reaon (see KLEE_FORK
  /// constants in Constants.h).  Returns "0" in one forked state and "1"
  /// in the other state.
  /// REQUIRES: arguments are constants
  int klee_fork(int reason);

  /// Cause multiple symbolic paths to merge
  /// XXX: currently not implemented
  void klee_merge(void);

  /// klee_alias_function("foo", "bar") will replace, at runtime (on
  /// the current path and all paths spawned on the current path), all
  /// calls to foo() by calls to bar().  foo() and bar() have to exist
  /// and have identical types.  Use klee_alias_function("foo", "foo")
  /// to undo.  Be aware that some special functions, such as exit(),
  /// may not always work.
  /// REQUIRES: arguments are constants
  void klee_alias_function(const char* fn_name, const char* new_fn_name);

  // For input-covering schedules
  void klee_ics_begin(void);
  void klee_ics_region_marker(void);
  int klee_ics_once_per_region(void);

  /// This is used to initialize fields of model objects that must be
  /// concrete.  For such objects, we want to fork N+1 ways:
  ///   * N ways for each value (*addr1, *addr2, ..., *addrN), where each
  ///     "addrI" was previously initialized and may-alias "addr".
  ///   * +1 way for a new value
  ///
  /// The canonical example is the "last_hbnode" fieds of sync objects, e.g.,
  /// the last_hbnode of a mutex.  In this case, we want to build all possible
  /// HB graphs, so we want to fork for all possible values of last_hbnode.
  /// These are the first N cases -- the final "+1" case covers the possibility
  /// that *addr has not yet been initialized.
  ///
  /// RETURNS: see the definition of "enum InitState"
  /// REQUIRES: val == *addr
  enum InitState klee_fork_for_concrete_initialization(uint64_t type, void *addr, uint64_t val);


  //////////////////////////////////////////////////////////////////////////////
  // LibC Stand-Ins
  //////////////////////////////////////////////////////////////////////////////

  /// Special klee assert macro. This assert should be used when path consistency
  /// across platforms is desired (e.g., in tests).
#define klee_assert(expr) \
  ((expr) ? (void)0       \
          : klee_assert_fail (#expr, __FILE__, __LINE__, __PRETTY_FUNCTION__))

  /// Stand-in for __assert (POSIX) and assert_fail (glibc)
  __attribute__((noreturn))
  extern void klee_assert_fail(const char *assertion,
                               const char *file,
                               unsigned int line,
                               const char *function);

  /// Stand-in for malloc, calloc, realloc, etc.
  ///
  /// \arg size - Size of the object to allocate
  /// \arg oldptr - If non-null, this is a realloc
  /// \arg zeroinit - If non-zero, the new memory is memset to 0
  /// \arg alignment - The returned ptr is aligned to this boundary
  /// REQUIRES: arg "zeroinit" must be a constant expression
  void* klee_malloc(size_t size, void *oldptr, unsigned zeroinit, size_t alignment);

  /// Stand-in for free
  void klee_free(void *ptr);

  /// Stand-in for mem fns
  void klee_memset(void *ptr, char c, size_t nbytes);
  void klee_memcpy(void *dst, const void *src, size_t nbytes);
  void klee_memmove(void *dst, const void *src, size_t nbytes);

  /// Stand-in for string fns
  /// For strdup: if nbytes == constant(-1), then this is strdup(), else it is strndup()
  char* klee_strdup(const void *src, size_t nbytes);

  /// Stand-in for time
  uint64_t klee_get_time(void);
  void klee_set_time(uint64_t value);

  /// Retrieves errno for the klee intrepreter process
  /// Used by model code to forward errno after making a concrete syscall
  int klee_get_errno(void);

  /// Direct implementation of syscall()
  long int syscall(long int syscallno, ...);


  //////////////////////////////////////////////////////////////////////////////
  // Thread Scheduling Management
  //////////////////////////////////////////////////////////////////////////////

  void klee_thread_terminate(void) __attribute__ ((__noreturn__));
  void klee_process_terminate(void) __attribute__ ((__noreturn__));

  void klee_thread_create(uint32_t concrete_tid, void *(*start_routine)(void*), void *arg);
  int klee_process_fork(uint32_t pid);

  uint64_t klee_get_wlist(void);

  /*
   * For internal purposes, sometimes we need to translate between the
   * symbolic thread id that is exposed to the user, and the concrete
   * thread id that is used internally in the symbolic engine.
   *
   * The concrete --> symbolic conversion is used for pthread_create, where
   * our runtime decides on a concrete idx and the symbolic engine returns
   * the corresponding symbolic id.
   *
   * CAREFUL: function in this API that takes a "tid" might take a symbolic
   * or concrete "tid".  The parameters are named appropriately.
   *
   * N.B.: The symbolic ids do not have an explicit concretization. E.g.,
   * if x = klee_get_symbolic_thread_context_for(1), it is not necessarily
   * true that x == 1.
   *
   * N.B.: These are uint64_t since we use uint64_t for pthread_t
   *
   */
  uint64_t klee_get_symbolic_thread_context(void);
  uint64_t klee_get_symbolic_thread_context_for(uint64_t concrete_tid);
  uint64_t klee_get_concrete_thread_context(void);
  uint64_t klee_get_concrete_thread_context_for(uint64_t symbolic_tid);

  void klee_thread_preempt(int yield);
  void klee_thread_sleep(uint64_t wlist, void *wlist_addr, uint64_t wlist_type);
  void klee_thread_notify(uint64_t wlist, void *wlist_addr, uint64_t wlist_type, int all,
                          uint64_t hbedge_source_node,
                          uint64_t hbedge_sink_node,
                          int64_t expected_queue_size, int ics_region_marker);
  /*
   * Semantics of "expected_queue_size" for klee_thread_notify():
   * This is intended to tie a program expression with the number of threads on
   * a waiting list.  Ignored if == constant(-1).  See executeThreadNotifyAll()
   * for more details.
   */

  /*
   * Semantics of "ics_region_marker" for klee_thread_notify():
   * Immediately after all threads are woken, we invoke klee_ics_region_marker()
   * in the calling thread as well as in all notified threads.
   */

  /*
   * Semantics of hbedge creation for klee_thread_notify():
   *   (1) src=null, sink=null :: no edges created
   *   (2) src=A,    sink=null :: let T = {new node foreach notified thread}
   *                              forall t in T. make edge A->t
   *   (3) src=null, sink=A    :: let T = {new node foreach notified thread}
   *                              forall t in T. make edge t->A
   *   (4) src=A,    sink=A    :: let T0 = {new node foreach notified thread}
   *                              let T1 = {new node foreach notified thread}
   *                              forall t0 in T0. make edge t0->A
   *                              forall t1 in T1. make edge A->t1
   *   (5) src=A,    sink=B    :: let T = {new node foreach notified thread}
   *                              forall t in T. make edges A->t, t->B
   *
   * Use cases:
   *   (2) pthread_condvar_broadcast
   *   (4) pthread_barrier_wait (where "A" is a node that marks the barrier)
   */

  uint64_t klee_add_hbnode(void);
  uint64_t klee_last_hbnode(void);

  void klee_add_hbedge(uint64_t from, uint64_t to);
  void klee_add_hbedge_from_threads_last_hbnode(uint64_t symbolic_tid, uint64_t to_node);

  void klee_add_hbnode_to_hbgroup(uint64_t node, uint64_t group);
  void klee_add_hbedges_from_hbgroup(uint64_t groupFrom, uint64_t nodeTo);
  void klee_add_hbedges_to_hbgroup(uint64_t nodeFrom, uint64_t groupTo);
  void klee_remove_hbgroup(uint64_t group);

  /*
   * Enumerate via static analysis
   * Returns the maximum number of possible init counts (or -1 if "any").
   *   if 1 => we store the single possible init count to "*uniqueCount"
   *   else => "*uniqueCount" is unchanged and we emits constraints on "initCountField"
   */
  unsigned klee_enumerate_possible_barrier_init_counts(void *barrier_addr,
                                                       unsigned initCountField,
                                                       unsigned *uniqueCount);

  /*
   * Enumerate via static analysis
   * Returns the maximum number of possible lock owners(or -1 if "any").
   *   if 0 => we do nothing (caller is expected to assign "0" to the "taken" field)
   *   else => we emit constraints over the "taken" and "owner" fields
   */
  unsigned klee_enumerate_possible_lock_owners(void *lock_addr,
                                               unsigned takenField,
                                               unsigned ownersField);

  /*
   * Enumerate via dynamic alias analysis
   * The actual number of waiters, N, is constrained by mustCount <= N <= mayCount
   */
  void klee_enumerate_possible_waiters(uint64_t wlist_id, void *wlist_addr, uint64_t wlist_type,
                                       unsigned *mayCount,
                                       unsigned *mustCount);

  /* Enumerate threads for the current process */
  /* Caller's responsibility to zero these arrays before calling */
  void klee_enumerate_threads(char *islive, char *iszombie);

  /* Updating the current thread's locksets at runtime */
  void klee_lockset_add(void *lock_addr);
  void klee_lockset_remove(void *lock_addr);


  //////////////////////////////////////////////////////////////////////////////
  // XXX: Completely stupid
  // XXX: These are here for the same reason printf is in io.c (stupid aliasing sucks)
  //////////////////////////////////////////////////////////////////////////////

#undef putc
#undef putc_unlocked

#undef putchar
#undef putchar_unlocked

#undef fputc
#undef fputc_unlocked

#ifdef __cplusplus
}
#endif

#endif /* __KLEE_H__ */
