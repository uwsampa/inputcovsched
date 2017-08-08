//===-- Updates.cpp -------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Expr.h"
#include "klee/util/ExprHashMap.h"
#include "klee/util/ExprVisitor.h"
#include "llvm/Support/CommandLine.h"

#include <cassert>
#include <tr1/functional>

using namespace klee;
extern "C" void vc_DeleteExpr(void*);

// this option is meant for testing code only
llvm::cl::opt<bool> UseStableArrayHashing("use-stable-array-hashing", llvm::cl::init(false));

//-----------------------------------------------------------------------------
// EqCache
//-----------------------------------------------------------------------------

// Kind of ugly: it sometimes happens that the same UpdateList gets
// generated in different states.  For example, we fork into two states,
// each of which eventually performs the same write to the same object.
// When this happens, we can end up comparing long UpdateLists over and
// over again.  Fortunately, there is good temporal locality of comparisons
// so we can speed this up with a cache.
struct EqCacheEntry {
  const UpdateNode *ahead;
  const UpdateNode *bhead;
};
static const int EqCacheSize = 8;
static EqCacheEntry EqCache[EqCacheSize];
static int EqCacheNextIdx;

//-----------------------------------------------------------------------------
// UpdateNode
//-----------------------------------------------------------------------------

UpdateNode::UpdateNode(const UpdateNode *n,
                       const UpdateNode &copy)
  : refCount(0),
    stpArray(NULL),
    _hashValue(copy._hashValue),
    _sequenceLength(copy._sequenceLength),
    _guard(copy._guard),
    _byteIndex(copy._byteIndex),
    _byteValue(copy._byteValue),
    _src(copy._src),
    next(n)
{
  if (next) ++next->refCount;
}

UpdateNode::UpdateNode(const UpdateNode *n,
                       const ref<Expr> &byteIndex,
                       const ref<Expr> &byteValue,
                       const ref<Expr> &guard)
  : refCount(0),
    stpArray(NULL),
    _guard(guard),
    _byteIndex(byteIndex),
    _byteValue(byteValue),
    _src(NULL),
    next(n)
{
  assert(byteValue.get());
  assert(byteIndex.get());
  assert(byteValue->getWidth() == Expr::Int8 && "Update value should be 8-bit wide.");
  assert(byteIndex->getWidth() == Expr::Int32 && "Update index should be 32-bit wide.");
  initialize();
}

UpdateNode::UpdateNode(const UpdateNode *n,
                       const Array *src,
                       const ref<Expr> &guard)
  : refCount(0),
    stpArray(NULL),
    _guard(guard),
    _src(src),
    next(n)
{
  assert(_src);
  assert(!isa<ConstantExpr>(_guard) && "guard must be non-const for multi-byte updates");
  initialize();
}

void UpdateNode::initialize() {
  // Sanity checks
  assert(_guard.get());
  assert((_byteIndex.get() == NULL) != (_src == NULL));
  assert((_byteIndex.get() == NULL) == (_byteValue.get() == NULL));

  // Common initialization
  computeHash();
  if (next) {
    ++next->refCount;
    _sequenceLength = 1 + next->_sequenceLength;
  } else {
    _sequenceLength = 1;
  }
}

UpdateNode::~UpdateNode() {
  // XXX gross
  if (stpArray)
    ::vc_DeleteExpr(stpArray);
  // XXX gross
  for (int i = 0; i < EqCacheSize; ++i) {
    if (EqCache[i].ahead == this || EqCache[i].bhead == this) {
      EqCache[i].ahead = NULL;
      EqCache[i].bhead = NULL;
      break;
    }
  }
}

ref<Expr> UpdateNode::getGuardForIndex(const ref<Expr> &idx) const {
  // Single-byte
  if (_byteIndex.get())
    return AndExpr::create(_guard, EqExpr::create(_byteIndex, idx));

  // Multi-byte
  assert(_src);
  return _guard;
}

