//===-- SymbolicObjectTable.h -----------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_SYMBOLICOBJECTTABLE_H
#define KLEE_SYMBOLICOBJECTTABLE_H

#include "klee/Expr.h"
#include "klee/Threading.h"
#include "klee/util/ExprHashMap.h"
#include "klee/util/Ref.h"
#include "klee/util/RefCounted.h"
#include "klee/Internal/ADT/ImmutableSet.h"
#include "klee/Internal/ADT/ImmutableMap.h"

#include "../lib/Core/Memory.h"

#include "llvm/GlobalVariable.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/EquivalenceClasses.h"

#include <map>
#include <vector>
#include <set>
#include <iostream>
#include <tr1/unordered_map>
#include <tr1/unordered_set>

namespace llvm {
class Argument;
class CallInst;
class Function;
class Instruction;
class Type;
class Value;
class raw_ostream;
}

namespace klee {
class Array;
class ExecutionState;
class MemoryObject;
class Thread;
class KInstruction;

//-----------------------------------------------------------------------------
// Describes a calling context as a list of return addresses
//-----------------------------------------------------------------------------

class CallingContext {
public:
  typedef llvm::SmallVector<KInstruction*,16> ListTy;

private:
  ListTy _callstack;
  uint64_t _id;

  // Global cache of contexts: we use this to cache canonical ptrs
  // so we can compare contexts with simple pointer equality.
  typedef std::set<CallingContext> CacheTy;
  static CacheTy Cache;
  static const CallingContext* FromThreadStackImpl(const Thread &thread, bool withpc);

public:
  // Constructs a ctx including the return addresses only
  // The returned pointer is guaranteed unique for the given context
  static const CallingContext* FromThreadStack(const Thread &thread) {
    return FromThreadStackImpl(thread, false);
  }

  // Constructs a ctx including the return addresses and the PC of the youngest frame
  // The returned pointer is guaranteed unique for the given context
  static const CallingContext* FromThreadStackWithPC(const Thread &thread) {
    return FromThreadStackImpl(thread, true);
  }

  // Constructs a new ctx directly using the given ctx list
  // The returned pointer is guaranteed unique
  static const CallingContext* FromList(const ListTy &list);

  // Constructs an empty ctx
  // The returned pointer is guaranteed unique
  static const CallingContext* Empty();

  // Constructs a ctx by prepending "older" before "younger"
  // The returned pointer is guaranteed unique
  static const CallingContext* Append(const CallingContext *older, const CallingContext *younger);

  // Constructs a ctx by prepending "older" before "youngest"
  // The returned pointer is guaranteed unique
  static const CallingContext* Append(const CallingContext *older, KInstruction *youngest);

  // Constructs a new ctx the same as this one, but with all calls younger
  // than "youngest" removed.  If "youngest" does not exist, this does nothing.
  // The returned pointer is guaranteed unique.
  const CallingContext* removeYoungerThan(KInstruction *youngest) const;

  // Constructs a new ctx the same as this one, but with all calls  to "pthreads"
  // functions removed.  The returned pointer is guaranteed unique.
  const CallingContext* removePthreadsFunctions() const;

  // As above, but removes uClibc pthreads wrapper functions.
  const CallingContext* removeUclibcWrapperFunctions() const;

  // As above, but removes any function matching the specified prefix
  const CallingContext* removeFunctionsMatchingPrefix(const char *prefix) const;

public:
  // A unique integer identifier
  // Ids are allocated from the sequence 1,2,3,..
  uint64_t id() const { return _id; }

  // Returns a string "F1.F2.F3..." for each function Fi in the context, oldest to youngest
  std::string shortName() const;

  // The oldest stack frames appear first
  typedef ListTy::const_iterator iterator;
  typedef ListTy::const_reverse_iterator reverse_iterator;

  iterator begin() const { return _callstack.begin(); }
  iterator end() const { return _callstack.end(); }

  reverse_iterator rbegin() const { return _callstack.rbegin(); }
  reverse_iterator rend() const { return _callstack.rend(); }

  size_t size() const { return _callstack.size(); }
  bool empty() const { return _callstack.empty(); }

  KInstruction* oldest() const { return _callstack.front(); }
  KInstruction* youngest() const { return _callstack.back(); }
  KInstruction* operator[](size_t i) const { return _callstack[i]; }

