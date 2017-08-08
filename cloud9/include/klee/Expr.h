//===-- Expr.h --------------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_EXPR_H
#define KLEE_EXPR_H

#include "klee/util/Bits.h"
#include "klee/util/Ref.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"

#include "cloud9/Logger.h"
#include "cloud9/Utils.h"

#include <assert.h>
#include <set>
#include <vector>
#include <iosfwd> // FIXME: Remove this!!!

namespace llvm {
  class Type;
  class ICmpInst;
  class raw_ostream;
}

namespace klee {

class Array;
class ConstantExpr;
class ReadExpr;
class UpdateList;

template<class T> class ref;


/// Class representing symbolic expressions.
/**

<b>Expression canonicalization</b>: we define certain rules for
canonicalization rules for Exprs in order to simplify code that
pattern matches Exprs (since the number of forms are reduced), to open
up further chances for optimization, and to increase similarity for
caching and other purposes.

The general rules are:
<ol>
<li> No Expr has all constant arguments.</li>

<li> Booleans:
    <ol type="a">
     <li> \c Ne, \c Ugt, \c Uge, \c Sgt, \c Sge are not used </li>
     <li> The only acceptable operations with boolean arguments are
          \c Not \c And, \c Or, \c Xor, \c Eq, \c Implies
          as well as \c SExt, \c ZExt,
          \c Select and \c NotOptimized. </li>
     <li> The only boolean operation which may involve a constant is
          boolean not (<tt>== false</tt>). </li>
     </ol>
</li>

<li> Linear Formulas:
   <ol type="a">
   <li> For any subtree representing a linear formula, a constant
   term must be on the LHS of the root node of the subtree.  In particular,
   in a BinaryExpr a constant must always be on the LHS.  For example, subtraction
   by a constant c is written as <tt>add(-c, ?)</tt>.  </li>
    </ol>
</li>


<li> Chains are unbalanced to the right </li>

</ol>


<b>Steps required for adding an expr</b>:
   -# Add case to compare
   -# Add case to printKind
   -# Add to ExprVisitor
   -# Add to Parser
   -# Add to STPBuilder
   -# Add to IVC (implied value concretization) if possible

Todo: Shouldn't bool \c Xor just be written as not equal?

*/

class Expr {
public:
  static unsigned count;
  static const unsigned MAGIC_HASH_CONSTANT = 39;
  // Used when we need a "null" const ref<Expr>&.
  static ref<Expr> InvalidExprRef;

  /// The type of an expression is simply its width, in bits.
  typedef unsigned Width;

  static const Width InvalidWidth = 0;
  static const Width Bool = 1;
  static const Width Int8 = 8;
  static const Width Int16 = 16;
  static const Width Int32 = 32;
  static const Width Int64 = 64;
  static const Width Fl80 = 80;

  enum Kind {
    InvalidKind = -1,

    // Primitive

    Constant = 0,

    // Special

    /// Prevents optimization below the given expression.  Used for
    /// testing: make equality constraints that KLEE will not use to
    /// optimize to concretes.
    NotOptimized,

    /// Skip old varexpr, just for deserialization, purge at some point
    Read=NotOptimized+2,
    Select,
    Concat,
    Extract,

    /// Casting,
    ZExt,
    SExt,

    // All subsequent kinds are binary.

    /// Arithmetic
    Add,
    Sub,
    Mul,
    UDiv,
    SDiv,
    URem,
    SRem,

    /// Bit
    Not,
    And,
    Or,
    Xor,
    Shl,
    LShr,
    AShr,

    /// Compare
    Eq,
    Ne,  ///< Not used in canonical form
    Ult,
    Ule,
    Ugt, ///< Not used in canonical form
    Uge, ///< Not used in canonical form
    Slt,
    Sle,
    Sgt, ///< Not used in canonical form
    Sge, ///< Not used in canonical form

    /// Logical
    Implies,
    QVar,
    Forall,

    LastKind=Forall,

    CastKindFirst=ZExt,
    CastKindLast=SExt,
    BinaryKindFirst=Add,
    BinaryKindLast=Implies,
    SimplifiableBinaryKindFirst=Implies,
    SimplifiableBinaryKindLast=Implies,
    CmpKindFirst=Eq,
    CmpKindLast=Sge
  };

  const Kind kind;
  unsigned refCount;

protected:
  unsigned hashValue;

public:
  ///
  /// This is a bit of a kludge: we set this flag when we believe this Expr
  /// contains the value of a symbolic pointer, where we consider a pointer
  /// "symbolic" when it satisfies Expr::isSymbolicRead() and contains a
  /// value of pointer type.  This flag can be set in two ways:
  ///
  /// 1. When we execute LLVM's "inttoptr" instruction on an expression
  ///    that is a pure symbolic read.  In this case, we're conjuring a
  ///    symbolic pointer from a symbolic integer.
  ///
  /// 2. When we execute LLVM's "load" instruction where (a) the target type
  ///    is a pointer, and (b) the expression is a pure symbolic read.
  ///    In this case, we're reading a pure symbolic pointer.
  ///
  /// XXX: This is getting pretty ugly and it would be good to somehow replace
  /// this giant kludge with actual Ptr expressions (the difficulty of that is
  /// handling ptr<->int, both via llvm's ptrtoint/inttoptr, and via loading
  /// an integer value from memory in a context where a ptr value was expected).
  ///
  bool isSymbolicPointer;

  /// Sets the isSymbolicPointerFlag.  If this is a ConcatExpr, the flag is
  /// set in all children, the idea being to set the flag in each byte of the
  /// expression so that when individual bytes are extracted, the flag will
  /// be passed onwards.
  void setIsSymbolicPointer();

  /// Cached results for klee::findReads()
  typedef llvm::SmallVector<ref<ReadExpr>,8> CachedReadsTy;
  CachedReadsTy *cachedReads;

protected:
  explicit Expr(Kind k)
    : kind(k), refCount(0), isSymbolicPointer(false), cachedReads(NULL)
  { Expr::count++; }

public:
  virtual ~Expr() {
    Expr::count--;
    if (cachedReads)
      delete cachedReads;
  }

  Kind getKind() const { return kind; }

  virtual Width getWidth() const = 0;
  virtual unsigned getNumKids() const = 0;

  // This will assert-fail when given an out-of-range index
  virtual const ref<Expr>& getKid(unsigned i) const = 0;

  void print(std::ostream &os) const;
  void print(llvm::raw_ostream &os) const;

  /// Computes the number of nodes (this node + all children)
  /// For ReadExprs: doesn't count updates
  size_t getSize() const;

  /// dump - Print the expression to stderr.
  void dump() const;

  /// Returns the pre-computed hash of the current expression
  unsigned hash() const { return hashValue; }

