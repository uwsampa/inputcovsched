//===-- Expr.cpp ----------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Expr.h"

#include "llvm/Instructions.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
// FIXME: We shouldn't need this once fast constant support moves into
// Core. If we need to do arithmetic, we probably want to use APInt.
#include "klee/Internal/Support/IntEvaluation.h"

#include "klee/util/ExprPPrinter.h"
#include "klee/util/ExprUtil.h"
#include "klee/util/ExprVisitor.h"

#include <iostream>
#include <sstream>
#include <map>

using namespace klee;
using llvm::APInt;

/***/

ref<Expr> Expr::InvalidExprRef;

ref<ConstantExpr> Expr::Null;
ref<ConstantExpr> Expr::True;
ref<ConstantExpr> Expr::False;
ref<ConstantExpr> Expr::Zero8;
ref<ConstantExpr> Expr::Zero32;
ref<ConstantExpr> Expr::One8;
ref<ConstantExpr> Expr::One32;

unsigned Expr::count = 0;

ConstantExpr::CacheTy ConstantExpr::Cache;

/***/

bool Expr::hasSubExpr(ref<Expr> e, ref<Expr> sub) {
  if (e == sub)
    return true;

  unsigned aN = e->getNumKids();
  for (unsigned i=0; i<aN; i++) {
    if (hasSubExpr(e->getKid(i), sub))
      return true;
  }

  return false;
}

void Expr::setIsSymbolicPointer() {
  isSymbolicPointer = true;

  ref<ConcatExpr> concat = dyn_cast<ConcatExpr>(this);
  if (concat.get()) {
    concat->left->setIsSymbolicPointer();
    concat->right->setIsSymbolicPointer();
  }
}

bool Expr::isSymbolicRead(ref<Expr> e) {
  // Check for a ReadExpr or a ConcatExpr of ReadExprs.
  for (ExprNaryIterator it(e, Expr::Concat); !it.done(); ++it) {
    if (!isa<ReadExpr>(*it))
      return false;
  }
  return true;
}

ref<Expr> Expr::removeUpdatesFromSymbolicRead(ref<Expr> e) {
  // Assuming that Expr::isSymbolicRead(e), return a new Expr
  // which is an exact copy except that all ReadExprs have had
  // their update lists cleared.
  ref<ReadExpr> read = dyn_cast<ReadExpr>(e);
  if (read.get()) {
    if (read->updates.head)
      return read->updates.root->getValueAtIndex(read->index, e->isSymbolicPointer);
    return read;
  }

  ref<ConcatExpr> concat = dyn_cast<ConcatExpr>(e);
  if (concat.get()) {
    ref<Expr> left = removeUpdatesFromSymbolicRead(concat->left);
    ref<Expr> right = removeUpdatesFromSymbolicRead(concat->right);

    // Sanity check
    if ((isa<ReadExpr>(left)  && (left->isSymbolicPointer  != e->isSymbolicPointer)) ||
        (isa<ReadExpr>(right) && (right->isSymbolicPointer != e->isSymbolicPointer))) {
      std::cerr << "ERROR: bad 'isSymbolicPointer' field?:"
                << " OLD:"       << e->isSymbolicPointer
                << " OLD.LEFT:"  << concat->left->isSymbolicPointer
                << " OLD.RIGHT:" << concat->right->isSymbolicPointer
                << " NEW.LEFT:"  << left->isSymbolicPointer
                << " NEW.RIGHT:" << right->isSymbolicPointer
                << "\n";
      if (const ReadExpr* re = dyn_cast<ReadExpr>(concat->left)) {
        std::cerr << "OLD.LEFT:\n" << ReadExpr::alloc(UpdateList(re->updates.root,NULL),re->index,false) << "\n";
      }
      if (const ReadExpr* re = dyn_cast<ReadExpr>(concat->right)) {
        std::cerr << "OLD.RIGHT:\n" << ReadExpr::alloc(UpdateList(re->updates.root,NULL),re->index,false) << "\n";
      }
      std::cerr << "NEW.LEFT:\n" << left << "\n";
      std::cerr << "NEW.RIGHT:\n" << right << "\n";
      std::cerr << "NEW:\n" << ConcatExpr::create(left,right) << "\n";
      assert(0);
    }

    // Rebuild with the new children
    if (left.get() != concat->left.get() || right.get() != concat->right.get()) {
      return ConcatExpr::create(left, right);
    } else {
      return e;
    }
  }

  assert(0);
  return e;
}

static bool isReadExprAtOffset(ref<Expr> e, const ReadExpr *base, ref<Expr> offset) {
  const ReadExpr *re = dyn_cast<ReadExpr>(e.get());

  // Right now, all Reads are byte reads but some
  // transformations might change this
  if (!re || (re->getWidth() != Expr::Int8))
    return false;

  // Must read from the same array with the same updates
  if (re->updates.compare(base->updates) != 0)
    return false;

  // Check if the index follows the stride.
  // FIXME: How aggressive should this be simplified. The
  // canonicalizing builder is probably the right choice, but this
  // is yet another area where we would really prefer it to be
  // global or else use static methods.
  return SubExpr::create(re->index, base->index) == offset;
}

ref<ReadExpr> Expr::hasOrderedReads(ref<Expr> e, int stride) {
  assert(stride == 1 || stride == -1);

  if (ReadExpr *read = dyn_cast<ReadExpr>(e))
    return read;
  if (e->getKind() != Expr::Concat)
    return NULL;

  ReadExpr *base = dyn_cast<ReadExpr>(e->getKid(0));

  // right now, all Reads are byte reads but some
  // transformations might change this
  if (!base || base->getWidth() != Expr::Int8)
    return NULL;

  // Get stride expr in proper index width.
  Expr::Width idxWidth = base->index->getWidth();
  ref<Expr> strideExpr = ConstantExpr::alloc(stride, idxWidth);
  ref<Expr> offset = ConstantExpr::create(0, idxWidth);

  e = e->getKid(1);

  // concat chains are unbalanced to the right
  while (e->getKind() == Expr::Concat) {
    offset = AddExpr::create(offset, strideExpr);
    if (!isReadExprAtOffset(e->getKid(0), base, offset))
      return NULL;
    e = e->getKid(1);
  }

  offset = AddExpr::create(offset, strideExpr);
  if (!isReadExprAtOffset(e, base, offset))
    return NULL;

  if (stride == -1)
    return cast<ReadExpr>(e.get());
  else return base;
}

/***/

size_t Expr::getSize() const {
  size_t size = 1;
  unsigned aN = getNumKids();
  for (unsigned i=0; i<aN; i++)
    size += getKid(i)->getSize();
  return size;
}