  // Users that obtain a CallingContext* via FromThreadStackImpl() can just compare pointers.
  // These are used internally for structural comparison
  bool operator<(const CallingContext& rhs)  const { return this->_callstack < rhs._callstack; }
  bool operator==(const CallingContext& rhs) const { return this->_callstack == rhs._callstack; }
  bool operator!=(const CallingContext& rhs) const { return !(*this == rhs); }
};

// Second parameter is a prefix string to print before each stackframe (e.g., indentation)
typedef std::pair<const CallingContext*, const char*> CallingContextPrinter;

// Instantiated for std::ostream and llvm::raw_ostream
template<class OStream> OStream& operator<<(OStream &os, CallingContextPrinter p);

//-----------------------------------------------------------------------------
// Describes a symbolic object and what value it represents
//-----------------------------------------------------------------------------

struct SymbolicObject {
  // An "Array" is a symbolic object.  This struct tags an "Array" with
  // metadata describing the object the "Array" represents, in terms of
  // LLVM source variables.  There are a few ways a symbolic object might
  // be created:
  //
  //   * For a virtual register, during dataflow analysis:
  //       array :: value of the virtual register
  //       inst  :: this is the virtual register
  //       callctx :: calling context, used to name a dynamic instance
  //       tuid  :: id of the thread this register belongs to
  //
  //   * For a function argument, during dataflow analysis:
  //       array :: value of the function argument
  //       arg   :: this is the argument ("arg" is non-null for this type only)
  //       callctx :: calling context, used to name a dynamic instance
  //       tuid  :: id of the thread this argument belongs to
  //
  //   * For a global variable
  //       array :: value of (*ptr)
  //       ptr   :: this is a pointer to the global
  //       mo    :: location of the global
  //
  //   * For a thread-local variable
  //       array :: value of (*ptr)
  //       ptr   :: this is a pointer to the global
  //       mo    :: location of the global
  //       tuid  :: id of the owning thread
  //
  //       [Note: ThreadLocalVariables are *exactly* like GlobalVariables except
  //        that (1) the "tuid" field is meaningful for thread-locals, and (2) it
  //        must be true that cast<GlobalVariable>(ptr)->isThreadLocal().  We separate
  //        thread-locals from globals because they are often handled differently.]
  //
  //   * By klee_make_symbolic() and friends, aka a "symbolic input"
  //       array :: value of the input
  //       inst  :: this is the call to klee_make_symbolic(), klee_int(), etc.
  //       ptr   :: this is arg0 with bitcasts stripped (klee_make_symbolic() only)
  //       mo    :: location of the object pointed to by "ptr", if that location is unique
  //                (used for debugging only)
  //
  //   * By symbolic pointer resolution:
  //       array :: value of (*ptr)
  //       ptr   :: this is the ptr that was resolved
  //       mo    :: location of the canonical object pointed to by "ptr"
  //
  //   * For an initial waiter's wait slot
  //       array :: position of the thread in its initial wait queue
  //       tuid  :: id of the thread
  //
  // Note that "kind" and "array" are always specified, and all other fields
  // are NULL except in the cases described above.  The "callctx" is used to
  // name a dynamic instance of a stack variable (but note that this is just
  // a calling context, so "dynamic instance" distinguishes different stack
  // frames but not different loop iterations -- within loops, our dynamic
  // instance always refers to the "most recent" loop iteration).
  //
  // TODO:
  // To instrument the program for constrained exec, we need to enumerate
  // all symbolic objects used by all input constraints, then add instrumentation
  // save the value of each object:
  //    * Virtual registers: save "inst" just after "inst"
  //    * memory allocators: save "ptr" just after "inst"
  //    * klee_make_symbolic: treat like symbolic ptr resolution?
  //      (where "ptr" is a root?)
  //    * symbolic ptr resolutions: need to save anything???
  //      (should be ok as long as we have the roots: can get the parent & offset
  //       via the SymbolicPtrResolution)
  //
  SymbolicObject()
    : kind((Kind)-1), array(NULL), arg(NULL), inst(NULL), callctx(NULL),
      ptr(NULL), mo(NULL)
  {}

  // DEBUG: dump to stderr, one field per line, each line with the given prefix
  static void Dump(const char *prefix, const SymbolicObject *obj, bool labeled = true);

  // Return the type of the underlying object represented by the Array
  const llvm::Type* getObjectType() const;

