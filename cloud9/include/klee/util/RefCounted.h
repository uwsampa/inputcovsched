//===-- RefCounted.h --------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_REFCOUNTED_H
#define KLEE_REFCOUNTED_H

#include "klee/util/Ref.h"

namespace klee {

// This is somewhat evil since T may not have a virtual destructor,
// but it's okay as long as long as you always delete the object through
// a RefCounted<T>* and never through a T*.  Note that ref<> always does
// the former, so if all your deletes go through ref<>, you should be safe).
template<class T>
class RefCounted : public T {
public:
  RefCounted()
    : T(), refCount(0)
  {}

private:
  unsigned refCount;
  friend class ref< RefCounted<T> >;

  // Not copyable
  RefCounted(const RefCounted &);
  RefCounted& operator=(const RefCounted &);
};

} // end namespace klee

#endif /* KLEE_REFCOUNTED_H */
