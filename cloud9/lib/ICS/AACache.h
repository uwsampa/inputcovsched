/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * A wrapper class for caching AliasAnalysis::alias results.
 *
 */

#ifndef ICS_AACACHE_H
#define ICS_AACACHE_H

#include "AARunner.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Instructions.h"
#include <tr1/unordered_set>

//-----------------------------------------------------------------------------
// Utils for the cache type
//-----------------------------------------------------------------------------

namespace std {
namespace tr1 {

template<class A, class B>
struct hash<std::pair<A,B> > : public unary_function<std::pair<A,B>, size_t> {
  size_t operator()(const std::pair<A,B> &p) const {
    return hash<A>()(p.first) ^ hash<B>()(p.second);
  }
};

template<>
struct hash<llvm::AliasAnalysis::Location> : public unary_function<llvm::AliasAnalysis::Location, size_t> {
  size_t operator()(const llvm::AliasAnalysis::Location &loc) const {
    return hash<const llvm::Value*>()(loc.Ptr) + loc.Size;
  }
};

}
}

namespace llvm {

bool operator==(const llvm::AliasAnalysis::Location &A,
                const llvm::AliasAnalysis::Location &B) {
  return A.Ptr  == B.Ptr
      && A.Size == B.Size;
}

}

//-----------------------------------------------------------------------------
// AliasAnalysis::alias cache
//-----------------------------------------------------------------------------

namespace ics {

class AACache {
private:
  typedef llvm::AliasAnalysis::AliasResult AliasResult;
  typedef llvm::AliasAnalysis::Location    Location;

  typedef std::pair<Location, Location>                      alias_key_t;
  typedef std::tr1::unordered_map<alias_key_t, AliasResult>  alias_cache_t;

  typedef std::pair<const llvm::Function*, Location>   modref_key_t;
  typedef std::tr1::unordered_map<modref_key_t, bool>  modref_cache_t;

  alias_cache_t  _aliasCache;
  modref_cache_t _modrefCache;

  llvm::AliasAnalysis *_AA;
  llvm::DSAA *_DSAA;

  struct Stats {
    size_t aliasCacheHit;
    size_t aliasCacheMiss;
    size_t modRefCacheHit;
    size_t modRefCacheHitTransitive;
    size_t modRefCacheMiss;
    size_t modRefCacheMissTransitive;
  } _stats;

public:
  AACache() : _AA(NULL), _DSAA(NULL) { resetStats(); }

  size_t nAliasCacheHit() const { return _stats.aliasCacheHit; }
  size_t nAliasCacheMiss() const { return _stats.aliasCacheMiss; }
  size_t nAliasCacheSize() const { return _aliasCache.size(); }

  size_t nModRefCacheHit() const { return _stats.modRefCacheHit; }
  size_t nModRefCacheMiss() const { return _stats.modRefCacheMiss; }
  size_t nModRefCacheSize() const { return _modrefCache.size(); }

  size_t nModRefCacheHitTransitive() const { return _stats.modRefCacheHitTransitive; }
  size_t nModRefCacheMissTransitive() const { return _stats.modRefCacheMissTransitive; }

  void setAA(AARunner *runner) {
    _aliasCache.clear();
    _modrefCache.clear();
    _AA = runner->getAA();
    _DSAA = runner->getDSAA();
    resetStats();
  }

  void resetStats() {
    memset(&_stats, 0, sizeof _stats);
  }