// returns 0 if b is structurally equal to *this
int Expr::compare(const Expr &b) const {
  if (this == &b)
    return 0;

  if (hashValue != b.hashValue)
    return (hashValue < b.hashValue) ? -1 : 1;

  const Kind ak = getKind(), bk = b.getKind();
  if (ak!=bk)
    return (ak < bk) ? -1 : 1;

  if (int res = compareContents(b))
    return res;

  unsigned aN = getNumKids();
  for (unsigned i=0; i<aN; i++)
    if (int res = getKid(i).compare(b.getKid(i)))
      return res;

  return 0;
}

template<class OStream>
void Expr::printKind(OStream &os, Kind k) {
  switch(k) {
#define X(C) case C: os << #C; break
    X(Constant);
    X(NotOptimized);
    X(Read);
    X(Select);
    X(Concat);
    X(Extract);
    X(ZExt);
    X(SExt);
    X(Add);
    X(Sub);
    X(Mul);
    X(UDiv);
    X(SDiv);
    X(URem);
    X(SRem);
    X(Not);
    X(And);
    X(Or);
    X(Xor);
    X(Shl);
    X(LShr);
    X(AShr);
    X(Eq);
    X(Ne);
    X(Ult);
    X(Ule);
    X(Ugt);
    X(Uge);
    X(Slt);
    X(Sle);
    X(Sgt);
    X(Sge);
    X(Implies);
    X(QVar);
    X(Forall);
#undef X
  default:
    assert(0 && "invalid kind");
    }
}

template<class OStream>
void Expr::printWidth(OStream &os, Width width) {
  switch(width) {
  case Expr::Bool: os << "Expr::Bool"; break;
  case Expr::Int8: os << "Expr::Int8"; break;
  case Expr::Int16: os << "Expr::Int16"; break;
  case Expr::Int32: os << "Expr::Int32"; break;
  case Expr::Int64: os << "Expr::Int64"; break;
  case Expr::Fl80: os << "Expr::Fl80"; break;
  default: os << "<invalid type: " << (unsigned) width << ">";
  }
}

namespace klee {

template void Expr::printKind(std::ostream &os, Kind k);
template void Expr::printKind(llvm::raw_ostream &os, Kind k);

template void Expr::printWidth(std::ostream &os, Width width);
template void Expr::printWidth(llvm::raw_ostream &os, Width width);

void Expr::print(std::ostream &os) const {
  ExprPPrinter::printSingleExpr(os, const_cast<Expr*>(this));
}

void Expr::print(llvm::raw_ostream &os) const {
  std::ostringstream str;
  ExprPPrinter::printSingleExpr(str, const_cast<Expr*>(this));
  os << str.str();
}

}  // end namespace klee

////////
//
// Simple hash functions for various kinds of Exprs
//
///////

unsigned Expr::computeHash() {
  unsigned res = getKind() * Expr::MAGIC_HASH_CONSTANT;

  int n = getNumKids();
  for (int i = 0; i < n; i++) {
    res <<= 1;
    res ^= getKid(i)->hash() * Expr::MAGIC_HASH_CONSTANT;
  }

  hashValue = res;
  return hashValue;
}

unsigned ConstantExpr::computeHash() {
  hashValue = value.getHashValue() ^ (getWidth() * MAGIC_HASH_CONSTANT);
  return hashValue;
}

unsigned CastExpr::computeHash() {
  unsigned res = getWidth() * Expr::MAGIC_HASH_CONSTANT;
  hashValue = res ^ src->hash() * Expr::MAGIC_HASH_CONSTANT;
  return hashValue;
}

unsigned ExtractExpr::computeHash() {
  unsigned res = offset * Expr::MAGIC_HASH_CONSTANT;
  res ^= getWidth() * Expr::MAGIC_HASH_CONSTANT;
  hashValue = res ^ expr->hash() * Expr::MAGIC_HASH_CONSTANT;
  return hashValue;
}

unsigned ReadExpr::computeHash() {
  unsigned res = index->hash() * Expr::MAGIC_HASH_CONSTANT;
  res ^= updates.hash();
  hashValue = res;
  return hashValue;
}

unsigned NotExpr::computeHash() {
  unsigned hashValue = expr->hash() * Expr::MAGIC_HASH_CONSTANT * Expr::Not;
  return hashValue;
}

ref<Expr> Expr::createFromKind(Kind k, std::vector<CreateArg> args) {
  unsigned numArgs = args.size();
  (void) numArgs;

  switch(k) {
    case Constant:
    case Extract:
    case Read:
    case QVar:
    case Forall:
    default:
      assert(0 && "invalid kind");

    case NotOptimized:
      assert(numArgs == 1 && args[0].isExpr() &&
             "invalid args array for given opcode");
      return NotOptimizedExpr::create(args[0].expr);

    case Select:
      assert(numArgs == 3 && args[0].isExpr() &&
             args[1].isExpr() && args[2].isExpr() &&
             "invalid args array for Select opcode");
      return SelectExpr::create(args[0].expr,
                                args[1].expr,
                                args[2].expr);

    case Concat: {
      assert(numArgs == 2 && args[0].isExpr() && args[1].isExpr() &&
             "invalid args array for Concat opcode");

      return ConcatExpr::create(args[0].expr, args[1].expr);
    }

#define CAST_EXPR_CASE(T)                                    \
      case T:                                                \
        assert(numArgs == 2 &&				     \
               args[0].isExpr() && args[1].isWidth() &&      \
               "invalid args array for given opcode");       \
      return T ## Expr::create(args[0].expr, args[1].width); \

#define BINARY_EXPR_CASE(T)                                 \
      case T:                                               \
        assert(numArgs == 2 &&                              \
               args[0].isExpr() && args[1].isExpr() &&      \
               "invalid args array for given opcode");      \
      return T ## Expr::create(args[0].expr, args[1].expr); \

      CAST_EXPR_CASE(ZExt);
      CAST_EXPR_CASE(SExt);

      BINARY_EXPR_CASE(Add);
      BINARY_EXPR_CASE(Sub);
      BINARY_EXPR_CASE(Mul);
      BINARY_EXPR_CASE(UDiv);
      BINARY_EXPR_CASE(SDiv);
      BINARY_EXPR_CASE(URem);
      BINARY_EXPR_CASE(SRem);
      BINARY_EXPR_CASE(And);
      BINARY_EXPR_CASE(Or);
      BINARY_EXPR_CASE(Xor);
      BINARY_EXPR_CASE(Shl);
      BINARY_EXPR_CASE(LShr);
      BINARY_EXPR_CASE(AShr);

      BINARY_EXPR_CASE(Eq);
      BINARY_EXPR_CASE(Ne);
      BINARY_EXPR_CASE(Ult);
      BINARY_EXPR_CASE(Ule);
      BINARY_EXPR_CASE(Ugt);
      BINARY_EXPR_CASE(Uge);
      BINARY_EXPR_CASE(Slt);
      BINARY_EXPR_CASE(Sle);
      BINARY_EXPR_CASE(Sgt);
      BINARY_EXPR_CASE(Sge);

      BINARY_EXPR_CASE(Implies);
  }
}