  // Return the representative llvm::Value for this object
  const llvm::Value* getProgramValue() const;

  enum Kind {
    VirtualRegister,
    FunctionArgument,
    GlobalVariable,
    ThreadLocalVariable,
    ThreadId,
    SymbolicInput,
    SymbolicPtr,
    WaitSlot
  };

  Kind kind;
  const Array *array;
  const llvm::Argument *arg;
  const llvm::Instruction *inst;
  const CallingContext *callctx;
  ThreadUid tuid;

  // For GlobalVariable, SymbolicInput, SymbolicPtr
  const llvm::Value *ptr;
  const MemoryObject *mo;

  // DEBUG
  static std::string KindToString(Kind k);
};

//-----------------------------------------------------------------------------
// Describes a mapping from Arrays to SymbolicObjects
//-----------------------------------------------------------------------------

class SymbolicObjectTable {
private:
  // See SymbolicPtrResolutionTable for commentary on why these are ref<>s
  // The object table
  typedef std::tr1::unordered_map<const Array*, SymbolicObject> MapTy;
  ref< RefCounted<MapTy> > _objects;

  // The caches
  typedef std::pair<const llvm::Value*,
                    std::pair<const CallingContext*, ThreadUid> > ValueCacheKeyTy;
  typedef std::pair<const Array*, ref<Expr> > CachedValueTy;
  typedef llvm::DenseMap<ValueCacheKeyTy, CachedValueTy> ValueCacheTy;
  typedef llvm::DenseMap<ThreadUid, CachedValueTy> PerThreadCacheTy;

  ref< RefCounted<ValueCacheTy> > _cacheForValues;
  ref< RefCounted<PerThreadCacheTy> > _cacheForThreadIds;
  ref< RefCounted<PerThreadCacheTy> > _cacheForWaitSlots;

  // Cache for "input" symbolics keyed by "inst"
  typedef std::map<const llvm::Instruction*, std::vector<const SymbolicObject*> > InputCacheTy;
  ref< RefCounted<InputCacheTy> > _cacheForInputs;

  // The state we are embedded in
  ExecutionState *_state;

private:
  static ValueCacheKeyTy MakeKeyForLocal(const llvm::Value *v, const CallingContext *ctx,
                                         const ThreadUid tuid) {
    return ValueCacheKeyTy(v, std::make_pair(ctx, tuid));
  }
  static ValueCacheKeyTy MakeKeyForThreadLocal(const llvm::GlobalVariable *v, ThreadUid tuid) {
    return ValueCacheKeyTy(v, std::make_pair((const CallingContext*)NULL, tuid));
  }
  static ValueCacheKeyTy MakeKeyForGlobal(const llvm::GlobalVariable *v) {
    return ValueCacheKeyTy(v, std::make_pair((const CallingContext*)NULL, ThreadUid()));
  }

  ref<Expr> getPerThreadExpr(PerThreadCacheTy *cache,
                             const Array* (SymbolicObjectTable::*addFor)(const ThreadUid),
                             const ThreadUid tuid,
                             const Expr::Width width);

public:
  explicit SymbolicObjectTable(ExecutionState &state)
    : _objects(new RefCounted<MapTy>()),
      _cacheForValues(new RefCounted<ValueCacheTy>()),
      _cacheForThreadIds(new RefCounted<PerThreadCacheTy>()),
      _cacheForWaitSlots(new RefCounted<PerThreadCacheTy>()),
      _cacheForInputs(new RefCounted<InputCacheTy>()),
      _state(&state)
  {}

  bool empty() const { return _objects->empty(); }
  size_t size() const { return _objects->size(); }

  // Called from ExecutionState::branch()
  void setExecutionState(ExecutionState &state) { _state = &state; }

  //
  // getUniqueArray()
  //   Returns a unique array for the given tuple.  If the tuple
  //   hasn't been seen, we construct a new array using add() and
  //   then cache the result for future calls.
  //
  // getExpr()
  //   Shorthand for calling getUniqueArray() followed by
  //   Expr::createArrayRead().
  //
  // lookupExpr()
  //   Like getExpr(), but returns NULL if an array has not yet
  //   been created for the given tuple.
  //

  // For VirtualRegister or FunctionArgument
  const Array* getUniqueArrayForLocalVar(const CallingContext *callctx,
                                         const ThreadUid tuid,
                                         const llvm::Value *val,
                                         const char *name = 0);

