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

#ifndef CONSTANTS_H_
#define CONSTANTS_H_

#ifndef KLEE_CONSTANTS_FROM_KLEE_H
namespace klee {
#endif

////////////////////////////////////////////////////////////////////////////////
// Event Types
////////////////////////////////////////////////////////////////////////////////

enum EventClass {
  KLEE_EVENT_BREAKPOINT = 10,
  // from runtime/POSIX/common.h
  KLEE_EVENT_PKT_FRAGMENT = 1337
};

////////////////////////////////////////////////////////////////////////////////
// Breakpoint Types
////////////////////////////////////////////////////////////////////////////////

#define KLEE_BRK_START_TRACING  42

////////////////////////////////////////////////////////////////////////////////
// Execution Options
////////////////////////////////////////////////////////////////////////////////

enum KleeExecOption {
  // Enables/disables forking of the symbolic execution.
  // When forking is disabled, one randomly-selected feasible path will be executed.
  KLEE_OPTION_FORKING_ENABLED,

  // Enables/disables the state.totalBranches counter.
  // Used to ignore some branches internal to the runtime.
  KLEE_OPTION_BRANCH_COUNTER_ENABLED
};

////////////////////////////////////////////////////////////////////////////////
// Return value of klee_fork_for_concrete_initialization()
////////////////////////////////////////////////////////////////////////////////

// InitStates:
//   0 => already initialized
//   1 => needs symbolic initialization (aliased ptr, so don't update last_hbnode)
//   2 => needs symbolic initialization (unaliased ptr, so set last_hbnode = 0)

enum InitState {
  AlreadyInit = 0,
  NeedsSymbolicInitAliased = 1,
  NeedsSymbolicInitUnique = 2
};

////////////////////////////////////////////////////////////////////////////////
// Fork Types
////////////////////////////////////////////////////////////////////////////////

// N.B.: If you change either of these, you also need to change runtime/POSIX/common.h
//   KLEE_FORK_DEFAULT
//   KLEE_FORK_FAULTINJ

enum ForkClass {
  KLEE_FORK_BRANCH             = 0,  // br or switch instructions
  KLEE_FORK_FAULTINJ           = 1,  // internal: fault injection
  KLEE_FORK_RESOLVE_FN_POINTER = 3,  // call instruction (with indirect fn ptr)
  KLEE_FORK_SCHEDULE           = 4,  // internal: thread scheduling
  KLEE_FORK_SCHEDULE_MULTI     = 5,  // internal: thread scheduling
  KLEE_FORK_INTERNAL           = 6,  // internal (generic)

  // Following are not actually used with fork(), meaning they don't induce a branch
  // ... for constraints added via klee_assume
  KLEE_FORK_FIRST_NON_BRANCHING = 20,
  KLEE_FORK_ASSUME = KLEE_FORK_FIRST_NON_BRANCHING,

  // ... for branches that were made unconditional after conversion to an assertion
  KLEE_FORK_CONVERTED_BRANCH,

  // ... for misc. invariants added internally
  KLEE_FORK_INVARIANT_INTERNAL,
  KLEE_FORK_INVARIANT_INTERNAL_HIDDEN,   // don't show in pathConstraints
  KLEE_FORK_INVARIANT_INPUT,

  // ... for constraints added for concretization (these imply incompleteness)
  KLEE_FORK_CONCRETIZE_FP,    // concretized a floating point number
  KLEE_FORK_CONCRETIZE_ADDR_ON_SOLVER_FAILURE,      // concretized a memory address
  KLEE_FORK_CONCRETIZE_ADDR_ON_BIG_SYMBOLIC_ARRAY,  // concretized a memory address

  // ... for constraints added to restrict pointer aliasing
  KLEE_FORK_POINTER_ALIASING,

  // ... for constraints added to bounds-check pointers
  KLEE_FORK_BOUNDS_CHECK,

  // Aliases
  KLEE_FORK_MAXCLASS,
  KLEE_FORK_DEFAULT = KLEE_FORK_BRANCH
};

#ifdef __cplusplus
inline bool IsBranchingForkClass(enum ForkClass fc) {
  return fc < KLEE_FORK_FIRST_NON_BRANCHING;
}

inline bool IsBranchingOrInputForkClass(enum ForkClass fc) {
  return IsBranchingForkClass(fc) || fc == KLEE_FORK_INVARIANT_INPUT;
}

inline bool IsNonBranchingForkClass(enum ForkClass fc) {
  return fc >= KLEE_FORK_FIRST_NON_BRANCHING;
}

inline bool IsConcretizationForkClass(enum ForkClass fc) {
  return fc >= KLEE_FORK_CONCRETIZE_FP
      && fc <= KLEE_FORK_CONCRETIZE_ADDR_ON_BIG_SYMBOLIC_ARRAY;
}
#endif

#ifndef KLEE_CONSTANTS_FROM_KLEE_H
}  // namespace klee
#endif


#endif /* CONSTANTS_H_ */