ref<Expr> Expr::createIff(ref<Expr> l, ref<Expr> r) {
  assert(l->getWidth() == Bool && "iff: type mismatch on l");
  assert(r->getWidth() == Bool && "iff: type mismatch on r");
  return EqExpr::create(l, r);
}

ref<Expr> Expr::createImplies(ref<Expr> hyp, ref<Expr> conc) {
  return ImpliesExpr::create(hyp, conc);
}

ref<Expr> Expr::createIsZero(ref<Expr> e) {
  return EqExpr::create(e, ConstantExpr::create(0, e->getWidth()));
}

ref<Expr> Expr::createFromICmp(llvm::ICmpInst *icmp, ref<Expr> left, ref<Expr> right) {
  switch (icmp->getPredicate()) {
  case llvm::ICmpInst::ICMP_EQ:
    return EqExpr::create(left, right);

  case llvm::ICmpInst::ICMP_NE:
    return NeExpr::create(left, right);

  case llvm::ICmpInst::ICMP_UGT:
    return UgtExpr::create(left, right);

  case llvm::ICmpInst::ICMP_UGE:
    return UgeExpr::create(left, right);

  case llvm::ICmpInst::ICMP_ULT:
    return UltExpr::create(left, right);

  case llvm::ICmpInst::ICMP_ULE:
    return UleExpr::create(left, right);

  case llvm::ICmpInst::ICMP_SGT:
    return SgtExpr::create(left, right);

  case llvm::ICmpInst::ICMP_SGE:
    return SgeExpr::create(left, right);

  case llvm::ICmpInst::ICMP_SLT:
    return SltExpr::create(left, right);

  case llvm::ICmpInst::ICMP_SLE:
    return SleExpr::create(left, right);

  default:
    assert(0);
    return NULL;
  }
}

void Expr::dump() const {
  this->print(std::cerr);
  std::cerr << std::endl;
}

// Mimics ObjectState::read()
ref<Expr> Expr::createArrayReadAtIndex(const Array *array,
                                       ref<Expr> baseIndex,
                                       Expr::Width width,
                                       bool isLittleEndian) {
  assert(array);

  // Special case for non-byte sized reads
  if (width == Expr::Bool) {
    return ZExtExpr::create(array->getValueAtIndex(baseIndex), Expr::Bool);
  }

  // General case
  const unsigned numBytes = width / 8;
  assert(width == numBytes * 8 && "Invalid read size!");

  ref<Expr> res;
  for (unsigned i = 0; i != numBytes; ++i) {
    ref<Expr> idx = ConstantExpr::alloc(isLittleEndian ? i : (numBytes - i - 1), Array::getDomain());
    ref<Expr> byte = array->getValueAtIndex(AddExpr::create(baseIndex, idx));
    res = i ? ConcatExpr::create(byte, res) : byte;
  }

  return res;
}

// Mimics ObjectState::read()
ref<Expr> Expr::createMultibyteRead(const UpdateList &updates,
                                    ref<Expr> baseIndex,
                                    const unsigned nbytes,
                                    const bool isLittleEndian) {
  ref<Expr> res;
  for (unsigned i = 0; i != nbytes; ++i) {
    ref<Expr> idx = ConstantExpr::alloc(isLittleEndian ? i : (nbytes - i - 1), Array::getDomain());
    ref<Expr> byte = ReadExpr::create(updates, AddExpr::create(baseIndex, idx));
    res = i ? ConcatExpr::create(byte, res) : byte;
  }
  return res;
}

/***/

ref<Expr> ConstantExpr::fromMemory(void *address, Width width) {
  switch (width) {
  case  Expr::Bool: return ConstantExpr::create(*(( uint8_t*) address), width);
  case  Expr::Int8: return ConstantExpr::create(*(( uint8_t*) address), width);
  case Expr::Int16: return ConstantExpr::create(*((uint16_t*) address), width);
  case Expr::Int32: return ConstantExpr::create(*((uint32_t*) address), width);
  case Expr::Int64: return ConstantExpr::create(*((uint64_t*) address), width);
  // FIXME: what about machines without x87 support?
  default:
    return ConstantExpr::alloc(llvm::APInt(width,
      (width+llvm::integerPartWidth-1)/llvm::integerPartWidth,
      (const uint64_t*)address));
  }
}

void ConstantExpr::toMemory(void *address) {
  switch (getWidth()) {
  default: assert(0 && "invalid type");
  case  Expr::Bool: *(( uint8_t*) address) = getZExtValue(1); break;
  case  Expr::Int8: *(( uint8_t*) address) = getZExtValue(8); break;
  case Expr::Int16: *((uint16_t*) address) = getZExtValue(16); break;
  case Expr::Int32: *((uint32_t*) address) = getZExtValue(32); break;
  case Expr::Int64: *((uint64_t*) address) = getZExtValue(64); break;
  // FIXME: what about machines without x87 support?
  case Expr::Fl80:
    *((long double*) address) = *(long double*) value.getRawData();
    break;
  }
}

void ConstantExpr::toString(std::string &Res) const {
  Res = value.toString(10, false);
}

ref<ConstantExpr> ConstantExpr::Concat(const ref<ConstantExpr> &RHS) {
  Expr::Width W = getWidth() + RHS->getWidth();
  APInt Tmp(value);
  Tmp=Tmp.zext(W);
  Tmp <<= RHS->getWidth();
  Tmp |= APInt(RHS->value).zext(W);

  return ConstantExpr::alloc(Tmp);
}

ref<ConstantExpr> ConstantExpr::Extract(unsigned Offset, Width W) {
  return ConstantExpr::alloc(APInt(value.ashr(Offset)).zextOrTrunc(W));
}

ref<ConstantExpr> ConstantExpr::ZExt(Width W) {
  return ConstantExpr::alloc(APInt(value).zextOrTrunc(W));
}

ref<ConstantExpr> ConstantExpr::SExt(Width W) {
  return ConstantExpr::alloc(APInt(value).sextOrTrunc(W));
}

ref<ConstantExpr> ConstantExpr::Add(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value + RHS->value);
}

ref<ConstantExpr> ConstantExpr::Neg() {
  return ConstantExpr::alloc(-value);
}