  AliasResult alias(Location A, Location B);
  bool functionMayWriteLocation(const llvm::Function* F, const Location &Loc);

private:
  bool functionMayWriteLocation_r(const llvm::Function* F, const Location &Loc,
                                  llvm::SmallPtrSet<const llvm::Function*,16> &visited);
};

//-----------------------------------------------------------------------------
// AliasAnalysis::alias cache implementation
//-----------------------------------------------------------------------------

AACache::AliasResult
AACache::alias(Location A, Location B) {
  assert(_AA);

  // canonicalize
  if (A.Ptr > B.Ptr) {
    std::swap(A, B);
  }

  // check cache
  alias_key_t key(A,B);
  alias_cache_t::const_iterator it = _aliasCache.find(key);
  if (it != _aliasCache.end()) {
    _stats.aliasCacheHit++;
    return it->second;
  }

  // compute
  AliasResult result = _AA->alias(A, B);
  _aliasCache.insert(std::make_pair(key, result));
  _stats.aliasCacheMiss++;
  return result;
}

// Two implementations
// First updates cache for functions call transitively
// Second updates cache only for the base function

#if 1

bool
AACache::functionMayWriteLocation_r(const llvm::Function* F, const Location &Loc,
                                    llvm::SmallPtrSet<const llvm::Function*,16> &visited) {
  assert(_AA);
  assert(_DSAA);

  std::vector<const llvm::Function*> calltargets;
  llvm::SmallVector<const llvm::Function*,16> callees;

  bool result = false;
  bool incomplete = false;

  // Quick dumb ugly check
  // We assume that the "ignored" functions write to a set of objects
  // that are not written or read by any non-"ignored" function.  Thus,
  // we will not consider the "ignored" functions here.
  // XXX: Think again: do we want/need this?
  if (_DSAA->isSteensIgnoredFunction(F)) {
    result = false;
    goto done;
  }

  // Gah, retarded: llvm::AliasAnalysis::getModRefInfo() requires a call site.
  // We can't make a dummy callsite, that would break analysis expectations.
  // Thus, we do it the dumb way (could be sped up with hooks into DSAA).
 
  // The dumb way: iterate over all instructions in the function, and also in
  // all instructions which the function may call.
  for (llvm::Function::const_iterator BB = F->begin(); BB != F->end(); ++BB) {
    for (llvm::BasicBlock::const_iterator I = BB->begin(); I != BB->end(); ++I) {
      switch (I->getOpcode()) {
      case llvm::Instruction::Call:
      case llvm::Instruction::Invoke: {
        // Need to check all possible call targets
        llvm::CallSite CS(const_cast<llvm::Instruction*>((const llvm::Instruction*)I));
        calltargets.clear();
        _DSAA->getCallTargets(CS, &calltargets);
        for (size_t i = 0; i < calltargets.size(); ++i) {
          const llvm::Function* Callee = calltargets[i];
          // TODO: We probably should be checking known externals
          if (Callee->isDeclaration())
            continue;
          // check cache
          modref_key_t key(Callee,Loc);
          modref_cache_t::const_iterator it = _modrefCache.find(key);
          if (it != _modrefCache.end()) {
            _stats.modRefCacheHitTransitive++;
            result = it->second;
            if (result)
              goto done;
          }
          _stats.modRefCacheMissTransitive++;
          // visited but not cached? incompleteness!
          // for example, this happens with recursive calls
          if (visited.count(Callee)) {
            incomplete = true;
          }
          // unvisited? need to search!
          else {
            visited.insert(Callee);
            callees.push_back(Callee);
          }
        }
        break;
      }

      case llvm::Instruction::Store: {
        // Check aliasing
        const llvm::StoreInst *st = cast<llvm::StoreInst>(I);
        Location storeLoc = _AA->getLocation(st);
        AliasResult res = alias(Loc, storeLoc);
        if (res != llvm::AliasAnalysis::NoAlias) {
          result = true;
          goto done;
        }
        break;
      }

      default:
        break;
      }
    }
  }

  // Recurse over callees
  for (llvm::SmallVector<const llvm::Function*,16>::const_iterator
       it = callees.begin(); it != callees.end(); ++it) {
    result = functionMayWriteLocation_r(*it, Loc, visited);
    if (result)
      goto done;
  }

done:
  // If we found no aliased writes, we cant cache this unless the search was complete.
  if (result || !incomplete) {
    modref_key_t key(F,Loc);
    _modrefCache.insert(std::make_pair(key, result));
  }
  return result;
}

bool
AACache::functionMayWriteLocation(const llvm::Function* F, const Location &Loc) {
  assert(_AA);

  // check cache
  modref_key_t key(F,Loc);
  modref_cache_t::const_iterator it = _modrefCache.find(key);
  if (it != _modrefCache.end()) {
    _stats.modRefCacheHit++;
    return it->second;
  }

  _stats.modRefCacheMiss++;

  // compute
  // We may be updating the cache twice here. Oh well!
  llvm::SmallPtrSet<const llvm::Function*,16> visited;
  visited.insert(F);

  bool result = functionMayWriteLocation_r(F, Loc, visited);
  _modrefCache.insert(std::make_pair(key, result));
  return result;
}

#else

bool
AACache::functionMayWriteLocation(const llvm::Function* OrigF, const Location &Loc) {
  assert(_AA);

  // check cache
  modref_key_t key(OrigF,Loc);
  modref_cache_t::const_iterator it = _modrefCache.find(key);
  if (it != _modrefCache.end()) {
    _stats.modRefCacheHit++;
    return it->second;
  }

  _stats.modRefCacheMiss++;

  // compute
  bool result = false;

  // Gah, retarded: llvm::AliasAnalysis::getModRefInfo() requires a call site.
  // We can't make a dummy callsite, that would break analysis expectations.
  // Thus, we do it the dumb way (could be sped up with hooks into DSAA).
 
  // The dumb way: iterate over all instructions in the function, and also in
  // all instructions which the function may call.  We could make this slightly
  // less dumb by caching for other functions as we go (e.g., by using SCCs).
  llvm::SmallPtrSet<const llvm::Function*,16> visited;
  llvm::SmallVector<const llvm::Function*,16> stack;
  std::vector<const llvm::Function*> calltargets;

  visited.insert(OrigF);
  stack.push_back(OrigF);

  while (!stack.empty()) {
    const llvm::Function *F = stack.back();
    stack.pop_back();

    for (llvm::Function::const_iterator BB = F->begin(); BB != F->end(); ++BB) {
      for (llvm::BasicBlock::const_iterator I = BB->begin(); I != BB->end(); ++I) {
        switch (I->getOpcode()) {
        case llvm::Instruction::Call:
        case llvm::Instruction::Invoke: {
          // Need to check all possible call targets
          llvm::CallSite CS(const_cast<llvm::Instruction*>((const llvm::Instruction*)I));
          _DSAA->getCallTargets(CS, &calltargets);
          for (size_t i = 0; i < calltargets.size(); ++i) {
            const llvm::Function* Callee = calltargets[i];
            if (visited.count(Callee))
              continue;
            visited.insert(Callee);
            // check cache
            modref_key_t key(Callee,Loc);
            modref_cache_t::const_iterator it = _modrefCache.find(key);
            if (it != _modrefCache.end()) {
              _stats.modRefCacheHitTransitive++;
              result = it->second;
              goto done;
            }
            _stats.modRefCacheMissTransitive++;
            // no cache hit: need to search Callee
            stack.push_back(Callee);
          }
          break;
        }

        case llvm::Instruction::Store: {
          // Check aliasing
          const llvm::StoreInst *st = cast<llvm::StoreInst>(I);
          if (st == NULL)
            continue;
          Location storeLoc = _AA->getLocation(st);
          AliasResult res = alias(Loc, storeLoc);
          if (res != llvm::AliasAnalysis::NoAlias) {
            result = true;
            goto done;
          }
          break;
        }

        default:
          break;
        }
      }
    }
  }

done:
  _modrefCache.insert(std::make_pair(key, result));
  return result;
}

#endif

}  // namespace ics

#endif