ref<Expr> UpdateNode::getValueAtIndex(const ref<Expr> &idx,
                                      const bool expectIsSymbolicPtr /* = false */) const {
  // Single-byte
  if (_byteValue.get())
    return _byteValue;

  // Multi-byte
  assert(_src);
  return _src->getValueAtIndex(idx, expectIsSymbolicPtr);
}

ExprPair UpdateNode::extractMultiByteValue(const ref<Expr> &index,
                                           const unsigned totalBytes,
                                           const bool isLittleEndian) const {
  assert(totalBytes);
  assert(!isSingleByte() || index->getWidth() == _byteIndex->getWidth());

  // Can we read the entire value from this UpdateNode?
  if (totalBytes == 1) {
    return ExprPair(getValueAtIndex(index), getGuardForIndex(index));
  }
  if (isMultiByte()) {
    return ExprPair(Expr::createArrayReadAtIndex(_src, index, totalBytes*8), _guard);
  }

  // From here on: this must be a single-byte update
  // Must have enough "next" nodes to merge with
  unsigned avail = 1;
  for (const UpdateNode *n = next; n && n->isSingleByte() && avail < totalBytes; n = n->next) {
    avail++;
  }
  if (avail < totalBytes) {
    return ExprPair(NULL, NULL);
  }

  ExprPair res;
  unsigned curbyte = 0;

  // Test if "next" is adjacent to this UpdateNode
  if (EqExpr::create(next->_byteIndex,
                     AddExpr::create(_byteIndex, Expr::One32))->isTrue()) {
    // From this node, we read byte [0], and from
    // the next nodes, we read bytes [1, totalBytes).
    curbyte = 0;
    res = next->extractMultiByteValue(AddExpr::create(index, Expr::One32),
                                      totalBytes - 1,
                                      isLittleEndian);
  }
  else if (EqExpr::create(AddExpr::create(next->_byteIndex, Expr::One32),
                          _byteIndex)->isTrue()) {
    // From this node, we read byte [totalBytes-1], and from
    // the next nodes, we read bytes [0, totalBytes-1).
    curbyte = totalBytes - 1;
    res = next->extractMultiByteValue(index,
                                      totalBytes - 1,
                                      isLittleEndian);
  }

  // Not adjacent: we can't merge
  if (!res.first.get()) {
    return ExprPair(NULL, NULL);
  }

  // Merge our local value with the chained value
  // Big-Endian:     (Concat read[0] read[1] read[2] read[3])
  // Little-Endian:  (Concat read[3] read[2] read[1] read[0])
  if ((isLittleEndian && curbyte == 0) || (!isLittleEndian && curbyte != 0)) {
    res.first = ConcatExpr::create(res.first, _byteValue);
  } else {
    res.first = ConcatExpr::create(_byteValue, res.first);
  }

  // Merge guards
  assert(res.second.get());
  res.second = AndExpr::create(_guard, res.second);

  return res;
}

unsigned UpdateNode::computeHash() {
  _hashValue = _guard->hash();
  if (_byteIndex.get()) _hashValue ^= _byteIndex->hash();
  if (_byteValue.get()) _hashValue ^= _byteValue->hash();
  if (_src) _hashValue ^= (unsigned)(uintptr_t)_src;
  return _hashValue;
}

int UpdateNode::compareContents(const UpdateNode *bn) const {
  const UpdateNode *an = this;

  if (an->hash() != bn->hash())
    return an->hash() < bn->hash() ? -1 : 1;

  if (an->_src != bn->_src)
    return an->_src < bn->_src ? -1 : 1;

  if (int res = an->_byteIndex.compareAllowNull(bn->_byteIndex))
    return res;

  if (int res = an->_byteValue.compareAllowNull(bn->_byteValue))
    return res;

  if (int res = an->_guard.compare(bn->_guard))
    return res;

  return 0;
}

//-----------------------------------------------------------------------------
// UpdateList
//-----------------------------------------------------------------------------

UpdateList::UpdateList(const Array *_root, const UpdateNode *_head)
  : root(_root),
    head(_head) {
  if (head) ++head->refCount;
}