ref<ConstantExpr> ConstantExpr::Sub(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value - RHS->value);
}

ref<ConstantExpr> ConstantExpr::Mul(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value * RHS->value);
}

ref<ConstantExpr> ConstantExpr::UDiv(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value.udiv(RHS->value));
}

ref<ConstantExpr> ConstantExpr::SDiv(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value.sdiv(RHS->value));
}

ref<ConstantExpr> ConstantExpr::URem(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value.urem(RHS->value));
}

ref<ConstantExpr> ConstantExpr::SRem(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value.srem(RHS->value));
}

ref<ConstantExpr> ConstantExpr::And(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value & RHS->value);
}

ref<ConstantExpr> ConstantExpr::Or(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value | RHS->value);
}

ref<ConstantExpr> ConstantExpr::Xor(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value ^ RHS->value);
}

ref<ConstantExpr> ConstantExpr::Shl(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value.shl(RHS->value));
}

ref<ConstantExpr> ConstantExpr::LShr(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value.lshr(RHS->value));
}

ref<ConstantExpr> ConstantExpr::AShr(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value.ashr(RHS->value));
}

ref<ConstantExpr> ConstantExpr::Not() {
  return ConstantExpr::alloc(~value);
}

ref<ConstantExpr> ConstantExpr::Eq(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value == RHS->value, Expr::Bool);
}

ref<ConstantExpr> ConstantExpr::Ne(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value != RHS->value, Expr::Bool);
}

ref<ConstantExpr> ConstantExpr::Ult(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value.ult(RHS->value), Expr::Bool);
}

ref<ConstantExpr> ConstantExpr::Ule(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value.ule(RHS->value), Expr::Bool);
}

ref<ConstantExpr> ConstantExpr::Ugt(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value.ugt(RHS->value), Expr::Bool);
}

ref<ConstantExpr> ConstantExpr::Uge(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value.uge(RHS->value), Expr::Bool);
}

ref<ConstantExpr> ConstantExpr::Slt(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value.slt(RHS->value), Expr::Bool);
}

ref<ConstantExpr> ConstantExpr::Sle(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value.sle(RHS->value), Expr::Bool);
}

ref<ConstantExpr> ConstantExpr::Sgt(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value.sgt(RHS->value), Expr::Bool);
}

ref<ConstantExpr> ConstantExpr::Sge(const ref<ConstantExpr> &RHS) {
  return ConstantExpr::alloc(value.sge(RHS->value), Expr::Bool);
}

/***/

ref<Expr>  NotOptimizedExpr::create(ref<Expr> src) {
  return NotOptimizedExpr::alloc(src);
}

/***/

ref<Expr> ReadExpr::create(const UpdateList &ul, ref<Expr> index, const bool isSymbolicPtr) {
  // rollback index when possible...

  // XXX this doesn't really belong here... there are basically two
  // cases, one is rebuild, where we want to optimistically try various
  // optimizations when the index has changed, and the other is
  // initial creation, where we expect the ObjectState to have constructed
  // a smart UpdateList so it is not worth rescanning.
  // XXX actually, I think this does optimizations that ObjectState does NOT
  // do, mainly for the possibility of matching a symbolic index to an index
  // in the updates list.

  for (const UpdateNode *un = ul.head; un; un = un->next) {
    ref<Expr> cond = un->getGuardForIndex(index);

    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(cond)) {
      if (CE->isTrue())
        return un->getValueAtIndex(index, isSymbolicPtr);
    } else {
      return ReadExpr::alloc(ul, index, isSymbolicPtr);
    }
  }

  // Reached the end of the UpdateList, so read directly from the array
  return ReadExpr::alloc(UpdateList(ul.root, NULL), index, isSymbolicPtr);
}

int ReadExpr::compareContents(const Expr &b) const {
  return updates.compare(static_cast<const ReadExpr&>(b).updates);
}

ref<Expr> SelectExpr::create(ref<Expr> c, ref<Expr> t, ref<Expr> f) {
  Expr::Width kt = t->getWidth();

  assert(c->getWidth()==Bool && "type mismatch");
  assert(kt==f->getWidth() && "type mismatch");

  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(c)) {
    return CE->isTrue() ? t : f;
  } else if (t==f) {
    return t;
  } else if (kt==Expr::Bool) { // c ? t : f  <=> (c and t) or (not c and f)
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(t)) {
      if (CE->isTrue()) {
        return OrExpr::create(c, f);
      } else {
        return AndExpr::create(Expr::createIsZero(c), f);
      }
    } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(f)) {
      if (CE->isTrue()) {
        return OrExpr::create(Expr::createIsZero(c), t);
      } else {
        return AndExpr::create(c, t);
      }
    }
  }

  return SelectExpr::alloc(c, t, f);
}

/***/

ref<Expr> ConcatExpr::create(const ref<Expr> &l, const ref<Expr> &r) {
  Expr::Width w = l->getWidth() + r->getWidth();

  // Unbalanced to the right
  if (ConcatExpr *cleft = dyn_cast<ConcatExpr>(l)) {
    return ConcatExpr::create(cleft->left, ConcatExpr::create(cleft->right, r));
  }

  // Fold concatenation of constants.
  if (ConstantExpr *lCE = dyn_cast<ConstantExpr>(l)) {
    if (ConstantExpr *rCE = dyn_cast<ConstantExpr>(r))
      return lCE->Concat(rCE);
    // concat 0 x -> zext x
    if (lCE->isZero())
      return ZExtExpr::create(r, w);
  }

  // Merge contiguous Extracts
  if (ExtractExpr *ee_left = dyn_cast<ExtractExpr>(l)) {
    if (ExtractExpr *ee_right = dyn_cast<ExtractExpr>(r)) {
      if (ee_left->expr == ee_right->expr &&
          ee_right->offset + ee_right->width == ee_left->offset) {
        return ExtractExpr::create(ee_left->expr, ee_right->offset, w);
      }
    }
  }

  // Lift selects
  // (concat (select c t1 f1) (select c t2 f2)) ==> (select c (concat t1 t2) (concat f1 f2))
  if (SelectExpr *sleft = dyn_cast<SelectExpr>(l)) {
    if (SelectExpr *sright = dyn_cast<SelectExpr>(r)) {
      if (sleft->cond == sright->cond) {
        return SelectExpr::create(sleft->cond,
                                  ConcatExpr::create(sleft->trueExpr, sright->trueExpr),
                                  ConcatExpr::create(sleft->falseExpr, sright->falseExpr));
      }
    }
  }

  return ConcatExpr::alloc(l, r);
}