  /// (Re)computes the hash of the current expression.
  /// Returns the hash value.
  virtual unsigned computeHash();

  /// Returns 0 iff b is structuraly equivalent to *this
  int compare(const Expr &b) const;
  virtual int compareContents(const Expr &b) const { return 0; }

  // Given an array of new kids return a copy of the expression
  // but using those children.
  virtual ref<Expr> rebuild(ref<Expr> kids[/* getNumKids() */]) const = 0;

  //

  /// isZero - Is this a constant zero.
  bool isZero() const;

  /// isOne - Is this a constant one.
  bool isOne() const;

  /// isTrue - Is this the true expression.
  bool isTrue() const;

  /// isFalse - Is this the false expression.
  bool isFalse() const;

  /* Static utility methods */

  /// These are instantiated for both llvm::raw_ostream and std::ostream
  template<class OStream> static void printKind(OStream &os, Kind k);
  template<class OStream> static void printWidth(OStream &os, Expr::Width w);

  /// returns the smallest number of bytes in which the given width fits
  static inline unsigned getMinBytesForWidth(Width w) {
      return (w + 7) / 8;
  }

  /* Utility constants */
  static ref<ConstantExpr> Null;
  static ref<ConstantExpr> True;
  static ref<ConstantExpr> False;
  static ref<ConstantExpr> Zero8;
  static ref<ConstantExpr> Zero32;
  static ref<ConstantExpr> One8;
  static ref<ConstantExpr> One32;
  static void initialize();

  /* Utility creation functions */
  static ref<Expr> createCoerceToPointerType(ref<Expr> e);
  static ref<Expr> createIff(ref<Expr> l, ref<Expr> r);
  static ref<Expr> createImplies(ref<Expr> hyp, ref<Expr> conc);
  static ref<Expr> createIsZero(ref<Expr> e);
  static ref<Expr> createFromICmp(llvm::ICmpInst *icmp, ref<Expr> lhs, ref<Expr> rhs);

  /// Create a read of the given width at offset 0 of the given array.
  /// Uses the endianness specified by Context::get()
  static ref<Expr> createArrayRead(const Array *array, Expr::Width w);
  static ref<Expr> createArrayReadAtIndex(const Array *array, ref<Expr> index, Expr::Width w);
  /// Uses the specified endianness
  static ref<Expr> createArrayReadAtIndex(const Array *array, ref<Expr> index, Expr::Width w,
                                          bool isLittleEndian);

  /// Create a multi-byte read from a given UpdateList
  /// N.B.: It is assumed that isSymbolicPointer == false
  static ref<Expr> createMultibyteRead(const UpdateList &updates, ref<Expr> index, const unsigned nbytes);
  static ref<Expr> createMultibyteRead(const UpdateList &updates, ref<Expr> index, const unsigned nbytes,
                                       const bool isLittleEndian);

  /// Create a value of pointer size.
  static ref<ConstantExpr> createPointer(uint64_t v);

  struct CreateArg;
  static ref<Expr> createFromKind(Kind k, std::vector<CreateArg> args);

  /// Is 'sub' nested in 'e'?
  static bool hasSubExpr(ref<Expr> e, ref<Expr> sub);

  /* Utilities for dealing with ReadExprs */

  /// A "symbolic read" is an expression containing reads and concats only.
  /// The reads need not be of the same Array.
  static bool isSymbolicRead(ref<Expr> e);
  static ref<ReadExpr> getLowestByteOfSymbolicRead(ref<Expr> e);
  static ref<Expr> removeUpdatesFromSymbolicRead(ref<Expr> e);

  /// hasOrderedReads() is like isSymbolicRead(), but with a stronger
  /// condition: the expression must either be a single read, or a concat
  /// of reads that read from consecutive offsets in a single Array.
  ///
  /// Stride must be 1 or -1.  This returns the base ReadExpr if the
  /// expression has ordered reads, and otherwise returns NULL.  The base
  /// is determined by the given stride: it is the first Read for 1 (MSB),
  /// and the last Read for -1 (LSB).
  ///
  /// In the second form, stride is determined automatically based on the
  /// endianness specified by Context::get().
  static ref<ReadExpr> hasOrderedReads(ref<Expr> e, int stride);
  static ref<ReadExpr> hasOrderedReads(ref<Expr> e);

  /* Kind utilities */
  static bool isValidKidWidth(unsigned kid, Width w) { return true; }
  static bool needsResultType() { return false; }

  static bool classof(const Expr *) { return true; }
};

struct Expr::CreateArg {
  ref<Expr> expr;
  Width width;

  CreateArg(Width w = Bool) : expr(0), width(w) {}
  CreateArg(ref<Expr> e) : expr(e), width(Expr::InvalidWidth) {}

  bool isExpr() { return !isWidth(); }
  bool isWidth() { return width != Expr::InvalidWidth; }
};

typedef std::pair<ref<Expr>, ref<Expr> > ExprPair;

// Comparison operators

inline bool operator==(const Expr &lhs, const Expr &rhs) {
  return lhs.compare(rhs) == 0;
}

inline bool operator<(const Expr &lhs, const Expr &rhs) {
  return lhs.compare(rhs) < 0;
}

inline bool operator!=(const Expr &lhs, const Expr &rhs) {
  return !(lhs == rhs);
}

inline bool operator>(const Expr &lhs, const Expr &rhs) {
  return rhs < lhs;
}

inline bool operator<=(const Expr &lhs, const Expr &rhs) {
  return !(lhs > rhs);
}

inline bool operator>=(const Expr &lhs, const Expr &rhs) {
  return !(lhs < rhs);
}

// Printing operators
// Ugly: need templates to support both llvm::raw_ostream and std::ostream

template<class OStream>
inline OStream& operator<<(OStream &os, const Expr &e) {
  e.print(os);
  return os;
}

inline std::ostream& operator<<(std::ostream &os, const Expr::Kind kind) {
  Expr::printKind(os, kind);
  return os;
}

//-----------------------------------------------------------------------------
// Terminal Exprs
//-----------------------------------------------------------------------------

class ConstantExpr : public Expr {
public:
  static const Kind MyKind = Constant;
  static const unsigned MyNumKids = 0;

  // N.B.: Must put Width first since (-1) and (-2) are invalid widths
  // See DenseMapInfo<std::pair>
  typedef std::pair<Expr::Width, uint64_t> CacheKeyTy;
  typedef llvm::DenseMap<CacheKeyTy, ref<ConstantExpr> > CacheTy;

private:
  llvm::APInt value;
  ConstantExpr(const llvm::APInt &v) : Expr(Constant), value(v) {}

  static const size_t MaxCacheSize = (4<<20) / sizeof(llvm::APInt);  // ~4 MB
  static CacheTy Cache;

