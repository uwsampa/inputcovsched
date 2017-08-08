//===-- HashUtil.h ----------------------------------------------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#ifndef KLEE_STRINGUTIL_H
#define KLEE_STRINGUTIL_H

#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace klee {

struct StringMatcher {
  std::vector<llvm::StringRef> prefixes;
  std::vector<llvm::StringRef> exacts;

  static StringMatcher ForPrefix(llvm::StringRef prefix) {
    StringMatcher m;
    m.prefixes.push_back(prefix);
    return m;
  }

  static StringMatcher ForExact(llvm::StringRef exact) {
    StringMatcher m;
    m.exacts.push_back(exact);
    return m;
  }

  bool matches(llvm::StringRef name) const {
    for (size_t k = 0; k < prefixes.size(); ++k) {
      if (name.startswith(prefixes[k]))
        return true;
    }
    for (size_t k = 0; k < exacts.size(); ++k) {
      if (name == exacts[k])
        return true;
    }
    return false;
  }
};

}

#endif