/// Shortcut to concat N kids.  The chain returned is unbalanced to the right
ref<Expr> ConcatExpr::createN(unsigned n_kids, const ref<Expr> kids[]) {
  assert(n_kids > 0);
  if (n_kids == 1)
    return kids[0];

  ref<Expr> r = ConcatExpr::create(kids[n_kids-2], kids[n_kids-1]);
  for (int i=n_kids-3; i>=0; i--)
    r = ConcatExpr::create(kids[i], r);
  return r;
}

/// Shortcut to concat 4 kids.  The chain returned is unbalanced to the right
ref<Expr> ConcatExpr::create4(const ref<Expr> &kid1, const ref<Expr> &kid2,
                              const ref<Expr> &kid3, const ref<Expr> &kid4) {
  return ConcatExpr::create(kid1, ConcatExpr::create(kid2, ConcatExpr::create(kid3, kid4)));
}

/// Shortcut to concat 8 kids.  The chain returned is unbalanced to the right
ref<Expr> ConcatExpr::create8(const ref<Expr> &kid1, const ref<Expr> &kid2,
			      const ref<Expr> &kid3, const ref<Expr> &kid4,
			      const ref<Expr> &kid5, const ref<Expr> &kid6,
			      const ref<Expr> &kid7, const ref<Expr> &kid8) {
  return ConcatExpr::create(kid1, ConcatExpr::create(kid2, ConcatExpr::create(kid3,
			      ConcatExpr::create(kid4, ConcatExpr::create4(kid5, kid6, kid7, kid8)))));
}

/***/

ref<Expr> ExtractExpr::create(ref<Expr> expr, unsigned off, Width w) {
  unsigned kw = expr->getWidth();
  assert(w > 0 && off + w <= kw && "invalid extract");

  if (w == kw) {
    return expr;
  } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(expr)) {
    return CE->Extract(off, w);
  } else {
    // Extract(Concat)
    if (ConcatExpr *ce = dyn_cast<ConcatExpr>(expr)) {
      // if the extract skips the right side of the concat
      if (off >= ce->right->getWidth())
        return ExtractExpr::create(ce->left, off - ce->right->getWidth(), w);

      // if the extract skips the left side of the concat
      if (off + w <= ce->right->getWidth())
        return ExtractExpr::create(ce->right, off, w);

      // E(C(x,y)) = C(E(x), E(y))
      return ConcatExpr::create(ExtractExpr::create(ce->getKid(0), 0, w - ce->getKid(1)->getWidth() + off),
                                ExtractExpr::create(ce->getKid(1), off, ce->getKid(1)->getWidth() - off));
    }
    // Extract(ZExt(x))
    if (ZExtExpr *ze = dyn_cast<ZExtExpr>(expr)) {
      if (off == 0 && w == ze->src->getWidth())
        return ze->src;
      if (off >= ze->src->getWidth())
        return ConstantExpr::create(0, w);
    }
  }

  return ExtractExpr::alloc(expr, off, w);
}

/***/

ref<Expr> NotExpr::create(const ref<Expr> &e) {
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(e))
    return CE->Not();

  return NotExpr::alloc(e);
}


/***/

ref<Expr> ZExtExpr::create(const ref<Expr> &e, Width w) {
  unsigned kBits = e->getWidth();
  if (w == kBits) {
    return e;
  } else if (w < kBits) { // trunc
    return ExtractExpr::create(e, 0, w);
  } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(e)) {
    return CE->ZExt(w);
  } else if (ZExtExpr *ZE = dyn_cast<ZExtExpr>(e)) {
    return ZExtExpr::alloc(ZE->src, w);
  } else {
    return ZExtExpr::alloc(e, w);
  }
}

ref<Expr> SExtExpr::create(const ref<Expr> &e, Width w) {
  unsigned kBits = e->getWidth();
  if (w == kBits) {
    return e;
  } else if (w < kBits) { // trunc
    return ExtractExpr::create(e, 0, w);
  } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(e)) {
    return CE->SExt(w);
  } else {
    return SExtExpr::alloc(e, w);
  }
}

/***/

static ref<Expr> AndExpr_create(Expr *l, Expr *r);
static ref<Expr> XorExpr_create(Expr *l, Expr *r);

static ref<Expr> EqExpr_createPartial(Expr *l, const ref<ConstantExpr> &cr);
static ref<Expr> AndExpr_createPartialR(const ref<ConstantExpr> &cl, Expr *r);
static ref<Expr> SubExpr_createPartialR(const ref<ConstantExpr> &cl, Expr *r);
static ref<Expr> XorExpr_createPartialR(const ref<ConstantExpr> &cl, Expr *r);

static ref<Expr> AddExpr_createPartialR(const ref<ConstantExpr> &cl, Expr *r) {
  Expr::Width type = cl->getWidth();

  if (type==Expr::Bool) {
    return XorExpr_createPartialR(cl, r);
  } else if (cl->isZero()) {
    return r;
  } else {
    Expr::Kind rk = r->getKind();
    if (rk==Expr::Add && isa<ConstantExpr>(r->getKid(0))) { // A + (B+c) == (A+B) + c
      return AddExpr::create(AddExpr::create(cl, r->getKid(0)),
                             r->getKid(1));
    } else if (rk==Expr::Sub && isa<ConstantExpr>(r->getKid(0))) { // A + (B-c) == (A+B) - c
      return SubExpr::create(AddExpr::create(cl, r->getKid(0)),
                             r->getKid(1));
    } else {
      return AddExpr::alloc(cl, r);
    }
  }
}
static ref<Expr> AddExpr_createPartial(Expr *l, const ref<ConstantExpr> &cr) {
  return AddExpr_createPartialR(cr, l);
}
static ref<Expr> AddExpr_create(Expr *l, Expr *r) {
  Expr::Width type = l->getWidth();

  if (type == Expr::Bool) {
    return XorExpr_create(l, r);
  } else {
    Expr::Kind lk = l->getKind(), rk = r->getKind();
    if (lk==Expr::Add && isa<ConstantExpr>(l->getKid(0))) { // (k+a)+b = k+(a+b)
      return AddExpr::create(l->getKid(0),
                             AddExpr::create(l->getKid(1), r));
    } else if (lk==Expr::Sub && isa<ConstantExpr>(l->getKid(0))) { // (k-a)+b = k+(b-a)
      return AddExpr::create(l->getKid(0),
                             SubExpr::create(r, l->getKid(1)));
    } else if (rk==Expr::Add && isa<ConstantExpr>(r->getKid(0))) { // a + (k+b) = k+(a+b)
      return AddExpr::create(r->getKid(0),
                             AddExpr::create(l, r->getKid(1)));
    } else if (rk==Expr::Sub && isa<ConstantExpr>(r->getKid(0))) { // a + (k-b) = k+(a-b)
      return AddExpr::create(r->getKid(0),
                             SubExpr::create(l, r->getKid(1)));
    } else {
      return AddExpr::alloc(l, r);
    }
  }
}