  static ref<ConstantExpr> allocNew(const llvm::APInt &v) {
    ref<ConstantExpr> r(new ConstantExpr(v));
    r->computeHash();
    return r;
  }

  static ref<ConstantExpr> allocViaCache(const llvm::APInt &v, bool forced = false) {
    // Use the cache only if width <= 64 (otherwise getZExtValue() will fail)
    const Width w = v.getBitWidth();
    if (w > 64)
      return allocNew(v);

    // Cache lookup
    CacheKeyTy key(w, v.getZExtValue());
    CacheTy::iterator it = Cache.find(key);
    if (it == Cache.end()) {
      ref<ConstantExpr> r = allocNew(v);
      if (forced || Cache.size() <= MaxCacheSize)
        Cache[key] = r;
      return r;
    } else {
      assert(it->second->getAPValue() == v);
      return it->second;
    }
  }

public:
  ~ConstantExpr() {}

  Width getWidth() const { return value.getBitWidth(); }

  unsigned getNumKids() const { return 0; }
  const ref<Expr>& getKid(unsigned i) const {
    assert(0);
    return InvalidExprRef;
  }

  /// getAPValue - Return the arbitrary precision value directly.
  ///
  /// Clients should generally not use the APInt value directly and instead use
  /// native ConstantExpr APIs.
  const llvm::APInt &getAPValue() const { return value; }

  /// getZExtValue - Return the unsigned constant value for a limited number of bits.
  ///
  /// This routine should be used in situations where the width of the constant
  /// is known to be limited to a certain number of bits.
  uint64_t getZExtValue(unsigned bits = 64) const {
    assert(getWidth() <= bits && "Value may be out of range!");
    return value.getZExtValue();
  }

  /// getSExtValue - Return the signed constant value for a limited number of bits.
  ///
  /// This routine should be used in situations where the width of the constant
  /// is known to be limited to a certain number of bits.
  int64_t getSExtValue(unsigned bits = 64) const {
    assert(getWidth() <= bits && "Value may be out of range!");
    return value.getSExtValue();
  }

  /// getLimitedValue - If this value is smaller than the specified limit,
  /// return it, otherwise return the limit value.
  uint64_t getLimitedValue(uint64_t Limit = ~0ULL) const {
    return value.getLimitedValue(Limit);
  }

  /// toString - Return the constant value as a decimal string.
  void toString(std::string &Res) const;

  int compareContents(const Expr &b) const {
    const ConstantExpr &cb = static_cast<const ConstantExpr&>(b);
    if (getWidth() != cb.getWidth())
      return getWidth() < cb.getWidth() ? -1 : 1;
    if (value == cb.value)
      return 0;
    return value.ult(cb.value) ? -1 : 1;
  }

  virtual ref<Expr> rebuild(ref<Expr> kids[]) const {
    assert(0 && "rebuild() on ConstantExpr");
    return (Expr*) this;
  }

  virtual unsigned computeHash();

  static ref<Expr> fromMemory(void *address, Width w);
  void toMemory(void *address);

  static ref<ConstantExpr> alloc(const llvm::APInt &v) {
    return allocViaCache(v);
  }

  static ref<ConstantExpr> alloc(const llvm::APFloat &f) {
    return alloc(f.bitcastToAPInt());
  }

  static ref<ConstantExpr> alloc(uint64_t v, Width w) {
    return alloc(llvm::APInt(w, v));
  }

  static ref<ConstantExpr> create(uint64_t v, Width w) {
    // Ugly: this assertion fails for negative ints: use alloc() in that case
    assert(v == bits64::truncateToNBits(v, w) && "invalid constant");
    return alloc(v, w);
  }

  static ref<ConstantExpr> createCached(uint64_t v, Width w) {
    // Ugly: this assertion fails for negative ints: use alloc() in that case
    assert(v == bits64::truncateToNBits(v, w) && "invalid constant");
    return allocViaCache(llvm::APInt(w, v), true);
  }

  static bool classof(const Expr *E) {
    return E->getKind() == Expr::Constant;
  }
  static bool classof(const ConstantExpr *) { return true; }

  /* Utility Functions */

  /// isZero - Is this a constant zero.
  bool isZero() const {
    return value == 0;
  }

  /// isOne - Is this a constant one.
  bool isOne() const {
    return value == 1;
  }

  /// isTrue - Is this the true expression.
  bool isTrue() const {
    return getZExtValue(1) == 1;
  }

  /// isFalse - Is this the false expression.
  bool isFalse() const {
    return getZExtValue(1) == 0;
  }

  /// isAllOnes - Is this constant all ones.
  bool isAllOnes() const {
    return value.isAllOnesValue();
  }

  /* Constant Operations */

