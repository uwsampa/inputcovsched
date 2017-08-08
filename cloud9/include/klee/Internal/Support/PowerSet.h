/*
 * Compute the power set of a set
 * Author: Tom Bergan
 */

#ifndef POWERSET_H
#define POWERSET_H

#include <set>
#include <vector>

namespace klee {

template<class T>
void
ComputePowerSet(const std::set<T> &input, std::vector<std::set<T> > *powerset) {
  // Okay, so this might be cheating.  We only consider sets of size <= 12 since
  // the power set has size 2^n and we don't want to kill memory usage.  Thus, we
  // can use the standard bit-twiddling algorithm for enumerating the power set.
  // We first convert the set to a vector so we have random access.
  assert(input.size() <= 12);
  std::vector<T> items(input.begin(), input.end());

  size_t highMask = (1ull << input.size()) - 1;
  powerset->clear();
  powerset->resize(highMask+1);
 
  for (size_t mask = 0; mask <= highMask; ++mask) {
    std::set<T> &subset = (*powerset)[mask];
    for (size_t i = 0; i < items.size(); ++i) {
      if ((mask & (1ull << i)) != 0)
        subset.insert(items[i]);
    }
  }
}

}

#endif