static ref<Expr> SubExpr_createPartialR(const ref<ConstantExpr> &cl, Expr *r) {
  Expr::Width type = cl->getWidth();

  if (type==Expr::Bool) {
    return XorExpr_createPartialR(cl, r);
  } else {
    Expr::Kind rk = r->getKind();
    if (rk==Expr::Add && isa<ConstantExpr>(r->getKid(0))) { // A - (B+c) == (A-B) - c
      return SubExpr::create(SubExpr::create(cl, r->getKid(0)),
                             r->getKid(1));
    } else if (rk==Expr::Sub && isa<ConstantExpr>(r->getKid(0))) { // A - (B-c) == (A-B) + c
      return AddExpr::create(SubExpr::create(cl, r->getKid(0)),
                             r->getKid(1));
    } else {
      return SubExpr::alloc(cl, r);
    }
  }
}
static ref<Expr> SubExpr_createPartial(Expr *l, const ref<ConstantExpr> &cr) {
  // l - c => l + (-c)
  return AddExpr_createPartial(l,
                               ConstantExpr::alloc(0, cr->getWidth())->Sub(cr));
}
static ref<Expr> SubExpr_create(Expr *l, Expr *r) {
  Expr::Width type = l->getWidth();

  if (type == Expr::Bool) {
    return XorExpr_create(l, r);
  } else if (*l==*r) {
    return ConstantExpr::alloc(0, type);
  } else {
    Expr::Kind lk = l->getKind(), rk = r->getKind();
    if (lk==Expr::Add && isa<ConstantExpr>(l->getKid(0))) { // (k+a)-b = k+(a-b)
      return AddExpr::create(l->getKid(0),
                             SubExpr::create(l->getKid(1), r));
    } else if (lk==Expr::Sub && isa<ConstantExpr>(l->getKid(0))) { // (k-a)-b = k-(a+b)
      return SubExpr::create(l->getKid(0),
                             AddExpr::create(l->getKid(1), r));
    } else if (rk==Expr::Add && isa<ConstantExpr>(r->getKid(0))) { // a - (k+b) = (a-c) - k
      return SubExpr::create(SubExpr::create(l, r->getKid(1)),
                             r->getKid(0));
    } else if (rk==Expr::Sub && isa<ConstantExpr>(r->getKid(0))) { // a - (k-b) = (a+b) - k
      return SubExpr::create(AddExpr::create(l, r->getKid(1)),
                             r->getKid(0));
    } else {
      return SubExpr::alloc(l, r);
    }
  }
}

static ref<Expr> MulExpr_createPartialR(const ref<ConstantExpr> &cl, Expr *r) {
  Expr::Width type = cl->getWidth();

  if (type == Expr::Bool) {
    return AndExpr_createPartialR(cl, r);
  } else if (cl->isOne()) {
    return r;
  } else if (cl->isZero()) {
    return cl;
  } else {
    return MulExpr::alloc(cl, r);
  }
}
static ref<Expr> MulExpr_createPartial(Expr *l, const ref<ConstantExpr> &cr) {
  return MulExpr_createPartialR(cr, l);
}
static ref<Expr> MulExpr_create(Expr *l, Expr *r) {
  Expr::Width type = l->getWidth();

  if (type == Expr::Bool) {
    return AndExpr::alloc(l, r);
  } else {
    return MulExpr::alloc(l, r);
  }
}

static bool isNegation_Partial(Expr *l, Expr *r) {
  // Assuming width==Bool, is l the negation of r?
  if (EqExpr *EQ = dyn_cast<EqExpr>(l)) {   // l == (eq false r)?
    if (EQ->left == Expr::False && EQ->right == r)
      return true;
  }

  return false;
}

static bool isNegation(Expr *l, Expr *r) {
  return isNegation_Partial(l, r)
      || isNegation_Partial(r, l);
}

static ref<Expr> AndExpr_createPartial(Expr *l, const ref<ConstantExpr> &cr) {
  if (cr->isAllOnes()) {
    return l;
  } else if (cr->isZero()) {
    return cr;
  } else {
    return AndExpr::alloc(l, cr);
  }
}
static ref<Expr> AndExpr_createPartialR(const ref<ConstantExpr> &cl, Expr *r) {
  return AndExpr_createPartial(r, cl);
}
static ref<Expr> AndExpr_create(Expr *l, Expr *r) {
  if (l->getKind() == Expr::And) {  // (a&b)&c = a&(b&c)
    return AndExpr::create(l->getKid(0), AndExpr::create(l->getKid(1), r));
  }

  if (l->getWidth() == Expr::Bool) {
    // l & (a & b & .. &  l & ..) ==> r
    // l & (a & b & .. & !l & ..) ==> false
    for (ExprNaryIterator it(r, Expr::And); it != ExprNaryIterator(); ++it) {
      if (l->compare(**it) == 0)
        return r;
      if (isNegation(l, it.get()))
        return Expr::False;
    }
  }

  return AndExpr::alloc(l, r);
}

static ref<Expr> OrExpr_createPartial(Expr *l, const ref<ConstantExpr> &cr) {
  if (cr->isAllOnes()) {
    return cr;
  } else if (cr->isZero()) {
    return l;
  } else {
    return OrExpr::alloc(l, cr);
  }
}
static ref<Expr> OrExpr_createPartialR(const ref<ConstantExpr> &cl, Expr *r) {
  return OrExpr_createPartial(r, cl);
}
static ref<Expr> OrExpr_create(Expr *l, Expr *r) {
  if (l->getKind() == Expr::Or) {  // (a|b)|c = a|(b|c)
    return OrExpr::create(l->getKid(0), OrExpr::create(l->getKid(1), r));
  }

  if (l->getWidth() == Expr::Bool) {
    // l | (a | b | .. |  l | ..) ==> r
    // l | (a | b | .. | !l | ..) ==> true
    for (ExprNaryIterator it(r, Expr::Or); it != ExprNaryIterator(); ++it) {
      if (l->compare(**it) == 0)
        return r;
      if (isNegation(l, it.get()))
        return Expr::True;
    }
  }

  return OrExpr::alloc(l, r);
}

static ref<Expr> XorExpr_createPartialR(const ref<ConstantExpr> &cl, Expr *r) {
  if (cl->isZero()) {
    return r;
  } else if (cl->getWidth() == Expr::Bool) {
    return EqExpr_createPartial(r, ConstantExpr::create(0, Expr::Bool));
  } else {
    return XorExpr::alloc(cl, r);
  }
}