UpdateList::UpdateList(const UpdateList &b)
  : root(b.root),
    head(b.head) {
  if (head) ++head->refCount;
}

UpdateList::~UpdateList() {
  deleteNodes();
}

void UpdateList::deleteNodes() {
  // We need to be careful and avoid recursion here. We do this in
  // cooperation with the private dtor of UpdateNode which does not
  // recursively free its tail.
  while (head && --head->refCount==0) {
    const UpdateNode *n = head->next;
    delete head;
    head = n;
  }
}

UpdateList &UpdateList::operator=(const UpdateList &b) {
  // Inc b refcnt first in case b.head == this.head
  if (b.head)
    ++b.head->refCount;
  // Drop the old list
  deleteNodes();
  // Copy
  root = b.root;
  head = b.head;
  return *this;
}

unsigned UpdateList::getSize() const {
  return head ? head->getSize() : 0;
}

void UpdateList::replaceHead(const UpdateNode *newhead) {
  if (head) --head->refCount;
  head = newhead;
  ++head->refCount;
}

void UpdateList::addUpdate(const UpdateNode &copy) {
  replaceHead(new UpdateNode(head, copy));
}

void UpdateList::addSingleByteUpdate(const ref<Expr> &index,
                                     const ref<Expr> &value,
                                     const ref<Expr> guard /*= Expr::True*/) {
  replaceHead(new UpdateNode(head, index, value, guard));
}

void UpdateList::addMultiByteUpdate(const Array *src,
                                    const ref<Expr> &guard) {
  replaceHead(new UpdateNode(head, src, guard));
}

int UpdateList::compare(const UpdateList &b) const {
  // Fast path
  if (int res = root->compare(b.root))
    return res;

  const UpdateNode *an = head;
  const UpdateNode *bn = b.head;

  if (an == bn)
    return 0;

  if (getSize() != b.getSize())
    return getSize() < b.getSize() ? -1 : 1;

  // Fast path: cached?
  for (int i = 0; i < EqCacheSize; ++i) {
    if ((EqCache[i].ahead == an && EqCache[i].bhead == bn) ||
        (EqCache[i].ahead == bn && EqCache[i].bhead == an)) {
      return 0;
    }
  }

  // Slow path: full comparison
  for (; an && bn; an=an->next, bn=bn->next) {
    if (an == bn) // exploit shared list structure
      goto equal;
    if (int res = an->compareContents(bn))
      return res;
  }

  assert(!an && !bn);  

equal:
  // Update the cache
  EqCache[EqCacheNextIdx].ahead = head;
  EqCache[EqCacheNextIdx].bhead = b.head;
  if (++EqCacheNextIdx == EqCacheSize)
    EqCacheNextIdx = 0;

  return 0;
}

unsigned UpdateList::hash() const {
  unsigned res = 0;
  if (root) res ^= root->hash();
  if (head) res ^= head->hash();
  return res;
}

//-----------------------------------------------------------------------------
// Array
//-----------------------------------------------------------------------------

static ref<ForallExpr> GetArrayConstraint(const Array *array,
                                          const ref<Expr> &valueExpr,
                                          const ref<Expr> &guard,
                                          const ref<QVarExpr> &qvar,
                                          const UpdateList &nextSrc) {
  assert(array);
  assert(valueExpr.get());
  assert(qvar.get());
  assert(guard->isTrue() || nextSrc.root);

  // The constraint is triggered by a read of "array"
  const ForallTrigger trigger = ForallTrigger::onArrayReads(array);

  // Build the Forall
  ref<Expr> thisRead = ReadExpr::alloc(UpdateList(array, NULL), qvar, false);
  ref<Expr> ifTrue   = EqExpr::create(thisRead, valueExpr);
  if (guard->isTrue()) {
    // (forall i. array[i] == valueExpr)
    return ForallExpr::alloc(qvar, ifTrue, trigger);
  } else {
    // (forall i. ite(guard, array[i] == valueExpr, array[i] == next[i]))
    ref<Expr> nextRead = ReadExpr::alloc(nextSrc, qvar, false);
    ref<Expr> ifFalse  = EqExpr::create(thisRead, nextRead);
    return ForallExpr::alloc(qvar, SelectExpr::create(guard, ifTrue, ifFalse), trigger);
  }
}