  ref<Expr> getExprForLocalVar(const CallingContext *callctx,
                               const ThreadUid tuid,
                               const llvm::Value *val,
                               const char *name = 0);

  ref<Expr> lookupExprForLocalVar(const CallingContext *callctx,
                                  const ThreadUid tuid,
                                  const llvm::Value *val) const;

  const Array* getUniqueArrayForGlobalVar(const llvm::GlobalVariable *ptr,
                                          const MemoryObject *mo,
                                          const char *name = 0);

  const Array* getUniqueArrayForThreadLocalVar(const llvm::GlobalVariable *ptr,
                                               const MemoryObject *mo,
                                               const ThreadUid tuid,
                                               const char *name = 0);

  // For ThreadId, WaitSlot
  ref<Expr> getExprForThreadId(const ThreadUid tuid);
  ref<Expr> getExprForWaitSlot(const ThreadUid tuid);

  // For ThreadId
  bool getThreadIdForExpr(ref<Expr> expr, ThreadUid *tuid);

  // For VirtualRegister, FunctionArgument, ThreadId, or WaitSlot
  ref<Expr> lookupExprForObj(const SymbolicObject *obj) const;

  //
  // add()
  //   These allocate a new array and a new symbolic object,
  //   which are both stored in the object table.  The new
  //   array is returned, and the new symbolic object can be
  //   retrieved via find().
  //
  // N.B.: these do NOT use the caches!
  //

  const Array* addForVirtualRegister(const CallingContext *callctx,
                                     const ThreadUid tuid,
                                     const llvm::Instruction *inst,
                                     const char *name = 0);

  const Array* addForFunctionArgument(const CallingContext *callctx,
                                      const ThreadUid tuid,
                                      const llvm::Argument *arg,
                                      const char *name = 0);

  const Array* addForGlobalVariable(const llvm::GlobalVariable *ptr,
                                    const MemoryObject *mo,
                                    const char *name = 0);

  const Array* addForThreadLocalVariable(const llvm::GlobalVariable *ptr,
                                         const MemoryObject *mo,
                                         const ThreadUid tuid,
                                         const char *name = 0);

  const Array* addForThreadId(const ThreadUid tuid);

  // "inst" must be a call to klee_make_symbolic()
  const Array* addForSymbolicInput(const llvm::Instruction *inst,
                                   const MemoryObject *mo,
                                   const char *name = 0);

  // "ptr" is being resolved
  // This returns an array for the ptr's primary object
  const Array* addForSymbolicPtr(const llvm::Value *ptr,
                                 const MemoryObject *mo,
                                 const char *name = 0);

  // "thread" that is an initial waiter
  const Array* addForWaitSlot(const ThreadUid tuid);

  //
  // Lookup metadata for an array
  // Returns NULL if not found
  //

  const SymbolicObject* find(const Array* array) const;
  // WARNING: this method is SLOW
  const SymbolicObject* find(const MemoryObject* mo) const;

  //
  // Iterators
  //

  typedef MapTy::const_iterator iterator;

  iterator begin() const { return _objects->begin(); }
  iterator end() const { return _objects->end(); }

  //
  // Look for a symbolic input that matches the given instruction but
  // is not included in the given set.  Returns NULL iff not found.
  //

  const SymbolicObject*
  findUnusedInputAtInst(const llvm::Instruction *inst,
                        const ImmutableSet<const SymbolicObject*> used) const {
    InputCacheTy::iterator it = _cacheForInputs->find(inst);
    if (it == _cacheForInputs->end())
      return NULL;
    for (size_t k = 0; k < it->second.size(); ++k) {
      if (!used.count(it->second[k]))
        return it->second[k];
    }
    return NULL;
  }
};

//-----------------------------------------------------------------------------
// Describes a symbolic pointer resolution
//-----------------------------------------------------------------------------


class SymbolicPtrResolution {
public:
  //
  // pointer
  //   * This names the LLVM value that was dereferenced
  //
  // targetAddress
  //   * A symbolic expression that evaluates to the value of the ptr at the
  //     time it was dereferenced.  This should satisfy Expr::isSymbolicRead().
  //
  // targetObj
  //   * For function ptrs, this is null
  //   * For data ptrs, this is the primary object allocated for the
  //     symbolic ptr, and it should hold that targetObj->ptr == pointer.
  //
  const llvm::Value *pointer;
  ref<Expr> targetAddress;
  const SymbolicObject *targetObj;