static ref<Expr> XorExpr_createPartial(Expr *l, const ref<ConstantExpr> &cr) {
  return XorExpr_createPartialR(cr, l);
}
static ref<Expr> XorExpr_create(Expr *l, Expr *r) {
  return XorExpr::alloc(l, r);
}

static ref<Expr> UDivExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l->getWidth() == Expr::Bool) { // r must be 1
    return l;
  } else{
    return UDivExpr::alloc(l, r);
  }
}

static ref<Expr> SDivExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l->getWidth() == Expr::Bool) { // r must be 1
    return l;
  } else{
    return SDivExpr::alloc(l, r);
  }
}

static ref<Expr> URemExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l->getWidth() == Expr::Bool) { // r must be 1
    return ConstantExpr::create(0, Expr::Bool);
  } else{
    return URemExpr::alloc(l, r);
  }
}

static ref<Expr> SRemExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l->getWidth() == Expr::Bool) { // r must be 1
    return ConstantExpr::create(0, Expr::Bool);
  } else{
    return SRemExpr::alloc(l, r);
  }
}

static ref<Expr> ShlExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l->getWidth() == Expr::Bool) { // l & !r
    return AndExpr::create(l, Expr::createIsZero(r));
  } else{
    return ShlExpr::alloc(l, r);
  }
}

static ref<Expr> LShrExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l->getWidth() == Expr::Bool) { // l & !r
    return AndExpr::create(l, Expr::createIsZero(r));
  } else{
    return LShrExpr::alloc(l, r);
  }
}

static ref<Expr> AShrExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l->getWidth() == Expr::Bool) { // l
    return l;
  } else{
    return AShrExpr::alloc(l, r);
  }
}

#define BCREATE_R(_e_op, _op, partialL, partialR) \
ref<Expr>  _e_op ::create(const ref<Expr> &l, const ref<Expr> &r) { \
  assert(l->getWidth()==r->getWidth() && "type mismatch");              \
  if (ConstantExpr *cl = dyn_cast<ConstantExpr>(l)) {                   \
    if (ConstantExpr *cr = dyn_cast<ConstantExpr>(r))                   \
      return cl->_op(cr);                                               \
    return _e_op ## _createPartialR(cl, r.get());                       \
  } else if (ConstantExpr *cr = dyn_cast<ConstantExpr>(r)) {            \
    return _e_op ## _createPartial(l.get(), cr);                        \
  }                                                                     \
  return _e_op ## _create(l.get(), r.get());                            \
}

#define BCREATE(_e_op, _op) \
ref<Expr>  _e_op ::create(const ref<Expr> &l, const ref<Expr> &r) { \
  assert(l->getWidth()==r->getWidth() && "type mismatch");          \
  if (ConstantExpr *cl = dyn_cast<ConstantExpr>(l))                 \
    if (ConstantExpr *cr = dyn_cast<ConstantExpr>(r))               \
      return cl->_op(cr);                                           \
  return _e_op ## _create(l, r);                                    \
}

BCREATE_R(AddExpr, Add, AddExpr_createPartial, AddExpr_createPartialR)
BCREATE_R(SubExpr, Sub, SubExpr_createPartial, SubExpr_createPartialR)
BCREATE_R(MulExpr, Mul, MulExpr_createPartial, MulExpr_createPartialR)
BCREATE_R(AndExpr, And, AndExpr_createPartial, AndExpr_createPartialR)
BCREATE_R(OrExpr, Or, OrExpr_createPartial, OrExpr_createPartialR)
BCREATE_R(XorExpr, Xor, XorExpr_createPartial, XorExpr_createPartialR)
BCREATE(UDivExpr, UDiv)
BCREATE(SDivExpr, SDiv)
BCREATE(URemExpr, URem)
BCREATE(SRemExpr, SRem)
BCREATE(ShlExpr, Shl)
BCREATE(LShrExpr, LShr)
BCREATE(AShrExpr, AShr)

#define CMPCREATE(_e_op, _op) \
ref<Expr>  _e_op ::create(const ref<Expr> &l, const ref<Expr> &r) { \
  assert(l->getWidth()==r->getWidth() && "type mismatch");              \
  if (ConstantExpr *cl = dyn_cast<ConstantExpr>(l))                     \
    if (ConstantExpr *cr = dyn_cast<ConstantExpr>(r))                   \
      return cl->_op(cr);                                               \
  return _e_op ## _create(l, r);                                        \
}

#define CMPCREATE_T(_e_op, _op, _reflexive_e_op, partialL, partialR) \
ref<Expr>  _e_op ::create(const ref<Expr> &l, const ref<Expr> &r) {    \
  assert(l->getWidth()==r->getWidth() && "type mismatch");             \
  if (ConstantExpr *cl = dyn_cast<ConstantExpr>(l)) {                  \
    if (ConstantExpr *cr = dyn_cast<ConstantExpr>(r))                  \
      return cl->_op(cr);                                              \
    return partialR(cl, r.get());                                      \
  } else if (ConstantExpr *cr = dyn_cast<ConstantExpr>(r)) {           \
    return partialL(l.get(), cr);                                      \
  } else {                                                             \
    return _e_op ## _create(l.get(), r.get());                         \
  }                                                                    \
}


static bool EqExpr_isNeq(const ref<Expr> &l, const ref<Expr> &r) {
  Expr::Kind lk = l->getKind();

  // Look for things that are "obviously" not equal
  // (a+k != a)
  if (lk == Expr::Add) {
    const AddExpr *ae = cast<AddExpr>(l);
    if (isa<ConstantExpr>(ae->left) && ae->right == r)
      return true;
  }

  return false;
}

static ref<Expr> EqExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l == r)
    return Expr::True;

  if (EqExpr_isNeq(l,r) || EqExpr_isNeq(r,l))
    return Expr::False;

  return EqExpr::alloc(l, r);
}

