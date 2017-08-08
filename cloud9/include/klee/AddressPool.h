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

#ifndef ADDRESSPOOL_H_
#define ADDRESSPOOL_H_

#include <stdint.h>
#include <assert.h>

// XXX This may very well overlap a mmap()-ed region
// XXX We use tons of virtual address space, so this works only in a 64-bit virtual space
#define ADDRESS_POOL_DEFAULT_START  0xDEADBEEF00000000L
#define ADDRESS_POOL_DEFAULT_SIZE   0x10000000
#define ADDRESS_POOL_DEFAULT_GAP    (4*sizeof(uint64_t))
#define ADDRESS_POOL_DEFAULT_ALIGN  (4*sizeof(uint64_t)) // Must be a power of two

namespace klee {

class AddressPool {
private:
  bool _valid;
  uint64_t _startAddress;
  uint64_t _size;
  uint64_t _currentAddress;
  uint64_t _gap;
  uint64_t _defaultAlignment;  // Must be a power of two

public:
  AddressPool()
    : _valid(false),
      _startAddress(ADDRESS_POOL_DEFAULT_START),
      _size(ADDRESS_POOL_DEFAULT_SIZE),
      _currentAddress(_startAddress),
      _gap(ADDRESS_POOL_DEFAULT_GAP),
      _defaultAlignment(ADDRESS_POOL_DEFAULT_ALIGN)
  {}

  virtual ~AddressPool() {}

  // Use 0 for either parameter to select a default
  // This mmap()s a virtual memory backing region
  void initialize(uint64_t start=0, uint64_t size=0);

  // Returns 0 if OOM
  // REQUIRES: initialized
  uint64_t allocate(uint64_t amount, uint64_t alignment);

  // This resets the pool to empty, but does not release the mmap()ed backing
  // REQUIRES: initialized
  void clear();

  uint64_t getStartAddress() const {
    assert(_valid);
    return _startAddress;
  }

  uint64_t getSize() const {
    assert(_valid);
    return _size;
  }
};

}

#endif /* ADDRESSPOOL_H_ */