Array::Array(const std::string &_name)
  : name(_name),
    stpInitialArray(NULL),
    type(Symbolic)
{
  computeHash();
}

Array::Array(const std::string &_name,
             const ref<ConstantExpr> *cvBegin,
             const ref<ConstantExpr> *cvEnd)
  : name(_name),
    stpInitialArray(NULL),
    type(ConstantInitialValues),
    _constantInitialValues(cvBegin, cvEnd)
{
  computeHash();
#ifndef NDEBUG
  if (cvBegin != cvEnd) {
    for (const ref<ConstantExpr> *it = cvBegin; it != cvEnd; ++it)
      assert((*it)->getWidth() == getRange() && "Invalid initial constant value!");
  }
#endif
}

Array::Array(const std::string &_name,
             const ref<Expr> valueExpr,
             const ref<Expr> guard,
             const UpdateList nextSrc,
             const ref<QVarExpr> qvar)
  : name(_name),
    stpInitialArray(NULL),
    type(ConstrainedInitialValues),
    _valueExpr(valueExpr),
    _guard(guard),
    _nextSrc(nextSrc),
    _constraint(GetArrayConstraint(this, valueExpr, guard, qvar, nextSrc))
{
  assert(guard->isTrue() || nextSrc.root);
  computeHash();
}

Array::~Array() {
  // XXX gross
  if (stpInitialArray)
    ::vc_DeleteExpr(stpInitialArray);
}

void Array::computeHash() {
  _hashValue = 0;

  switch (type) {
  case Symbolic:
    if (UseStableArrayHashing) {
      _hashValue = (unsigned)std::tr1::hash<std::string>()(name);
    } else {
      _hashValue = (unsigned)(uintptr_t)this;
    }
    break;

  case ConstantInitialValues:
    for (size_t k = 0; k < _constantInitialValues.size(); ++k)
      _hashValue ^= _constantInitialValues[k]->hash();
    break;

  case ConstrainedInitialValues:
    _hashValue ^= _constraint->instantiate(Expr::One32)->hash();
    break;
  }
}

const Array* Array::createSymbolic(const std::string &name) {
  return new Array(name);
}

const Array* Array::createWithConstantValues(const std::string &name,
                                             const ref<ConstantExpr> *cvBegin,
                                             const ref<ConstantExpr> *cvEnd) {
  return new Array(name, cvBegin, cvEnd);
}

const Array* Array::createConstant(const std::string &name,
                                   const ref<Expr> &valueExpr,
                                   ref<QVarExpr> qvar /*= NULL*/,
                                   ref<Expr> guard /*= Expr::True*/,
                                   UpdateList nextSrc /*= UpdateList(NULL,NULL)*/) {
  typedef ExprHashMap<const Array*> CacheTy;
  static CacheTy Cache;
  static const ref<QVarExpr> CanonicalQVar = QVarExpr::alloc("CANON", getDomain());

  // Sanity checks
  if (qvar.isNull()) {
    assert(guard->isTrue());
  }

  if (guard->isTrue()) {
    assert(nextSrc.root == NULL);
    assert(nextSrc.head == NULL);
  } else {
    assert(nextSrc.root);
  }

  // Construct the cache key
  // We use a canonicalized valueExpr (which uses a canonical qvar).
  // It is (probably?) unlikely that we will reuse arrays that are guarded, so
  // we cache unguarded arrays only.  XXX: could use the constraint as the key
  ref<Expr> key;
  if (qvar.get()) {
    key = ExprReplaceVisitor(qvar, CanonicalQVar, false).visit(valueExpr);
  } else {
    key = valueExpr;
  }

  // Alloc, using a cached array if possible.
  const Array *array = NULL;

  if (guard->isTrue()) {
    CacheTy::iterator it = Cache.find(key);
    if (it != Cache.end())
      array = it->second;
  }
  if (!array) {
    if (qvar.isNull()) {
      qvar = QVarExpr::alloc("Index", getDomain());
    }
    array = new Array(name, valueExpr, guard, nextSrc, qvar);
    if (guard->isTrue())
      Cache[key] = array;
  }

  return array;
}