  ref<ConstantExpr> Concat(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> Extract(unsigned offset, Width W);
  ref<ConstantExpr> ZExt(Width W);
  ref<ConstantExpr> SExt(Width W);
  ref<ConstantExpr> Add(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> Sub(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> Mul(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> UDiv(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> SDiv(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> URem(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> SRem(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> And(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> Or(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> Xor(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> Shl(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> LShr(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> AShr(const ref<ConstantExpr> &RHS);

  // Comparisons return a constant expression of width 1.

  ref<ConstantExpr> Eq(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> Ne(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> Ult(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> Ule(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> Ugt(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> Uge(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> Slt(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> Sle(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> Sgt(const ref<ConstantExpr> &RHS);
  ref<ConstantExpr> Sge(const ref<ConstantExpr> &RHS);

  ref<ConstantExpr> Neg();
  ref<ConstantExpr> Not();
};

//-----------------------------------------------------------------------------
// Non-terminals
//-----------------------------------------------------------------------------

class NonConstantExpr : public Expr {
public:
  static bool classof(const Expr *E) {
    return E->getKind() != Expr::Constant;
  }
  static bool classof(const NonConstantExpr *) { return true; }

protected:
  NonConstantExpr(Kind k) : Expr(k) {}
};


//-----------------------------------------------------------------------------
// Quantification
//-----------------------------------------------------------------------------

/// Quantifier variable
class QVarExpr : public NonConstantExpr {
public:
  static const Kind MyKind = QVar;
  static const unsigned MyNumKids = 0;

public:
  const std::string name;
  const Width width;

public:
  static ref<QVarExpr> alloc(const std::string &namePrefix, Width w);

  Width getWidth() const { return width; }
  unsigned getNumKids() const { return MyNumKids; }
  const ref<Expr>& getKid(unsigned i) const { assert(0); return InvalidExprRef; }

  virtual ref<Expr> rebuild(ref<Expr> kids[]) const {
    assert(0 && "rebuild() on QVarExpr");
    return (Expr*) this;
  }

  virtual unsigned computeHash();

private:
  QVarExpr(const std::string &n, Width w)
    : NonConstantExpr(QVar), name(n), width(w)
  {}

public:
  static bool classof(const Expr *E) { return E->getKind() == Expr::QVar; }
  static bool classof(const QVarExpr *) { return true; }
};

/// Triggers for universal quantification
/// This tells the solver when to instantiate quantifiers
class ForallTrigger {
public:
  enum Type {
    // For each (read A x), where A \in this.arrays[], we instantiate
    // the quantifier with qvar = x
    OnArrayReads
  };
  enum {
    MaxArrays = 2
  };

  Type type;
  const Array *arrays[MaxArrays];

public:
  static ForallTrigger onArrayReads(const Array *a0, const Array *a1 = NULL) {
    assert(a0);
    ForallTrigger t;
    t.type = OnArrayReads;
    t.arrays[0] = a0;
    t.arrays[1] = a1;
    return t;
  }

private:
  ForallTrigger() {}
};

/// Universal quantification
/// These are always boolean expressions
class ForallExpr : public NonConstantExpr {
public:
  static const Kind MyKind = Forall;
  static const unsigned MyNumKids = 2;

public:
  /// (Forall qvar. expr)
  const ref<Expr> qvar;  // XXX: ugly: can't make this a QVarExpr due to getKid()
  const ref<Expr> expr;
  const ForallTrigger trigger;

public:
  static ref<ForallExpr> alloc(const ref<QVarExpr> &qv, const ref<Expr> &e,
                               const ForallTrigger &t) {
    assert(e->getWidth() == Expr::Bool);
    ref<ForallExpr> r(new ForallExpr(qv, e, t));
    r->computeHash();
    return r;
  }

  static ref<Expr> create(const ref<QVarExpr> &qv, const ref<Expr> &e,
                          const ForallTrigger &t);

  Width getWidth() const { return Expr::Bool; }
  unsigned getNumKids() const { return MyNumKids; }
  const ref<Expr>& getKid(unsigned i) const {
    switch(i) {
    case 0: return qvar;
    case 1: return expr;
    default: assert(0); return InvalidExprRef;
    }
  }

  virtual ref<Expr> rebuild(ref<Expr> kids[]) const {
    return create(cast<QVarExpr>(kids[0]), kids[1], trigger);
  }

  // Return expr.replace(qvar=x)
  ref<Expr> instantiate(const ref<Expr> &x) const;

private:
  ForallExpr(const ref<QVarExpr> &qv, const ref<Expr> &e, const ForallTrigger &t)
    : NonConstantExpr(Forall), qvar(qv), expr(e), trigger(t)
  {}

public:
  static bool classof(const Expr *E) { return E->getKind() == Expr::Forall; }
  static bool classof(const ForallExpr *) { return true; }
};


//-----------------------------------------------------------------------------
// Symbolic Reads
//-----------------------------------------------------------------------------

class Array;

/// Class representing an update of an array.
class UpdateNode {
  friend class UpdateList;
  friend class STPBuilder; // for setting stpArray

  // manually-maintained reference count
  // see ~UpdateList() for why we don't use ref<>
  mutable unsigned refCount;
  // gross (used by STPBuilder only)
  mutable void *stpArray;
  // hash of this node plus all next nodes
  unsigned _hashValue;
  // size of this update sequence, including this update
  unsigned _sequenceLength;

private:
  // The update applies when guard resolve to true.
  // This is an external condition that is often just Expr::True.
  const ref<Expr> _guard;

  // For single-byte updates
  const ref<Expr> _byteIndex;
  const ref<Expr> _byteValue;

  // For multi-byte updates
  // These are used to represent "memset" and "memcpy" operations
  // (see the Array types ConstantFunc and Copy, respectively).
  //
  // N.B.: If guard=true, this canonicalizes to UpdateList(src,NULL).
  // We assume this canonicalization is done externally, so we do NOT
  // allow multi-byte updates with guard=true.
  //
  // We could actually eliminate multi-byte updates as follows, where
  // "un" is a multi-byte update node and "ul" is its parent UpdateList:
  //
  //    arr = Array::createCopy()
  //    where arr.guard = AndExpr::create(un->guard, un->src->guard)
  //          arr.copySrc = UpdateList(un->src, NULL)
  //          arr.copyOffset = Expr::Zero
  //          arr.nextSrc = UpdateList(ul.root, un->next)
  //    un->prev->next = NULL  // remove "un" from "ul"
  //    ul.root = arr
  //
  // We support multi-byte UpdateNodes to allow us to refer to the same
  // Array under various guarded conditions.  Hopefully this increases
  // sharing and simplifies things for the solver (TODO: check if true)
  const Array *_src;

public:
  const UpdateNode *const next;

public:
  unsigned getSize() const { return _sequenceLength; }
  unsigned hash() const { return _hashValue; }

  // Build an expression that resolves to "true" when the given
  // index lies within the range specified by this UpdateNode.
  // Namely,
  //
  //   single-byte: guard && (index == byteIndex)
  //   multi-byte:  guard
  //
  ref<Expr> getGuardForIndex(const ref<Expr> &index) const;

  // Build an expression that reads from this UpdateNode at "index".
  // This is meaningless except when the expression returned by
  // getGuardForIndex(index) resolve to "true" on the current path.
  // XXX: The "expectIsSymbolicPtr" parameter is ugly ...
  ref<Expr> getValueAtIndex(const ref<Expr> &index,
                            const bool expectIsSymbolicPtr = false) const;

  // Attempts to extract a multi-byte value starting at "index".
  // The return value is a pair that is either:
  //
  //   * (NULL, NULL) :: could not extract "totalBytes"
  //   * (val, guard) :: val has width "totalBytes*8", and val
  //     is meaningless when the guard does not resolve to "true"
  //
  // For single-byte updates, we walk the next nodes and merge byteValues
  // to construct a value of size "totalBytes".
  //
  // For multi-byte updates, we read the entire value from "src".
  //
  ExprPair extractMultiByteValue(const ref<Expr> &index,
                                 const unsigned totalBytes,
                                 const bool isLittleEndian) const;

  // Returns true iff this is a single-byte write of a known value
  bool isSingleByte() const { return _byteIndex.get(); }

  // Returns true iff this is a multi-byte write
  bool isMultiByte() const { return _src != NULL; }

  // Simple accessors (used rarely)
  const ref<Expr>& getGuard() const { return _guard; }
  const ref<Expr>& getByteIndex() const { return _byteIndex; }
  const ref<Expr>& getByteValue() const { return _byteValue; }
  const Array* getSrcArray() const { return _src; }

private:
  // Copy another update
  UpdateNode(const UpdateNode *next,
             const UpdateNode &copy);

  // Single-byte update
  UpdateNode(const UpdateNode *next,
             const ref<Expr> &byteIndex,
             const ref<Expr> &byteValue,
             const ref<Expr> &guard);

  // Multi-byte update
  UpdateNode(const UpdateNode *next,
             const Array *src,
             const ref<Expr> &guard);

  ~UpdateNode();
  void initialize();
  unsigned computeHash();
  int compareContents(const UpdateNode *bn) const;
};

/// Class representing a complete list of updates into an array.
class UpdateList {
  void replaceHead(const UpdateNode *newhead);
  void deleteNodes();

public:
  /// the initial array
  const Array *root;
  /// pointer to the most recent update node
  const UpdateNode *head;

public:
  UpdateList() : root(NULL), head(NULL) {}
  UpdateList(const Array *_root, const UpdateNode *_head);
  UpdateList(const UpdateList &b);
  ~UpdateList();

  UpdateList &operator=(const UpdateList &b);
  bool operator==(const UpdateList &b) const { return compare(b) == 0; }
  bool operator!=(const UpdateList &b) const { return compare(b) != 0; }
  bool operator<(const UpdateList &b) const { return compare(b) < 0; }

  /// Number of updates in this list
  unsigned getSize() const;

  /// Copy of another update
  void addUpdate(const UpdateNode &copy);

  /// Single-byte update, aka root[index] = value
  void addSingleByteUpdate(const ref<Expr> &byteIndex,
                           const ref<Expr> &byteValue,
                           const ref<Expr> guard = Expr::True);

  /// Multi-byte update
  void addMultiByteUpdate(const Array *src,
                          const ref<Expr> &guard);

  int compare(const UpdateList &b) const;
  unsigned hash() const;
};

/// Class representing an array of bytes
/// FIXME: We never delete Arrays: they are leaked
class Array {
public:
  /// The name for this array. Names should generally be unique across an app,
  /// but this is not necessary for correctness, except when printing expressions.
  /// When expressions are printed the output will not parse correctly since two
  /// arrays with the same name cannot be distinguished once printed (but see the
  /// --debug-print-array-addrs option for ExprPPrinter).
  const std::string name;

  /// For STPBuilder
  // FIXME: This does not belong here.
  mutable void *stpInitialArray;

  /// The type of array
  enum Type {
    Symbolic,
    ConstantInitialValues,
    ConstrainedInitialValues
  };
  const Type type;

private:
  unsigned _hashValue;

  /// For Type==ConstantInitialValues
  /// The constant initial values for this array, or empty for a symbolic
  /// array.  If non-empty, each constantValues[i] gives the initial values
  /// for index i.  This should specify ALL initial values for the array:
  /// i.e., when non-empty, we assume that arraySize = constantValues.size().
  typedef std::vector< ref<ConstantExpr> > ConstantInitialValuesTy;
  ConstantInitialValuesTy _constantInitialValues;

  /// For Type==ConstrainedInitialValues,
  /// The constraint on this array is:
  ///
  ///    (forall i. ite(guard, thisArray[i] == valueExpr, thisArray[i] == nextSrc[i]))
  ///
  /// Guards make arrays "conditional": when the guard resolves to true, the
  /// array has value "valueExpr", and otherwise it has value "nextSrc[i]".
  ///
  /// It seems redundant to have guard/nextSrc here when we also have a
  /// guard/next at each UpdateNode.  However, the guard used here is
  /// allowed to refer to parameter "i" specifying the index of the access
  /// (as above), which allows us to constrain this Array to refer to only
  /// a subset of indices, while the guards on UpdateNodes cannot refer to
  /// an index in the same way.  This is perhaps ugly, and we should fold
  /// this functionality into UpdateNodes.  Oh well.
  ///
  /// For createConstant(), valueExpr is given directly.
  /// For createCopy(), valueExpr is read(copySrc, qvar+copyOffset).
  const ref<Expr> _valueExpr;
  const ref<Expr> _guard;
  const UpdateList _nextSrc;

  /// This is the constraint described above
  /// Note that valueExpr and guard may both make use of constraint.qvar
  const ref<ForallExpr> _constraint;

public:
  ~Array();

  /// Create an array with purely symbolic initial contents
  static const Array* createSymbolic(const std::string &name);

  /// Create an array with constant initial contents given by the initial vector
  static const Array* createWithConstantValues(const std::string &name,
                                               const ref<ConstantExpr> *cvBegin,
                                               const ref<ConstantExpr> *csEnd);

  /// Create a "constant" array, meaning that all indices have the same value,
  /// which is given by "valueExpr" (which need not be a ConstantExpr).  We need
  /// a ConstraintManager to emit a forall constraint describing the contents of
  /// this array.
  static const Array* createConstant(const std::string &name,
                                     const ref<Expr> &valueExpr,
                                     ref<QVarExpr> qvar = NULL,
                                     ref<Expr> guard = Expr::True,
                                     UpdateList nextSrc = UpdateList(NULL,NULL));

  /// Create a "copy" of an array: all indices A[i] have the value src[i+offset].
  /// We need a ConstraintManager to emit a forall constraint describing the contents
  /// of this array.
  static const Array* createCopy(const std::string &name,
                                 const UpdateList &copySrc,
                                 const ref<Expr> &offset,
                                 ref<QVarExpr> qvar = NULL,
                                 ref<Expr> guard = Expr::True,
                                 UpdateList nextSrc = UpdateList(NULL,NULL));

  /// Array[Domain]->Range
  static Expr::Width getDomain() { return Expr::Int32; }
  static Expr::Width getRange() { return Expr::Int8; }

  /// Build an expression that reads from this array at "index".  This constructs
  /// the general expression ite(guard, array[i], next[i]), which can often simplify
  /// to a constant value.
  // XXX: The "expectIsSymbolicPtr" parameter is ugly ...
  ref<Expr> getValueAtIndex(const ref<Expr> &index,
                            const bool expectIsSymbolicPtr = false) const;

  /// For Type==ConstrainedInitialValues
  /// Returns a ForallExpr that constrains the initial containts of this array
  ref<ForallExpr> getConstraint() const { return _constraint; }

  /// Shorthand
  bool isSymbolicArray() const { return type == Symbolic; }

  /// Simple accessors (used rarely)
  unsigned hash() const { return _hashValue; }
  const UpdateList& getNextSrc() const { return _nextSrc; }
  const ConstantInitialValuesTy& getConstantInitialValues() const { return _constantInitialValues; }

  /// Comparison
  int compare(const Array *b) const;

private:
  // Type == Symbolic
  Array(const std::string &_name);

  // Type == ConstantInitialValues
  Array(const std::string &_name,
        const ref<ConstantExpr> *cvBegin,
        const ref<ConstantExpr> *cvEnd);

  // Type == ConstrainedInitialValues
  Array(const std::string &_name,
        const ref<Expr> valueExpr,
        const ref<Expr> guard,
        const UpdateList nextSrc,
        const ref<QVarExpr> qvar);

  void computeHash();
};

/// Class representing a one byte read from an array.
class ReadExpr : public NonConstantExpr {
public:
  static const Kind MyKind = Read;
  static const unsigned MyNumKids = 1;

public:
  const UpdateList updates;
  const ref<Expr> index;

public:
  static ref<ReadExpr> alloc(const UpdateList &updates, const ref<Expr> &index,
                             const bool isSymbolicPtr) {
    assert(index->getWidth() == Array::getDomain());
    ref<ReadExpr> r(new ReadExpr(updates, index, isSymbolicPtr));
    r->computeHash();
    return r;
  }

  static ref<Expr> create(const UpdateList &updates, ref<Expr> i,
                          const bool isSymbolicPtr = false);

  Width getWidth() const { return Expr::Int8; }
  unsigned getNumKids() const { return MyNumKids; }
  const ref<Expr>& getKid(unsigned i) const {
    switch(i) {
    case 0: return index;
    default: assert(0); return InvalidExprRef;
    }
  }

  int compareContents(const Expr &b) const;

  virtual ref<Expr> rebuild(ref<Expr> kids[]) const {
    return create(updates, kids[0], isSymbolicPointer);
  }

  virtual unsigned computeHash();

private:
  ReadExpr(const UpdateList &_updates, const ref<Expr> &_index, const bool _isSymbolicPtr)
    : NonConstantExpr(Read), updates(_updates), index(_index) {
    assert(updates.root);
    isSymbolicPointer = _isSymbolicPtr;
  }

public:
  static bool classof(const Expr *E) { return E->getKind() == Expr::Read; }
  static bool classof(const ReadExpr *) { return true; }
};


//-----------------------------------------------------------------------------
// Special
//-----------------------------------------------------------------------------

class NotOptimizedExpr : public NonConstantExpr {
public:
  static const Kind MyKind = NotOptimized;
  static const unsigned MyNumKids = 1;

public:
  const ref<Expr> src;

public:
  static ref<NotOptimizedExpr> alloc(const ref<Expr> &src) {
    ref<NotOptimizedExpr> r(new NotOptimizedExpr(src));
    r->computeHash();
    return r;
  }

  static ref<Expr> create(ref<Expr> src);

  Width getWidth() const { return src->getWidth(); }
  unsigned getNumKids() const { return MyNumKids; }
  const ref<Expr>& getKid(unsigned i) const {
    switch(i) {
    case 0: return src;
    default: assert(0); return InvalidExprRef;
    }
  }

  virtual ref<Expr> rebuild(ref<Expr> kids[]) const { return create(kids[0]); }

private:
  NotOptimizedExpr(const ref<Expr> &_src) : NonConstantExpr(NotOptimized), src(_src) {}

public:
  static bool classof(const Expr *E) { return E->getKind() == Expr::NotOptimized; }
  static bool classof(const NotOptimizedExpr *) { return true; }
};


/// Class representing an if-then-else expression.
class SelectExpr : public NonConstantExpr {
public:
  static const Kind MyKind = Select;
  static const unsigned MyNumKids = 3;

public:
  const ref<Expr> cond, trueExpr, falseExpr;

public:
  static ref<SelectExpr> alloc(const ref<Expr> &c, const ref<Expr> &t,
                               const ref<Expr> &f) {
    ref<SelectExpr> r(new SelectExpr(c, t, f));
    r->computeHash();
    return r;
  }

  static ref<Expr> create(ref<Expr> c, ref<Expr> t, ref<Expr> f);

  Width getWidth() const { return trueExpr->getWidth(); }
  unsigned getNumKids() const { return MyNumKids; }
  const ref<Expr>& getKid(unsigned i) const {
    switch(i) {
    case 0: return cond;
    case 1: return trueExpr;
    case 2: return falseExpr;
    default: assert(0); return InvalidExprRef;
    }
  }

  static bool isValidKidWidth(unsigned kid, Width w) {
    if (kid == 0)
      return w == Bool;
    else
      return true;
  }

  virtual ref<Expr> rebuild(ref<Expr> kids[]) const {
    return create(kids[0], kids[1], kids[2]);
  }

private:
  SelectExpr(const ref<Expr> &c, const ref<Expr> &t, const ref<Expr> &f)
    : NonConstantExpr(Select), cond(c), trueExpr(t), falseExpr(f) {}

public:
  static bool classof(const Expr *E) { return E->getKind() == Expr::Select; }
  static bool classof(const SelectExpr *) { return true; }
};


/// Children of a concat expression can have arbitrary widths.
/// Kid 0 is the left kid, kid 1 is the right kid.
class ConcatExpr : public NonConstantExpr {
public:
  static const Kind MyKind = Concat;
  static const unsigned MyNumKids = 2;

public:
  const Width width;
  const ref<Expr> left, right;

public:
  static ref<ConcatExpr> alloc(const ref<Expr> &l, const ref<Expr> &r) {
    ref<ConcatExpr> c(new ConcatExpr(l, r));
    c->computeHash();
    return c;
  }

  static ref<Expr> create(const ref<Expr> &l, const ref<Expr> &r);

  Width getWidth() const { return width; }
  unsigned getNumKids() const { return MyNumKids; }
  const ref<Expr>& getKid(unsigned i) const {
    switch(i) {
    case 0: return left;
    case 1: return right;
    default: assert(0); return InvalidExprRef;
    }
  }

  /// Shortcuts to create larger concats.  The chain returned is unbalanced to the right
  static ref<Expr> createN(unsigned nKids, const ref<Expr> kids[]);
  static ref<Expr> create4(const ref<Expr> &kid1, const ref<Expr> &kid2,
                           const ref<Expr> &kid3, const ref<Expr> &kid4);
  static ref<Expr> create8(const ref<Expr> &kid1, const ref<Expr> &kid2,
                           const ref<Expr> &kid3, const ref<Expr> &kid4,
                           const ref<Expr> &kid5, const ref<Expr> &kid6,
                           const ref<Expr> &kid7, const ref<Expr> &kid8);

  virtual ref<Expr> rebuild(ref<Expr> kids[]) const { return create(kids[0], kids[1]); }

private:
  ConcatExpr(const ref<Expr> &l, const ref<Expr> &r)
    : NonConstantExpr(Concat), width(l->getWidth() + r->getWidth()), left(l), right(r) {
    if (l->isSymbolicPointer && r->isSymbolicPointer)
      isSymbolicPointer = true;
  }

public:
  static bool classof(const Expr *E) { return E->getKind() == Expr::Concat; }
  static bool classof(const ConcatExpr *) { return true; }
};


/// This class represents an extract from expression {\tt expr}, at
/// bit offset {\tt offset} of width {\tt width}.  Bit 0 is the right-most
/// bit of the expression.
class ExtractExpr : public NonConstantExpr {
public:
  static const Kind MyKind = Extract;
  static const unsigned MyNumKids = 1;

public:
  const ref<Expr> expr;
  const unsigned offset;
  const Width width;

public:
  static ref<ExtractExpr> alloc(const ref<Expr> &e, unsigned o, Width w) {
    ref<ExtractExpr> r(new ExtractExpr(e, o, w));
    r->computeHash();
    return r;
  }

  /// Creates an ExtractExpr with the given bit offset and width
  static ref<Expr> create(ref<Expr> e, unsigned bitOff, Width w);

  Width getWidth() const { return width; }
  unsigned getNumKids() const { return MyNumKids; }
  const ref<Expr>& getKid(unsigned i) const {
    switch(i) {
    case 0: return expr;
    default: assert(0); return InvalidExprRef;
    }
  }

  int compareContents(const Expr &b) const {
    const ExtractExpr &eb = static_cast<const ExtractExpr&>(b);
    if (offset != eb.offset) return offset < eb.offset ? -1 : 1;
    if (width != eb.width) return width < eb.width ? -1 : 1;
    return 0;
  }

  virtual ref<Expr> rebuild(ref<Expr> kids[]) const {
    return create(kids[0], offset, width);
  }

  virtual unsigned computeHash();

private:
  ExtractExpr(const ref<Expr> &e, unsigned b, Width w)
    : NonConstantExpr(Extract), expr(e), offset(b), width(w) {}

public:
  static bool classof(const Expr *E) { return E->getKind() == Expr::Extract; }
  static bool classof(const ExtractExpr *) { return true; }
};


//-----------------------------------------------------------------------------
// Unary
//-----------------------------------------------------------------------------

/// Bitwise Not
class NotExpr : public NonConstantExpr {
public:
  static const Kind MyKind = Not;
  static const unsigned MyNumKids = 1;

public:
  const ref<Expr> expr;

public:
  static ref<NotExpr> alloc(const ref<Expr> &e) {
    ref<NotExpr> r(new NotExpr(e));
    r->computeHash();
    return r;
  }

  static ref<Expr> create(const ref<Expr> &e);

  Width getWidth() const { return expr->getWidth(); }
  unsigned getNumKids() const { return MyNumKids; }
  const ref<Expr>& getKid(unsigned i) const {
    switch(i) {
    case 0: return expr;
    default: assert(0); return InvalidExprRef;
    }
  }

  virtual ref<Expr> rebuild(ref<Expr> kids[]) const {
    return create(kids[0]);
  }

  virtual unsigned computeHash();

private:
  NotExpr(const ref<Expr> &e) : NonConstantExpr(Not), expr(e) {}

public:
  static bool classof(const Expr *E) { return E->getKind() == Expr::Not; }
  static bool classof(const NotExpr *) { return true; }
};


//-----------------------------------------------------------------------------
// Casting
//-----------------------------------------------------------------------------

class CastExpr : public NonConstantExpr {
public:
  const ref<Expr> src;
  const Width width;

protected:
  CastExpr(Kind k, const ref<Expr> &e, Width w) : NonConstantExpr(k), src(e), width(w) {}

public:
  Width getWidth() const { return width; }
  unsigned getNumKids() const { return 1; }
  const ref<Expr>& getKid(unsigned i) const {
    switch(i) {
    case 0: return src;
    default: assert(0); return InvalidExprRef;
    }
  }

  static bool needsResultType() { return true; }

  int compareContents(const Expr &b) const {
    const CastExpr &eb = static_cast<const CastExpr&>(b);
    if (width != eb.width) return width < eb.width ? -1 : 1;
    return 0;
  }

  virtual unsigned computeHash();

  static bool classof(Kind k) { return Expr::CastKindFirst <= k && k <= Expr::CastKindLast; }
  static bool classof(const Expr *E) { return classof(E->getKind()); }
  static bool classof(const CastExpr *) { return true; }
};

#define CAST_EXPR_CLASS(_class_kind)                                    \
class _class_kind ## Expr : public CastExpr {                           \
public:                                                                 \
  static const Kind MyKind = _class_kind;                               \
  static const unsigned MyNumKids = 1;                                  \
private:                                                                \
  _class_kind ## Expr(const ref<Expr> &e, Width w)                      \
    : CastExpr(_class_kind, e, w) {}                                    \
public:                                                                 \
  static ref<_class_kind ## Expr> alloc(const ref<Expr> &e, Width w) {  \
    ref<_class_kind ## Expr> r(new _class_kind ## Expr(e, w));          \
    r->computeHash();                                                   \
    return r;                                                           \
  }                                                                     \
  static ref<Expr> create(const ref<Expr> &e, Width w);                 \
  virtual ref<Expr> rebuild(ref<Expr> kids[]) const {                   \
    return create(kids[0], width);                                      \
  }                                                                     \
                                                                        \
  static bool classof(const Expr *E) {                                  \
    return E->getKind() == Expr::_class_kind;                           \
  }                                                                     \
  static bool classof(const  _class_kind ## Expr *) {                   \
    return true;                                                        \
  }                                                                     \
};

CAST_EXPR_CLASS(SExt)
CAST_EXPR_CLASS(ZExt)


//-----------------------------------------------------------------------------
// Binary
//-----------------------------------------------------------------------------

class BinaryExpr : public NonConstantExpr {
public:
  const ref<Expr> left, right;

public:
  unsigned getNumKids() const { return 2; }
  const ref<Expr>& getKid(unsigned i) const {
    switch(i) {
    case 0: return left;
    case 1: return right;
    default: assert(0); return InvalidExprRef;
    }
  }

protected:
  BinaryExpr(Kind k, const ref<Expr> &l, const ref<Expr> &r)
    : NonConstantExpr(k), left(l), right(r) {}

public:
  static bool classof(Kind k) {
    return Expr::BinaryKindFirst <= k && k <= Expr::BinaryKindLast;
  }
  static bool classof(const Expr *E) {
    return classof(E->getKind());
  }
  static bool classof(const BinaryExpr *) { return true; }
};

class SimplifiableBinaryExpr : public BinaryExpr {
protected:
  SimplifiableBinaryExpr(Kind k, const ref<Expr> &l, const ref<Expr> &r)
    : BinaryExpr(k, l, r) {}

public:
  /// Produces an expression equivalent to this one, but using simpler operators.
  /// This is a hack to reuse code in non-performance-critical places.
  virtual ref<Expr> simplify() const = 0;

public:
  static bool classof(Kind k) {
    return Expr::SimplifiableBinaryKindFirst <= k && k <= Expr::SimplifiableBinaryKindLast;
  }
  static bool classof(const Expr *E) {
    return classof(E->getKind());
  }
  static bool classof(const BinaryExpr *) { return true; }
  static bool classof(const SimplifiableBinaryExpr *) { return true; }
};

class CmpExpr : public BinaryExpr {
protected:
  CmpExpr(Kind k, ref<Expr> l, ref<Expr> r) : BinaryExpr(k,l,r) {}

public:
  Width getWidth() const { return Bool; }

  static bool classof(const Expr *E) {
    Kind k = E->getKind();
    return Expr::CmpKindFirst <= k && k <= Expr::CmpKindLast;
  }
  static bool classof(const CmpExpr *) { return true; }
};

// Arithmetic/Bit/Logical Exprs

#define ARITHMETIC_EXPR_CLASS(_class_kind)                                        \
class _class_kind ## Expr : public BinaryExpr {                                   \
public:                                                                           \
  static const Kind MyKind = _class_kind;                                         \
  static const unsigned MyNumKids = 2;                                            \
private:                                                                          \
  _class_kind ## Expr(const ref<Expr> &l,                                         \
                      const ref<Expr> &r) : BinaryExpr(_class_kind,l,r) {}        \
public:                                                                           \
  static ref<_class_kind ## Expr> alloc(const ref<Expr> &l, const ref<Expr> &r) { \
    ref<_class_kind ## Expr> res(new _class_kind ## Expr (l, r));                 \
    res->computeHash();                                                           \
    return res;                                                                   \
  }                                                                               \
  static ref<Expr> create(const ref<Expr> &l, const ref<Expr> &r);                \
  Width getWidth() const { return left->getWidth(); }                             \
  virtual ref<Expr> rebuild(ref<Expr> kids[]) const {                             \
    return create(kids[0], kids[1]);                                              \
  }                                                                               \
                                                                                  \
  static bool classof(const Expr *E) {                                            \
    return E->getKind() == Expr::_class_kind;                                     \
  }                                                                               \
  static bool classof(const  _class_kind ## Expr *) {                             \
    return true;                                                                  \
  }                                                                               \
};

ARITHMETIC_EXPR_CLASS(Add)
ARITHMETIC_EXPR_CLASS(Sub)
ARITHMETIC_EXPR_CLASS(Mul)
ARITHMETIC_EXPR_CLASS(UDiv)
ARITHMETIC_EXPR_CLASS(SDiv)
ARITHMETIC_EXPR_CLASS(URem)
ARITHMETIC_EXPR_CLASS(SRem)
ARITHMETIC_EXPR_CLASS(And)
ARITHMETIC_EXPR_CLASS(Or)
ARITHMETIC_EXPR_CLASS(Xor)
ARITHMETIC_EXPR_CLASS(Shl)
ARITHMETIC_EXPR_CLASS(LShr)
ARITHMETIC_EXPR_CLASS(AShr)

// Comparison Exprs

#define COMPARISON_EXPR_CLASS(_class_kind)                                        \
class _class_kind ## Expr : public CmpExpr {                                      \
public:                                                                           \
  static const Kind MyKind = _class_kind;                                         \
  static const unsigned MyNumKids = 2;                                            \
public:                                                                           \
  _class_kind ## Expr(const ref<Expr> &l,                                         \
                      const ref<Expr> &r) : CmpExpr(_class_kind,l,r) {}           \
  static ref<_class_kind ## Expr> alloc(const ref<Expr> &l, const ref<Expr> &r) { \
    ref<_class_kind ## Expr> res(new _class_kind ## Expr (l, r));                 \
    res->computeHash();                                                           \
    return res;                                                                   \
  }                                                                               \
  static ref<Expr> create(const ref<Expr> &l, const ref<Expr> &r);                \
  virtual ref<Expr> rebuild(ref<Expr> kids[]) const {                             \
    return create(kids[0], kids[1]);                                              \
  }                                                                               \
                                                                                  \
  static bool classof(const Expr *E) {                                            \
    return E->getKind() == Expr::_class_kind;                                     \
  }                                                                               \
  static bool classof(const  _class_kind ## Expr *) {                             \
    return true;                                                                  \
  }                                                                               \
};

COMPARISON_EXPR_CLASS(Eq)
COMPARISON_EXPR_CLASS(Ne)
COMPARISON_EXPR_CLASS(Ult)
COMPARISON_EXPR_CLASS(Ule)
COMPARISON_EXPR_CLASS(Ugt)
COMPARISON_EXPR_CLASS(Uge)
COMPARISON_EXPR_CLASS(Slt)
COMPARISON_EXPR_CLASS(Sle)
COMPARISON_EXPR_CLASS(Sgt)
COMPARISON_EXPR_CLASS(Sge)

// Logic Exprs

#define SIMPLIFIABLE_EXPR_CLASS(_class_kind)                                      \
class _class_kind ## Expr : public SimplifiableBinaryExpr {                       \
public:                                                                           \
  static const Kind MyKind = _class_kind;                                         \
  static const unsigned MyNumKids = 2;                                            \
private:                                                                          \
  _class_kind ## Expr(const ref<Expr> &l, const ref<Expr> &r)                     \
                     : SimplifiableBinaryExpr(_class_kind,l,r) {}                 \
public:                                                                           \
  static ref<_class_kind ## Expr> alloc(const ref<Expr> &l, const ref<Expr> &r) { \
    ref<_class_kind ## Expr> res(new _class_kind ## Expr (l, r));                 \
    res->computeHash();                                                           \
    return res;                                                                   \
  }                                                                               \
  static ref<Expr> create(const ref<Expr> &l, const ref<Expr> &r);                \
  ref<Expr> simplify() const;                                                     \
  Width getWidth() const { return left->getWidth(); }                             \
  virtual ref<Expr> rebuild(ref<Expr> kids[]) const {                             \
    return create(kids[0], kids[1]);                                              \
  }                                                                               \
                                                                                  \
  static bool classof(const Expr *E) {                                            \
    return E->getKind() == Expr::_class_kind;                                     \
  }                                                                               \
  static bool classof(const  _class_kind ## Expr *) {                             \
    return true;                                                                  \
  }                                                                               \
};

SIMPLIFIABLE_EXPR_CLASS(Implies)


//-----------------------------------------------------------------------------
// Implementations
//-----------------------------------------------------------------------------

inline bool Expr::isZero() const {
  if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(this))
    return CE->isZero();
  return false;
}

inline bool Expr::isOne() const {
  if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(this))
    return CE->isOne();
  return false;
}

inline bool Expr::isTrue() const {
  assert(getWidth() == Expr::Bool && "Invalid isTrue() call!");
  if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(this))
    return CE->isTrue();
  return false;
}

inline bool Expr::isFalse() const {
  assert(getWidth() == Expr::Bool && "Invalid isFalse() call!");
  if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(this))
    return CE->isFalse();
  return false;
}

} // End klee namespace

#endif
