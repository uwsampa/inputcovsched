//===-- ThreadingUtil.h -----------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_THREADINGUTIL_H
#define KLEE_THREADINGUTIL_H

#include "klee/Threading.h"
#include "llvm/ADT/DenseMapInfo.h"

namespace klee {
  inline bool operator==(const ThreadUid left, const ThreadUid right) {
    return left.tid == right.tid && left.pid == right.pid;
  }

  inline bool operator!=(const ThreadUid left, const ThreadUid right) {
    return !(left == right);
  }

  inline bool operator<(const ThreadUid left, const ThreadUid right) {
    if (left.tid < right.tid)
      return true;
    if (right.tid < left.tid)
      return false;
    return left.pid < right.pid;
  }

  template<class OStream>
  inline OStream& operator<<(OStream &os, const ThreadUid tuid) {
    return os << "<" << tuid.tid << "," << tuid.pid << ">";
  }
}  // namespace klee

namespace llvm {
  // Modelled on DenseMapInfo<std::pair<T,U>>
  // N.B.: This cannot represent tids or pids equal to -1 or -2
  template<>
  struct DenseMapInfo<klee::ThreadUid> {
    typedef std::pair<klee::thread_id_t, klee::process_id_t> PairTy;
    typedef DenseMapInfo<klee::thread_id_t> TidInfo;
    typedef DenseMapInfo<klee::process_id_t> PidInfo;

    static inline klee::ThreadUid getEmptyKey() {
      return klee::ThreadUid(TidInfo::getEmptyKey(),
                             PidInfo::getEmptyKey());
    }
    static inline klee::ThreadUid getTombstoneKey() {
      return klee::ThreadUid(TidInfo::getTombstoneKey(),
                             PidInfo::getTombstoneKey());
    }
    static unsigned getHashValue(const klee::ThreadUid &tuid) {
      return DenseMapInfo<PairTy>::getHashValue(PairTy(tuid.tid, tuid.pid));
    }
    static bool isEqual(const klee::ThreadUid &left, const klee::ThreadUid &right) {
      return left == right;
    }
  };
}  // namespace llvm

#endif