const Array* Array::createCopy(const std::string &name,
                               const UpdateList &copySrc,
                               const ref<Expr> &offset,
                               ref<QVarExpr> qvar /*= NULL*/,
                               ref<Expr> guard /*= Expr::True*/,
                               UpdateList nextSrc /*= UpdateList(NULL,NULL)*/) {
  if (qvar.isNull())
    qvar = QVarExpr::alloc("CopyIndex", getDomain());

  // array[i] == copySrc[i+offset]
  ref<Expr> valueExpr = ReadExpr::create(copySrc, AddExpr::create(qvar, offset));
  return createConstant(name, valueExpr, qvar, guard, nextSrc);
}

ref<Expr> Array::getValueAtIndex(const ref<Expr> &index,
                                 const bool expectIsSymbolicPtr /*= false*/) const {
  switch (type) {
  case Symbolic:
  RawRead:
    // N.B.: No guard in this case
    // N.B.: Use ReadExpr::alloc() to avoid recursion with ReadExpr::create()
    return ReadExpr::alloc(UpdateList(this, NULL), index, expectIsSymbolicPtr);

  case ConstantInitialValues:
    // N.B.: No guard in this case
    if (ConstantExpr *cex = dyn_cast<ConstantExpr>(index)) {
      size_t i = cex->getZExtValue();
      if (i < _constantInitialValues.size())
        return _constantInitialValues[i];
    }
    goto RawRead;

  case ConstrainedInitialValues: {
    ref<Expr> thisValue;
    if (_guard->isTrue()) {
      thisValue = _valueExpr;
    } else {
      ref<Expr> next = ReadExpr::create(_nextSrc, index, expectIsSymbolicPtr);
      thisValue = SelectExpr::create(_guard, _valueExpr, next);
    }
    // N.B.: Need to replace the entire SelectExpr as the guard can also use the qvar
    thisValue = ExprReplaceVisitor(_constraint->qvar, index, false).visit(thisValue);
    return thisValue;
  }

  default:
    assert(0);
    return NULL;
  }
}

int Array::compare(const Array *b) const {
  const Array *a = this;
  if (a == b)
    return 0;

  if (a->hash() != b->hash())
    return a->hash() < b->hash() ? -1 : 1;

  if (a->type != b->type)
    return a->type < b->type ? -1 : 1;

  switch (a->type) {
  case Symbolic:                    // symbolic arrays compare by pointer equality
    return a < b ? -1 : 1;
  case ConstantInitialValues:  {    // constant arrays compare by contents
    const size_t asize = a->_constantInitialValues.size();
    const size_t bsize = b->_constantInitialValues.size();
    if (asize != bsize)
      return asize < bsize ? -1 : 1;
    for (size_t k = 0; k < asize; ++k) {
      if (int res = a->_constantInitialValues[k].compare(b->_constantInitialValues[k]))
        return res;
    }
    return 0;
  }
  case ConstrainedInitialValues:  { // constrained arrays compare by constraint
    ExprReplaceVisitor visitor(a->_constraint->qvar, b->_constraint->qvar, false);
    if (int res = visitor.visit(a->_guard).compare(b->_guard))
      return res;
    if (int res = visitor.visit(a->_valueExpr).compare(b->_valueExpr))
      return res;
    if (a->_guard->isTrue()) {
      return 0;
    } else {
      return a->_nextSrc.compare(b->_nextSrc);
    }
  }
  default:
    assert(0);
  }
}
