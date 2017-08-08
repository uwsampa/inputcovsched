//===- DataStructure.h - Build data structure graphs ------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Info about common standard c library functions
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_STDLIBINFO_H
#define LLVM_ANALYSIS_STDLIBINFO_H

#include "llvm/Intrinsics.h"
#include "llvm/ADT/StringRef.h"
#include <map>

namespace llvm {
class Module;
class Function;

class StdLibInfo {
public:
  enum { numOps = 10 };
  //
  // Structure: libAction
  //
  // Description:
  //  Describe how the DSGraph of a function should be built.  Note that for the
  //  boolean arrays of arity numOps, the first element is a flag describing the
  //  return value, and the remaining elements are flags describing the
  //  function's arguments.
  //
  struct LibAction {
    // The return value/arguments that should be marked read.
    bool read[numOps];

    // The return value/arguments that should be marked modified.
    bool write[numOps];

    // The return value/arguments that should be marked as heap.
    bool heap[numOps];

    // Flags whether the return value should be merged with all arguments.
    bool mergeNodes[numOps];

    // Flags whether the return value and arguments should be folded.
    bool collapse;

    // Flags whether the function implies a memory barrier (accesses cannot
    // be optimized across this barrier).  Used by DSAA::getModRefInfo, but
    // not used to build DSGraphs as it does not affect the points-to relation.
    bool memoryBarrier;

    // Specify whether this function is a thread creation function.  If not,
    // these are -1.  If so, these are the argument indices for the thread spawn
    // function and the argument pass to the thread spawn function.  Argument
    // indices start at 0.  For building DSGraphs, this adds a call to the spawn
    // function with the given argument.
    int threadFnArg;        // e.g., this is 2 for pthread_create
    int threadFnParamArg;   // e.g., this is 3 for pthread_create
  };

private:
  typedef std::map<const Function*, const LibAction*> FunctionMapTy;
  typedef std::map<Intrinsic::ID, const LibAction*> IntrinsicMapTy;

  Module *module;
  FunctionMapTy functionMap;
  IntrinsicMapTy intrinsicMap;

public:
  StdLibInfo() : module(NULL) {}
  ~StdLibInfo() {}

  // Call this before querying for getLibActionForFunction().
  void initialize(Module &M);

  // Returns the LibAction for the specified function, if it exists,
  // and otherwise returns NULL.
  const LibAction* getLibActionForFunction(const Function *F) const;

  // Returns the LibAction for the specified function, if it exists,
  // and otherwise returns NULL.  Note that this works even if this
  // class hasn't been initialized.
  const LibAction* getLibActionForFunctionName(StringRef name) const;
};


} // End llvm namespace

#endif