static ref<Expr> EqExpr_createPartialR(const ref<ConstantExpr> &cl, Expr *r) {
  Expr::Width width = cl->getWidth();

  Expr::Kind rk = r->getKind();
  if (width == Expr::Bool) {
    if (cl->isTrue()) {
      return r;
    } else {
      // 0 == ...

      if (rk == Expr::Eq) {
        const EqExpr *ree = cast<EqExpr>(r);

        // eliminate double negation
        if (ConstantExpr *CE = dyn_cast<ConstantExpr>(ree->left)) {
          // 0 == (0 == A) => A
          if (CE->getWidth() == Expr::Bool &&
              CE->isFalse())
            return ree->right;
        }
      } else if (rk == Expr::Or) {
        const OrExpr *roe = cast<OrExpr>(r);

        // transform not(or(a,b)) to and(not a, not b)
        return AndExpr::create(Expr::createIsZero(roe->left),
                               Expr::createIsZero(roe->right));
      }
    }
  } else if (rk == Expr::SExt) {
    // (sext(a,T)==c) == (a==c)
    const SExtExpr *see = cast<SExtExpr>(r);
    Expr::Width fromBits = see->src->getWidth();
    ref<ConstantExpr> trunc = cl->ZExt(fromBits);

    // pathological check, make sure it is possible to
    // sext to this value *from any value*
    if (cl == trunc->SExt(width)) {
      return EqExpr::create(see->src, trunc);
    } else {
      return ConstantExpr::create(0, Expr::Bool);
    }
  } else if (rk == Expr::ZExt) {
    // (zext(a,T)==c) == (a==c)
    const ZExtExpr *zee = cast<ZExtExpr>(r);
    Expr::Width fromBits = zee->src->getWidth();
    ref<ConstantExpr> trunc = cl->ZExt(fromBits);

    // pathological check, make sure it is possible to
    // zext to this value *from any value*
    if (cl == trunc->ZExt(width)) {
      return EqExpr::create(zee->src, trunc);
    } else {
      return ConstantExpr::create(0, Expr::Bool);
    }
  } else if (rk == Expr::Add) {
    const AddExpr *ae = cast<AddExpr>(r);
    if (isa<ConstantExpr>(ae->left)) {
      // c0 = c1 + b => c0 - c1 = b
      return EqExpr_createPartialR(cast<ConstantExpr>(SubExpr::create(cl,
                                                                      ae->left)),
                                   ae->right.get());
    }
  } else if (rk == Expr::Sub) {
    const SubExpr *se = cast<SubExpr>(r);
    if (isa<ConstantExpr>(se->left)) {
      // c0 = c1 - b => c1 - c0 = b
      return EqExpr_createPartialR(cast<ConstantExpr>(SubExpr::create(se->left,
                                                                      cl)),
                                   se->right.get());
    }
  }

  return EqExpr_create(cl, r);
}

static ref<Expr> EqExpr_createPartial(Expr *l, const ref<ConstantExpr> &cr) {
  return EqExpr_createPartialR(cr, l);
}

ref<Expr> NeExpr::create(const ref<Expr> &l, const ref<Expr> &r) {
  return EqExpr::create(ConstantExpr::create(0, Expr::Bool),
                        EqExpr::create(l, r));
}

ref<Expr> UgtExpr::create(const ref<Expr> &l, const ref<Expr> &r) {
  return UltExpr::create(r, l);
}
ref<Expr> UgeExpr::create(const ref<Expr> &l, const ref<Expr> &r) {
  return UleExpr::create(r, l);
}

ref<Expr> SgtExpr::create(const ref<Expr> &l, const ref<Expr> &r) {
  return SltExpr::create(r, l);
}
ref<Expr> SgeExpr::create(const ref<Expr> &l, const ref<Expr> &r) {
  return SleExpr::create(r, l);
}

static ref<Expr> UltExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  Expr::Width t = l->getWidth();
  if (t == Expr::Bool) { // !l && r
    return AndExpr::create(Expr::createIsZero(l), r);
  } else {
    return UltExpr::alloc(l, r);
  }
}

static ref<Expr> UleExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l->getWidth() == Expr::Bool) { // !(l && !r)
    return OrExpr::create(Expr::createIsZero(l), r);
  } else {
    return UleExpr::alloc(l, r);
  }
}

static ref<Expr> SltExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l->getWidth() == Expr::Bool) { // l && !r
    return AndExpr::create(l, Expr::createIsZero(r));
  } else {
    return SltExpr::alloc(l, r);
  }
}

static ref<Expr> SleExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l->getWidth() == Expr::Bool) { // !(!l && r)
    return OrExpr::create(l, Expr::createIsZero(r));
  } else {
    return SleExpr::alloc(l, r);
  }
}

CMPCREATE_T(EqExpr, Eq, EqExpr, EqExpr_createPartial, EqExpr_createPartialR)
CMPCREATE(UltExpr, Ult)
CMPCREATE(UleExpr, Ule)
CMPCREATE(SltExpr, Slt)
CMPCREATE(SleExpr, Sle)

/***/

ref<Expr> ImpliesExpr::create(const ref<Expr> &hyp, const ref<Expr> &conc) {
  assert(hyp->getWidth() == Bool && "iff: type mismatch on hyp");
  assert(conc->getWidth() == Bool && "iff: type mismatch on conc");

  // This could simplify to one of:
  //  - !hyp || (hyp && conc)
  //  - !hyp || conc
  //  - hyp ? conc : True
  // We keep the implies structure because STP can optimize these more readily
  const ref<Expr> &t = Expr::True;
  if (ConstantExpr *hypC = dyn_cast<ConstantExpr>(hyp)) {
    return hypC->isTrue() ? conc : t;
  }
  if (ConstantExpr *concC = dyn_cast<ConstantExpr>(conc)) {
    return concC->isTrue() ? t : Expr::createIsZero(hyp);
  }

  return ImpliesExpr::alloc(hyp, conc);
}

ref<Expr> ImpliesExpr::simplify() const {
  return SelectExpr::create(left, right, Expr::True);
}

/***/

static std::map<std::string, unsigned> QVarAllocsByPrefix;

ref<QVarExpr> QVarExpr::alloc(const std::string &namePrefix, Width w) {
  // Uniquify the name and prefix by "$"
  std::string name = "$" + namePrefix + "_" + llvm::utostr_32(++QVarAllocsByPrefix[namePrefix]);
  QVarExpr *qv = new QVarExpr(name, w);
  qv->computeHash();
  return qv;
}

unsigned QVarExpr::computeHash() {
  hashValue = (unsigned)(uintptr_t)this ^ (getWidth() * MAGIC_HASH_CONSTANT);
  return hashValue;
}

/***/

ref<Expr> ForallExpr::create(const ref<QVarExpr> &qv,
                             const ref<Expr> &e,
                             const ForallTrigger &t) {
  // Only bool exprs supported
  assert(e->getWidth() == Expr::Bool);

  // If qv \notin e, then return e
  if (!ExprSearchVisitor(qv).findIn(e))
    return e;

  return ForallExpr::alloc(qv, e, t);
}

ref<Expr> ForallExpr::instantiate(const ref<Expr> &x) const {
  // Replace qvar with x in expr
  return ExprReplaceVisitor(qvar, x, false).visit(expr);
}
