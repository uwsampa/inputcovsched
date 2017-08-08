//===-- LLVMSupport.h -------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_UTIL_LLVMSUPPORT_H
#define KLEE_UTIL_LLVMSUPPORT_H

#include "llvm/Support/ValueHandle.h"

namespace llvm {

// ValueHandle like WeakVH except it does not track RAUW ops
class WeakNonMovingVH : public CallbackVH {
public:
  WeakNonMovingVH() : CallbackVH() {}
  WeakNonMovingVH(Value *P) : CallbackVH(P) {}

  Value *operator=(Value *RHS) {
    return ValueHandleBase::operator=(RHS);
  }
  Value *operator=(const ValueHandleBase &RHS) {
    return ValueHandleBase::operator=(RHS);
  }

  virtual void deleted() {
    setValPtr(NULL);
  }

  virtual void allUsesReplacedWith(Value *v) {}
};

// Specialize simplify_type to allow WeakNonMovingVH to participate in
// dyn_cast, isa, etc.
template<typename From> struct simplify_type;
template<> struct simplify_type<const WeakNonMovingVH> {
  typedef Value* SimpleType;
  static SimpleType getSimplifiedValue(const WeakNonMovingVH &WVH) {
    return static_cast<Value *>(WVH);
  }
};
template<> struct simplify_type<WeakNonMovingVH>
  : public simplify_type<const WeakNonMovingVH> {};

}

#endif