  //
  // uniqueSourceObj,
  // uniqueSourceOffset,
  //   * For the common case, this represents the object that we dereferenced,
  //     as computed by Expr::hasOrderedReads(targetAddress).  In psuedo bitcode:
  //     %targetAddress = ld (%uniqueSourceObj + %uniqueSourceOffset).
  //     CAREFUL: uniqueSourceOffset is always 32-bit
  //
  //   * In the uncommong case, hasOrderedReads() fails and these are left at
  //     NULL.  This can happen, e.g., if targetAddress is an interleaving of
  //     unordered reads, or of reads from different Arrays.  Mostly these fields
  //     are used for debugging.
  //
  const SymbolicObject *uniqueSourceObj;
  ref<Expr> uniqueSourceOffset;

public:
  SymbolicPtrResolution()
    : pointer(NULL), targetObj(NULL), uniqueSourceObj(NULL), refCount(0)
  {}

  bool isDataPtr() const { return targetObj != NULL; }
  bool isFunctionPtr() const { return !isDataPtr(); }

  // DEBUG: dump to stderr, one field per line, each line with the given prefix
  static void Dump(const char *prefix, const SymbolicPtrResolution *ptr);

private:
  unsigned refCount;
  friend class ref<SymbolicPtrResolution>;
};

//-----------------------------------------------------------------------------
// Describes a set of symbolic pointer resolutions
//-----------------------------------------------------------------------------

class SymbolicPtrResolutionTable {
private:
  // Intended usage:
  // You will be exploring multiple paths, and across this set of paths you
  // want to maintain a fixed set of symbolic objects.  So, this table includes
  // two classes of information:
  //
  //   * Shared info, which is shared by all paths.  This includes the primary
  //     resolutions of each symbolic ptr.  The idea is to share these resolutions
  //     across paths so that each unique symbolic ptr is paired with a unique
  //     Array and MemoryObject.  There is also a shared AddressPool from which
  //     we allocate those MemoryObjects.
  //
  //   * Per-Path info, which is unique to each path.  This includes the sets
  //     of secondary aliases.  These change per-path because they include only
  //     those objects that were allocated on the current path.
  //
  // This struct is embedded in an ExecutionState, which means that our
  // copy-constructor must fork the per-path info without changing the shared
  // info (hence the use of ref<>s for the shared info).  Use clearPathState()
  // to reset per-path state.  Shared state cannot be cleared, as the intention
  // is to keep it around for all paths.
  //
  // TODO: We indirectly depend on two other bits of shared state: a
  // SymbolicObjectTable and an AddressPool.  These are obtained indirectly
  // via the provided ExecutionState (specifically, via state.symbolicObjects
  // and state.symbolicAddressPool).

  // ===== Shared State =====
  //
  // This is the set of primary resolutions for each resolved pointer.
  // This is really a map of:
  //    (SourceArray,TargetAddress) -> SymbolicPtrResolution
  //
  // But the following is easier to search through, as we sometimes need to
  // iterate over all pairs (Array,*)
  //
  // TODO: In this primaries map, we compare TargetAddresses via structural equality.
  // A more powerful idea is to add a per-path notion of address-expr equivalence
  // classes, where two addresses are in the same class if they must-equal on the
  // current path.  Note that those equivalence classes must be per-path because
  // they might differ across paths, depending on the path constraints.
  // XXX: Future implementation note: we'd implement this by pairing sets of exprs
  // with a canonical leader expr (where the leader is used to index into the
  // primaries table), but note we couldn't use llvm::EquivalenceClasses for this
  // because EquivalenceClasses doesn't guarantee stability of leaders.  Also, if
  // we do this, we'd have to be careful to deterministically select the leader so
  // that input path constraints are valid from run-to-run.
  // XXX: A test case for this is symbolic-ptr-tree-2
  //
  typedef std::multimap<const Array*, ref<SymbolicPtrResolution> > PrimaryMapTy;
  ref< RefCounted<PrimaryMapTy> > _primaries;

  // InverseMap: Resolution.targetObj.array ==> Resolution
  typedef llvm::DenseMap<const Array*, ref<SymbolicPtrResolution> > PrimaryByTargetArrayMapTy;
  ref< RefCounted<PrimaryByTargetArrayMapTy> > _primariesByTargetArray;

  // ===== Per-Path State =====
  // The equivalence classes of memory objects that symbolic pointers might resolve to.
  // See comments in Executor::resolveSymbolicPointer().
  typedef llvm::EquivalenceClasses<const MemoryObject*> SecondaryAliasSetsTy;
  SecondaryAliasSetsTy _aliasSets;

  // The sets of functions that a fn ptr might resolve to.
  // Used to remember the set of function ptrs already resolved on this path.
  typedef ExprHashMap< std::set<const llvm::Function*> > FunctionPtrSetsTy;
  FunctionPtrSetsTy _functionPtrSets;

  // Parent ExecutionState
  ExecutionState *_state;

public:
  explicit SymbolicPtrResolutionTable(ExecutionState &state)
    : _primaries(new RefCounted<PrimaryMapTy>()),
      _primariesByTargetArray(new RefCounted<PrimaryByTargetArrayMapTy>()),
      _state(&state)
  {}

  void clearPathState();

  bool empty() const {
    return _primaries->empty() && _aliasSets.empty() && _functionPtrSets.empty();
  }

  // Called from ExecutionState::branch()
  void setExecutionState(ExecutionState &state) { _state = &state; }

  //
  // Primaries
  //

  // Call to construct the primary resolution of a symbolic ptr.
  // REQUIRES: a primary does not yet exist for targetAddress
  // REQUIRES: targetSize == NULL iff pointer is a function ptr
  ref<SymbolicPtrResolution> addPrimary(const llvm::Value *pointer,
                                        ref<Expr> targetAddress,
                                        ref<Expr> targetSize,
                                        const unsigned flags = MemoryObject::Heap,
                                        const size_t alignment = 0);

  // Returns NULL iff it a primary not yet been created for the given target
  ref<SymbolicPtrResolution> lookupPrimary(ref<Expr> targetAddress) const;
  ref<SymbolicPtrResolution> lookupPrimaryByTargetObj(const SymbolicObject *targetObj) const;

  //
  // Secondaries
  //

  void addSecondary(const MemoryObject *a) {
    assert(a);
    _aliasSets.insert(a);
  }

  void addSecondary(const MemoryObject *a, const MemoryObject *b) {
    assert(a);
    assert(b);
    _aliasSets.unionSets(a, b);
  }

  // Iterate over an alias equivalence class
  typedef SecondaryAliasSetsTy::member_iterator alias_iterator;

  alias_iterator aliasBegin(const MemoryObject *alias) {
    return _aliasSets.findLeader(alias);
  }
  alias_iterator aliasEnd() {
    return _aliasSets.member_end();
  }

  // Has "alias" been added yet?
  bool aliasExists(const MemoryObject *alias) {
    return _aliasSets.findLeader(alias) != _aliasSets.member_end();
  }

  // How many "aliases" does the object have?
  size_t aliasCount(const MemoryObject *mo) {
    size_t count = 0;
    for (alias_iterator it = aliasBegin(mo); it != aliasEnd(); ++it) {
      if (*it != mo) ++count;
    }
    return count;
  }

  // Do the two objects alias?
  bool aliasMatch(const MemoryObject *a, const MemoryObject *b) {
    alias_iterator ai = _aliasSets.findLeader(a);
    alias_iterator bi = _aliasSets.findLeader(b);
    return ai == bi && ai != _aliasSets.member_end();
  }

  // Cache the set of ptr -> f pairings
  void addFunctionTarget(ref<Expr> ptr, const llvm::Function *f) {
    assert(f);
    _functionPtrSets[ptr].insert(f);
  }

  bool hasFunctionTargets(ref<Expr> ptr) {
    return _functionPtrSets.count(ptr) != 0;
  }

  typedef std::set<const llvm::Function*> FunctionPtrSetTy;
  const FunctionPtrSetTy& getFunctionTargets(ref<Expr> ptr) {
    assert(_functionPtrSets.count(ptr));
    return _functionPtrSets[ptr];
  }

  // Have we resolved this pointer on the current path?
  bool alreadyResolvedOnPath(ref<SymbolicPtrResolution> res) {
    if (!res.get()) {
      return false;
    } else if (res->isFunctionPtr()) {
      return _functionPtrSets.count(res->targetAddress);
    } else {
      return aliasExists(res->targetObj->mo);
    }
  }
};


}  // namespace klee

#endif
