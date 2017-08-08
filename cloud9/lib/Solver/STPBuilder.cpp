//===-- STPBuilder.cpp ----------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "STPBuilder.h"

#include "klee/Expr.h"
#include "klee/Solver.h"
#include "klee/util/Bits.h"

#include "ConstantDivision.h"
#include "SolverStats.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"

#include <cstdio>

#define vc_bvBoolExtract IAMTHESPAWNOFSATAN
// unclear return
#define vc_bvLeftShiftByConstantExpr IAMTHESPAWNOFSATAN
#define vc_bvRightShiftByConstantExpr IAMTHESPAWNOFSATAN
// bad refcnt'ng
#define vc_bvVar32LeftShiftExpr IAMTHESPAWNOFSATAN
#define vc_bvVar32RightShiftExpr IAMTHESPAWNOFSATAN
#define vc_bvVar32DivByPowOfTwoExpr IAMTHESPAWNOFSATAN
#define vc_bvCreateMemoryArray IAMTHESPAWNOFSATAN
#define vc_bvReadMemoryArray IAMTHESPAWNOFSATAN
#define vc_bvWriteToMemoryArray IAMTHESPAWNOFSATAN
// define to enable old (and possibly wrong?) implementations of variable shift operators
//#define STP_DUMB_SHIFT_OPERATORS

#include <algorithm> // max, min
#include <cassert>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

using namespace klee;

namespace {
  llvm::cl::opt<bool>
  UseConstructHash("use-construct-hash",
                   llvm::cl::desc("Use hash-consing during STP query construction."),
                   llvm::cl::init(true));
}

///

/***/

STPBuilder::STPBuilder(::VC _vc, bool optimizeDivides)
  : vc(_vc), _optimizeDivides(optimizeDivides) {
}

STPBuilder::~STPBuilder() {
}

void STPBuilder::clearCaches() {
  _constructed.clear();
  _onArrayReadWorklist.clear();
}

///

/* Warning: be careful about what c_interface functions you use. Some of
   them look like they cons memory but in fact don't, which is bad when
   you call vc_DeleteExpr on them. */

::VCExpr STPBuilder::buildArray(const char *name, unsigned indexWidth, unsigned valueWidth) {
  // XXX don't rebuild if this stuff cons's
  ::Type t1 = vc_bvType(vc, indexWidth);
  ::Type t2 = vc_bvType(vc, valueWidth);
  ::Type t = vc_arrayType(vc, t1, t2);
  ::VCExpr res = vc_varExpr(vc, const_cast<char*>(name), t);
  vc_DeleteExpr(t);
  vc_DeleteExpr(t2);
  vc_DeleteExpr(t1);
  return res;
}

ExprHandle STPBuilder::getTrue() {
  return vc_trueExpr(vc);
}
ExprHandle STPBuilder::getFalse() {
  return vc_falseExpr(vc);
}
ExprHandle STPBuilder::bvOne(unsigned width) {
  return bvConst32(width, 1);
}
ExprHandle STPBuilder::bvZero(unsigned width) {
  return bvConst32(width, 0);
}
ExprHandle STPBuilder::bvMinusOne(unsigned width) {
  return bvConst64(width, (int64_t) -1);
}
ExprHandle STPBuilder::bvConst32(unsigned width, uint32_t value) {
  return vc_bvConstExprFromInt(vc, width, value);
}
ExprHandle STPBuilder::bvConst64(unsigned width, uint64_t value) {
  return vc_bvConstExprFromLL(vc, width, value);
}

ExprHandle STPBuilder::bvBoolExtract(ExprHandle expr, int bit) {
  return vc_eqExpr(vc, bvExtract(expr, bit, bit), bvOne(1));
}
ExprHandle STPBuilder::bvExtract(ExprHandle expr, unsigned top, unsigned bottom) {
  return vc_bvExtract(vc, expr, top, bottom);
}
ExprHandle STPBuilder::eqExpr(ExprHandle a, ExprHandle b) {
  return vc_eqExpr(vc, a, b);
}

// logical left shift (constant)
ExprHandle STPBuilder::bvLeftShiftByConstant(ExprHandle expr, unsigned amount, unsigned shiftBits) {
  unsigned width = vc_getBVLength(vc, expr);
  unsigned shift = amount & ((1<<shiftBits) - 1);

  if (shift==0) {
    return expr;
  } else if (shift>=width) {
    return bvZero(width);
  } else {
    // stp shift does "expr @ [0 x s]" which we then have to extract,
    // rolling our own gives slightly smaller exprs
    return vc_bvConcatExpr(vc, 
                           bvExtract(expr, width - shift - 1, 0),
                           bvZero(shift));
  }
}

// logical right shift (constant)
ExprHandle STPBuilder::bvRightShiftByConstant(ExprHandle expr, unsigned amount, unsigned shiftBits) {
  unsigned width = vc_getBVLength(vc, expr);
  unsigned shift = amount & ((1<<shiftBits) - 1);

  if (shift==0) {
    return expr;
  } else if (shift>=width) {
    return bvZero(width);
  } else {
    return vc_bvConcatExpr(vc,
                           bvZero(shift),
                           bvExtract(expr, width - 1, shift));
  }
}

// arithmetic right shift (constant)
ExprHandle STPBuilder::bvArithRightShiftByConstant(ExprHandle expr,
                                                   unsigned amount,
                                                   ExprHandle isSigned, 
                                                   unsigned shiftBits) {
  unsigned width = vc_getBVLength(vc, expr);
  unsigned shift = amount & ((1<<shiftBits) - 1);

  if (shift==0) {
    return expr;
  } else if (shift>=width-1) {
    return vc_iteExpr(vc, isSigned, bvMinusOne(width), bvZero(width));
  } else {
    return vc_iteExpr(vc,
                      isSigned,
                      ExprHandle(vc_bvConcatExpr(vc,
                                                 bvMinusOne(shift),
                                                 bvExtract(expr, width - 1, shift))),
                      bvRightShiftByConstant(expr, shift, shiftBits));
  }
}

#ifdef STP_DUMB_SHIFT_OPERATORS

// left shift by a variable amount on an expression of the specified width
ExprHandle STPBuilder::bvLeftShift(ExprHandle expr, ExprHandle amount, unsigned width) {
  ExprHandle res = bvZero(width);

  int shiftBits = getShiftBits( width );

  //get the shift amount (looking only at the bits appropriate for the given width)
  ExprHandle shift = vc_bvExtract( vc, amount, shiftBits - 1, 0 ); 

  //construct a big if-then-elif-elif-... with one case per possible shift amount
  for( int i=width-1; i>=0; i-- ) {
    res = vc_iteExpr(vc,
                     eqExpr(shift, bvConst32(shiftBits, i)),
                     bvLeftShiftByConstant(expr, i, shiftBits),
                     res);
  }
  return res;
}

// logical right shift by a variable amount on an expression of the specified width
ExprHandle STPBuilder::bvRightShift(ExprHandle expr, ExprHandle amount, unsigned width) {
  ExprHandle res = bvZero(width);

  int shiftBits = getShiftBits( width );

  //get the shift amount (looking only at the bits appropriate for the given width)
  ExprHandle shift = vc_bvExtract( vc, amount, shiftBits - 1, 0 ); 

  //construct a big if-then-elif-elif-... with one case per possible shift amount
  for( int i=width-1; i>=0; i-- ) {
    res = vc_iteExpr(vc,
                     eqExpr(shift, bvConst32(shiftBits, i)),
                     bvRightShiftByConstant(expr, i, shiftBits),
                     res);
  }

  return res;
}

// arithmetic right shift by a variable amount on an expression of the specified width
ExprHandle STPBuilder::bvArithRightShift(ExprHandle expr, ExprHandle amount, unsigned width) {
  int shiftBits = getShiftBits( width );

  //get the shift amount (looking only at the bits appropriate for the given width)
  ExprHandle shift = vc_bvExtract( vc, amount, shiftBits - 1, 0 );

  //get the sign bit to fill with
  ExprHandle signedBool = bvBoolExtract(expr, width-1);

  //start with the result if shifting by width-1
  ExprHandle res = bvArithRightShiftByConstant(expr, width-1, signedBool, shiftBits);

  //construct a big if-then-elif-elif-... with one case per possible shift amount
  // XXX more efficient to move the ite on the sign outside all exprs?
  // XXX more efficient to sign extend, right shift, then extract lower bits?
  for( int i=width-2; i>=0; i-- ) {
    res = vc_iteExpr(vc,
                     eqExpr(shift, bvConst32(shiftBits,i)),
                     bvArithRightShiftByConstant(expr, 
                                                 i, 
                                                 signedBool, 
                                                 shiftBits),
                     res);
  }

  return res;
}

#else

// left shift by a variable amount on an expression of the specified width
ExprHandle STPBuilder::bvLeftShift(ExprHandle expr, ExprHandle amount, unsigned width) {
  return vc_bvLeftShiftExprExpr(vc, width, expr, amount);
}

// logical right shift by a variable amount on an expression of the specified width
ExprHandle STPBuilder::bvRightShift(ExprHandle expr, ExprHandle amount, unsigned width) {
  return vc_bvRightShiftExprExpr(vc, width, expr, amount);
}

// arithmetic right shift by a variable amount on an expression of the specified width
ExprHandle STPBuilder::bvArithRightShift(ExprHandle expr, ExprHandle amount, unsigned width) {
  return vc_bvSignedRightShiftExprExpr(vc, width, expr, amount);
}

#endif

ExprHandle STPBuilder::constructMulByConstant(ExprHandle expr, unsigned width, uint64_t x) {
  unsigned shiftBits = getShiftBits(width);
  uint64_t add, sub;
  ExprHandle res = 0;

  // expr*x == expr*(add-sub) == expr*add - expr*sub
  ComputeMultConstants64(x, add, sub);

  // legal, these would overflow completely
  add = bits64::truncateToNBits(add, width);
  sub = bits64::truncateToNBits(sub, width);

  for (int j=63; j>=0; j--) {
    uint64_t bit = 1LL << j;

    if ((add&bit) || (sub&bit)) {
      assert(!((add&bit) && (sub&bit)) && "invalid mult constants");
      ExprHandle op = bvLeftShiftByConstant(expr, j, shiftBits);
      
      if (add&bit) {
        if (res) {
          res = vc_bvPlusExpr(vc, width, res, op);
        } else {
          res = op;
        }
      } else {
        if (res) {
          res = vc_bvMinusExpr(vc, width, res, op);
        } else {
          res = vc_bvUMinusExpr(vc, op);
        }
      }
    }
  }

  if (!res) 
    res = bvZero(width);

  return res;
}

/* 
 * Compute the 32-bit unsigned integer division of n by a divisor d based on 
 * the constants derived from the constant divisor d.
 *
 * Returns n/d without doing explicit division.  The cost is 2 adds, 3 shifts, 
 * and a (64-bit) multiply.
 *
 * @param n      numerator (dividend) as an expression
 * @param width  number of bits used to represent the value
 * @param d      the divisor
 *
 * @return n/d without doing explicit division
 */
ExprHandle STPBuilder::constructUDivByConstant(ExprHandle expr_n, unsigned width, uint64_t d) {
  assert(width==32 && "can only compute udiv constants for 32-bit division");

  // Compute the constants needed to compute n/d for constant d w/o
  // division by d.
  uint32_t mprime, sh1, sh2;
  ComputeUDivConstants32(d, mprime, sh1, sh2);
  ExprHandle expr_sh1    = bvConst32( 32, sh1);
  ExprHandle expr_sh2    = bvConst32( 32, sh2);

  // t1  = MULUH(mprime, n) = ( (uint64_t)mprime * (uint64_t)n ) >> 32
  ExprHandle expr_n_64   = vc_bvConcatExpr( vc, bvZero(32), expr_n ); //extend to 64 bits
  ExprHandle t1_64bits   = constructMulByConstant( expr_n_64, 64, (uint64_t)mprime );
  ExprHandle t1          = vc_bvExtract( vc, t1_64bits, 63, 32 ); //upper 32 bits

  // n/d = (((n - t1) >> sh1) + t1) >> sh2;
  ExprHandle n_minus_t1  = vc_bvMinusExpr( vc, width, expr_n, t1 );
  ExprHandle shift_sh1   = bvRightShift( n_minus_t1, expr_sh1, 32 );
  ExprHandle plus_t1     = vc_bvPlusExpr( vc, width, shift_sh1, t1 );
  ExprHandle res         = bvRightShift( plus_t1, expr_sh2, 32 );

  return res;
}

/* 
 * Compute the 32-bitnsigned integer division of n by a divisor d based on 
 * the constants derived from the constant divisor d.
 *
 * Returns n/d without doing explicit division.  The cost is 3 adds, 3 shifts, 
 * a (64-bit) multiply, and an XOR.
 *
 * @param n      numerator (dividend) as an expression
 * @param width  number of bits used to represent the value
 * @param d      the divisor
 *
 * @return n/d without doing explicit division
 */
ExprHandle STPBuilder::constructSDivByConstant(ExprHandle expr_n, unsigned width, uint64_t d) {
  assert(width==32 && "can only compute sdiv constants for 32-bit division");

  // Compute the constants needed to compute n/d for constant d w/o division by d.
  int32_t mprime, dsign, shpost;
  ComputeSDivConstants32(d, mprime, dsign, shpost);
  ExprHandle expr_dsign   = bvConst32( 32, dsign);
  ExprHandle expr_shpost  = bvConst32( 32, shpost);

  // q0 = n + MULSH( mprime, n ) = n + (( (int64_t)mprime * (int64_t)n ) >> 32)
  int64_t mprime_64     = (int64_t)mprime;

  ExprHandle expr_n_64    = vc_bvSignExtend( vc, expr_n, 64 );
  ExprHandle mult_64      = constructMulByConstant( expr_n_64, 64, mprime_64 );
  ExprHandle mulsh        = vc_bvExtract( vc, mult_64, 63, 32 ); //upper 32-bits
  ExprHandle n_plus_mulsh = vc_bvPlusExpr( vc, width, expr_n, mulsh );

  // Improved variable arithmetic right shift: sign extend, shift,
  // extract.
  ExprHandle extend_npm   = vc_bvSignExtend( vc, n_plus_mulsh, 64 );
  ExprHandle shift_npm    = bvRightShift( extend_npm, expr_shpost, 64 );
  ExprHandle shift_shpost = vc_bvExtract( vc, shift_npm, 31, 0 ); //lower 32-bits

  // XSIGN(n) is -1 if n is negative, positive one otherwise
  ExprHandle is_signed    = bvBoolExtract( expr_n, 31 );
  ExprHandle neg_one      = bvMinusOne(32);
  ExprHandle xsign_of_n   = vc_iteExpr( vc, is_signed, neg_one, bvZero(32) );

  // q0 = (n_plus_mulsh >> shpost) - XSIGN(n)
  ExprHandle q0           = vc_bvMinusExpr( vc, width, shift_shpost, xsign_of_n );
  
  // n/d = (q0 ^ dsign) - dsign
  ExprHandle q0_xor_dsign = vc_bvXorExpr( vc, q0, expr_dsign );
  ExprHandle res          = vc_bvMinusExpr( vc, width, q0_xor_dsign, expr_dsign );

  return res;
}

::VCExpr STPBuilder::getInitialArray(const Array *root) {
  // Build the initial array
  if (!root->stpInitialArray) {
    // STP uniques arrays by name, so we make sure the name is unique by
    // including the address.
    std::string name = root->name + "_" + llvm::utohexstr((uintptr_t)root);
    root->stpInitialArray = buildArray(name.c_str(), 32, 8);

    if (root->type == Array::ConstantInitialValues) {
      // FIXME: Flush the concrete values into STP. Ideally we would do this
      // using assertions, which is much faster, but we need to fix the caching
      // to work correctly in that case.
      // TODO: It should be trivial to do this using assertions: is that better?
      for (unsigned i = 0, e = root->getConstantInitialValues().size(); i != e; ++i) {
        ::VCExpr prev = root->stpInitialArray;
        root->stpInitialArray = 
          vc_writeExpr(vc, prev,
                       construct(ConstantExpr::alloc(i, root->getDomain()), 0),
                       construct(root->getConstantInitialValues()[i], 0));
        vc_DeleteExpr(prev);
      }
    }
  }

  // Ensure the constraint (if any) is marked for instantiation
  if (root->type == Array::ConstrainedInitialValues) {
    scanTriggers(root->getConstraint(), &_assumptions);
  }

  return root->stpInitialArray;
}

void STPBuilder::processArrayReadTriggers(const Array *root, const ref<Expr> &index) {
  // Ensure we've visited this array
  getInitialArray(root);

  // Process all triggers
  // It's important to add "index" to the "done" set BEFORE constructing
  // the intantiated assumption so we avoid potential infinite recursion.
  OnArrayReadWorklistTy::mapped_type &work = _onArrayReadWorklist[root];
  for (OnArrayReadWorklistTy::mapped_type::iterator
       it = work.begin(); it != work.end(); ++it) {
    // it->first  = ForallExpr
    // it->second = set of indices that have been instantiated
    assert(it->second.dst);
    if (it->second.done.insert(index).second) {
      it->second.dst->push_back(construct(it->first->instantiate(index)));
    }
  }
}

ExprHandle STPBuilder::constructReadExpr(const ReadExpr *re) {
  ::VCExpr array = NULL;

  // Get the initial array
  // We walk updates youngest to oldest, stopping at the youngest cached result
  std::vector<const UpdateNode*> updates;
  updates.reserve(re->updates.getSize());
  for (const UpdateNode *un = re->updates.head; un; un = un->next) {
    if (un->stpArray) {
      array = un->stpArray;
      break;
    }
    updates.push_back(un);
  }

  if (!array) {
    array = getInitialArray(re->updates.root);
  }

  // Build the array to read from
  // We walk backwards (oldest to youngest) over the updates just visited
  for (size_t k = updates.size(); k; --k) {
    const UpdateNode *un = updates[k-1];
    assert(!un->stpArray);

    // Single-byte case: add a simple write
    // Multi-byte case: use the src array
    ::VCExpr newArray = NULL;
    if (un->isSingleByte()) {
      newArray = vc_writeExpr(vc, array,
                                  construct(un->getByteIndex()),
                                  construct(un->getByteValue()));
    } else {
      newArray = getInitialArray(un->getSrcArray());
    }

    // Guards: ite(guard, this, next)
    // TODO: do intelligent coalescing of guards?
    // TODO: e.g., if un->guard == un->next->guard, emit just one ite
    if (un->getGuard()->isTrue()) {
      array = newArray;
    } else {
      array = vc_iteExpr(vc, construct(un->getGuard()), newArray, array);
    }

    // Cache
    un->stpArray = array;
  }

  // Build the result
  ExprHandle result = vc_readExpr(vc, array, construct(re->index));

  // Scan the updates to instantiate all relavent quantifiers
  processArrayReadTriggers(re->updates.root, re->index);
  for (const UpdateNode *un = re->updates.head; un; un = un->next) {
    if (un->isMultiByte())
      processArrayReadTriggers(un->getSrcArray(), re->index);
  }

  return result;
}

/** if *width_out!=1 then result is a bitvector,
    otherwise it is a bool */
ExprHandle STPBuilder::construct(ref<Expr> e, int *width_out) {
  if (!UseConstructHash || isa<ConstantExpr>(e)) {
    return constructActual(e, width_out);
  } else {
    ConstructedCacheTy::iterator it = _constructed.find(e);
    if (it != _constructed.end()) {
      if (width_out)
        *width_out = it->second.second;
      return it->second.first;
    } else {
      int width;
      if (!width_out)
        width_out = &width;
      ExprHandle res = constructActual(e, width_out);
      _constructed.insert(std::make_pair(e, std::make_pair(res, *width_out)));
      return res;
    }
  }
}

/** if *width_out!=1 then result is a bitvector,
    otherwise it is a bool */
ExprHandle STPBuilder::constructActual(ref<Expr> e, int *width_out) {
  int width;
  if (!width_out) width_out = &width;

  ++stats::queryConstructs;

  assert(e.get());
  switch (e->getKind()) {
  case Expr::Constant: {
    ConstantExpr *CE = cast<ConstantExpr>(e);
    *width_out = CE->getWidth();

    // Coerce to bool if necessary.
    if (*width_out == 1)
      return CE->isTrue() ? getTrue() : getFalse();

    // Fast path.
    if (*width_out <= 32)
      return bvConst32(*width_out, CE->getZExtValue(32));
    if (*width_out <= 64)
      return bvConst64(*width_out, CE->getZExtValue());

    // FIXME: Optimize?
    ref<ConstantExpr> Tmp = CE;
    ExprHandle Res = bvConst64(64, Tmp->Extract(0, 64)->getZExtValue());
    for (unsigned i = (*width_out / 64) - 1; i; --i) {
      Tmp = Tmp->LShr(ConstantExpr::alloc(64, Tmp->getWidth()));
      Res = vc_bvConcatExpr(vc, bvConst64(std::min(64U, Tmp->getWidth()),
                                          Tmp->Extract(0, 64)->getZExtValue()),
                            Res);
    }
    return Res;
  }
    
  // Special
  case Expr::NotOptimized: {
    NotOptimizedExpr *noe = cast<NotOptimizedExpr>(e);
    return construct(noe->src, width_out);
  }

  case Expr::Read: {
    ReadExpr *re = cast<ReadExpr>(e);
    *width_out = 8;
    return constructReadExpr(re);
  }

  case Expr::Select: {
    SelectExpr *se = cast<SelectExpr>(e);
    ExprHandle cond = construct(se->cond, 0);
    ExprHandle tExpr = construct(se->trueExpr, width_out);
    ExprHandle fExpr = construct(se->falseExpr, width_out);
    return vc_iteExpr(vc, cond, tExpr, fExpr);
  }

  case Expr::Concat: {
    ConcatExpr *ce = cast<ConcatExpr>(e);
    unsigned numKids = ce->getNumKids();
    ExprHandle res = construct(ce->getKid(numKids-1), 0);
    for (int i=numKids-2; i>=0; i--) {
      res = vc_bvConcatExpr(vc, construct(ce->getKid(i), 0), res);
    }
    *width_out = ce->getWidth();
    return res;
  }

  case Expr::Extract: {
    ExtractExpr *ee = cast<ExtractExpr>(e);
    ExprHandle src = construct(ee->expr, width_out);    
    *width_out = ee->getWidth();
    if (*width_out==1) {
      return bvBoolExtract(src, ee->offset);
    } else {
      return vc_bvExtract(vc, src, ee->offset + *width_out - 1, ee->offset);
    }
  }

    // Casting

  case Expr::ZExt: {
    int srcWidth;
    CastExpr *ce = cast<CastExpr>(e);
    ExprHandle src = construct(ce->src, &srcWidth);
    *width_out = ce->getWidth();
    if (srcWidth==1) {
      return vc_iteExpr(vc, src, bvOne(*width_out), bvZero(*width_out));
    } else {
      return vc_bvConcatExpr(vc, bvZero(*width_out-srcWidth), src);
    }
  }

  case Expr::SExt: {
    int srcWidth;
    CastExpr *ce = cast<CastExpr>(e);
    ExprHandle src = construct(ce->src, &srcWidth);
    *width_out = ce->getWidth();
    if (srcWidth==1) {
      return vc_iteExpr(vc, src, bvMinusOne(*width_out), bvZero(*width_out));
    } else {
      return vc_bvSignExtend(vc, src, *width_out);
    }
  }

    // Arithmetic

  case Expr::Add: {
    AddExpr *ae = cast<AddExpr>(e);
    ExprHandle left = construct(ae->left, width_out);
    ExprHandle right = construct(ae->right, width_out);
    assert(*width_out!=1 && "uncanonicalized add");
    return vc_bvPlusExpr(vc, *width_out, left, right);
  }

  case Expr::Sub: {
    SubExpr *se = cast<SubExpr>(e);
    ExprHandle left = construct(se->left, width_out);
    ExprHandle right = construct(se->right, width_out);
    assert(*width_out!=1 && "uncanonicalized sub");
    return vc_bvMinusExpr(vc, *width_out, left, right);
  } 

  case Expr::Mul: {
    MulExpr *me = cast<MulExpr>(e);
    ExprHandle right = construct(me->right, width_out);
    assert(*width_out!=1 && "uncanonicalized mul");

    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(me->left))
      if (CE->getWidth() <= 64)
        return constructMulByConstant(right, *width_out, 
                                      CE->getZExtValue());

    ExprHandle left = construct(me->left, width_out);
    return vc_bvMultExpr(vc, *width_out, left, right);
  }

  case Expr::UDiv: {
    UDivExpr *de = cast<UDivExpr>(e);
    ExprHandle left = construct(de->left, width_out);
    assert(*width_out!=1 && "uncanonicalized udiv");
    
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(de->right)) {
      if (CE->getWidth() <= 64) {
        uint64_t divisor = CE->getZExtValue();
      
        if (bits64::isPowerOfTwo(divisor)) {
          return bvRightShiftByConstant(left,
                              bits64::indexOfSingleBit(divisor),
                              getShiftBits(*width_out));
        } else if (_optimizeDivides) {
          if (*width_out == 32) //only works for 32-bit division
            return constructUDivByConstant( left, *width_out, 
                                            (uint32_t) divisor);
        }
      }
    } 

    ExprHandle right = construct(de->right, width_out);
    return vc_bvDivExpr(vc, *width_out, left, right);
  }

  case Expr::SDiv: {
    SDivExpr *de = cast<SDivExpr>(e);
    ExprHandle left = construct(de->left, width_out);
    assert(*width_out!=1 && "uncanonicalized sdiv");

#if 0 // possibly buggy? (test case is FFT with the solver validity checks enabled)
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(de->right))
      if (_optimizeDivides)
        if (*width_out == 32) //only works for 32-bit division
          return constructSDivByConstant( left, *width_out, 
                                          CE->getZExtValue(32));
#endif

    // XXX need to test for proper handling of sign, not sure I
    // trust STP
    ExprHandle right = construct(de->right, width_out);
    return vc_sbvDivExpr(vc, *width_out, left, right);
  }

  case Expr::URem: {
    URemExpr *de = cast<URemExpr>(e);
    ExprHandle left = construct(de->left, width_out);
    assert(*width_out!=1 && "uncanonicalized urem");
    
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(de->right)) {
      if (CE->getWidth() <= 64) {
        uint64_t divisor = CE->getZExtValue();

        if (bits64::isPowerOfTwo(divisor)) {
          unsigned bits = bits64::indexOfSingleBit(divisor);

          // special case for modding by 1 or else we bvExtract -1:0
          if (bits == 0) {
            return bvZero(*width_out);
          } else {
            return vc_bvConcatExpr(vc,
                                   bvZero(*width_out - bits),
                                   bvExtract(left, bits - 1, 0));
          }
        }

        // Use fast division to compute modulo without explicit division for
        // constant divisor.

        if (_optimizeDivides) {
          if (*width_out == 32) { //only works for 32-bit division
            ExprHandle quotient = constructUDivByConstant( left, *width_out, (uint32_t)divisor );
            ExprHandle quot_times_divisor = constructMulByConstant( quotient, *width_out, divisor );
            ExprHandle rem = vc_bvMinusExpr( vc, *width_out, left, quot_times_divisor );
            return rem;
          }
        }
      }
    }
    
    ExprHandle right = construct(de->right, width_out);
    return vc_bvModExpr(vc, *width_out, left, right);
  }

  case Expr::SRem: {
    SRemExpr *de = cast<SRemExpr>(e);
    ExprHandle left = construct(de->left, width_out);
    ExprHandle right = construct(de->right, width_out);
    assert(*width_out!=1 && "uncanonicalized srem");

#if 0 //not faster per first benchmark
    if (_optimizeDivides) {
      if (ConstantExpr *cre = de->right->asConstant()) {
        uint64_t divisor = cre->asUInt64;

        //use fast division to compute modulo without explicit division for constant divisor
        if (*width_out == 32) { //only works for 32-bit division
          ExprHandle quotient = constructSDivByConstant( left, *width_out, divisor );
          ExprHandle quot_times_divisor = constructMulByConstant( quotient, *width_out, divisor );
          ExprHandle rem = vc_bvMinusExpr( vc, *width_out, left, quot_times_divisor );
          return rem;
        }
      }
    }
#endif

    // XXX implement my fast path and test for proper handling of sign
    return vc_sbvModExpr(vc, *width_out, left, right);
  }

    // Bitwise

  case Expr::Not: {
    NotExpr *ne = cast<NotExpr>(e);
    ExprHandle expr = construct(ne->expr, width_out);
    if (*width_out==1) {
      return vc_notExpr(vc, expr);
    } else {
      return vc_bvNotExpr(vc, expr);
    }
  }    

  case Expr::And: {
    AndExpr *ae = cast<AndExpr>(e);
    ExprHandle left = construct(ae->left, width_out);
    ExprHandle right = construct(ae->right, width_out);
    if (*width_out==1) {
      return vc_andExpr(vc, left, right);
    } else {
      return vc_bvAndExpr(vc, left, right);
    }
  }

  case Expr::Or: {
    OrExpr *oe = cast<OrExpr>(e);
    ExprHandle left = construct(oe->left, width_out);
    ExprHandle right = construct(oe->right, width_out);
    if (*width_out==1) {
      return vc_orExpr(vc, left, right);
    } else {
      return vc_bvOrExpr(vc, left, right);
    }
  }

  case Expr::Xor: {
    XorExpr *xe = cast<XorExpr>(e);
    ExprHandle left = construct(xe->left, width_out);
    ExprHandle right = construct(xe->right, width_out);
    
    if (*width_out==1) {
      // XXX check for most efficient?
      return vc_iteExpr(vc, left, 
                        ExprHandle(vc_notExpr(vc, right)), right);
    } else {
      return vc_bvXorExpr(vc, left, right);
    }
  }

  case Expr::Shl: {
    ShlExpr *se = cast<ShlExpr>(e);
    ExprHandle left = construct(se->left, width_out);
    assert(*width_out!=1 && "uncanonicalized shl");

    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(se->right)) {
      return bvLeftShiftByConstant(left, (unsigned) CE->getLimitedValue(), 
                         getShiftBits(*width_out));
    } else {
      int shiftWidth;
      ExprHandle amount = construct(se->right, &shiftWidth);
      return bvLeftShift( left, amount, *width_out );
    }
  }

  case Expr::LShr: {
    LShrExpr *lse = cast<LShrExpr>(e);
    ExprHandle left = construct(lse->left, width_out);
    unsigned shiftBits = getShiftBits(*width_out);
    assert(*width_out!=1 && "uncanonicalized lshr");

    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(lse->right)) {
      return bvRightShiftByConstant(left, (unsigned) CE->getLimitedValue(), 
                          shiftBits);
    } else {
      int shiftWidth;
      ExprHandle amount = construct(lse->right, &shiftWidth);
      return bvRightShift( left, amount, *width_out );
    }
  }

  case Expr::AShr: {
    AShrExpr *ase = cast<AShrExpr>(e);
    ExprHandle left = construct(ase->left, width_out);
    assert(*width_out!=1 && "uncanonicalized ashr");
    
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(ase->right)) {
      unsigned shift = (unsigned) CE->getLimitedValue();
      ExprHandle signedBool = bvBoolExtract(left, *width_out-1);
      return bvArithRightShiftByConstant(left, shift, signedBool, 
                                         getShiftBits(*width_out));
    } else {
      int shiftWidth;
      ExprHandle amount = construct(ase->right, &shiftWidth);
      return bvArithRightShift( left, amount, *width_out );
    }
  }

    // Comparison

  case Expr::Eq: {
    EqExpr *ee = cast<EqExpr>(e);
    ExprHandle left = construct(ee->left, width_out);
    ExprHandle right = construct(ee->right, width_out);
    if (*width_out==1) {
      if (ConstantExpr *CE = dyn_cast<ConstantExpr>(ee->left)) {
        if (CE->isTrue())
          return right;
        return vc_notExpr(vc, right);
      } else {
        return vc_iffExpr(vc, left, right);
      }
    } else {
      *width_out = 1;
      return vc_eqExpr(vc, left, right);
    }
  }

  case Expr::Ult: {
    UltExpr *ue = cast<UltExpr>(e);
    ExprHandle left = construct(ue->left, width_out);
    ExprHandle right = construct(ue->right, width_out);
    assert(*width_out!=1 && "uncanonicalized ult");
    *width_out = 1;
    return vc_bvLtExpr(vc, left, right);
  }

  case Expr::Ule: {
    UleExpr *ue = cast<UleExpr>(e);
    ExprHandle left = construct(ue->left, width_out);
    ExprHandle right = construct(ue->right, width_out);
    assert(*width_out!=1 && "uncanonicalized ule");
    *width_out = 1;
    return vc_bvLeExpr(vc, left, right);
  }

  case Expr::Slt: {
    SltExpr *se = cast<SltExpr>(e);
    ExprHandle left = construct(se->left, width_out);
    ExprHandle right = construct(se->right, width_out);
    assert(*width_out!=1 && "uncanonicalized slt");
    *width_out = 1;
    return vc_sbvLtExpr(vc, left, right);
  }

  case Expr::Sle: {
    SleExpr *se = cast<SleExpr>(e);
    ExprHandle left = construct(se->left, width_out);
    ExprHandle right = construct(se->right, width_out);
    assert(*width_out!=1 && "uncanonicalized sle");
    *width_out = 1;
    return vc_sbvLeExpr(vc, left, right);
  }

    // Logical

  case Expr::Implies: {
    ImpliesExpr *xe = cast<ImpliesExpr>(e);
    ExprHandle left = construct(xe->left, width_out);
    ExprHandle right = construct(xe->right, width_out);
    return vc_impliesExpr(vc, left, right);
  }

  case Expr::QVar: {
    assert(0 && "attempting to construct a QVar");
    break;
  }

  case Expr::Forall: {
    assert(0 && "attempting to construct a Forall");
    break;
  }

    // unused due to canonicalization
#if 0
  case Expr::Ne:
  case Expr::Ugt:
  case Expr::Uge:
  case Expr::Sgt:
  case Expr::Sge:
#endif

  default: 
    assert(0 && "unhandled Expr type");
    return vc_trueExpr(vc);
  }
}

void STPBuilder::scanTriggers(ref<Expr> e, ExprHandleList *target) {
  ForallExpr *forall = dyn_cast<ForallExpr>(e);
  if (!forall)
    return;

  const ForallTrigger &t = forall->trigger;
  switch (t.type) {
  case ForallTrigger::OnArrayReads:
    assert(t.arrays[0]);
    for (int i = 0; i < ForallTrigger::MaxArrays; ++i) {
      if (t.arrays[i]) {
        InstantiatedSet &is = _onArrayReadWorklist[t.arrays[i]][forall];
        assert(!is.dst || is.dst == target);
        is.dst = target;
      }
    }
    break;

  default:
    assert(0 && "bad trigger type");
  }
}

void STPBuilder::assertAssumptions(const ConstraintManager &constraints) {
  // Do an initial scanning pass
  // This builds an initial list of quantifiers that must be instantiated
  for (ConstraintManager::iterator
       it = constraints.begin(); it != constraints.end(); ++it) {
    scanTriggers(*it, &_assumptions);
  }

  // Assert all assumptions, except foralls
  for (ConstraintManager::iterator
       it = constraints.begin(); it != constraints.end(); ++it) {
    if (!isa<ForallExpr>(*it))
      vc_assertFormula(vc, construct(*it));
  }

  // Assert all other instantiated foralls
  while (!_assumptions.empty()) {
    vc_assertFormula(vc, _assumptions.back());
    _assumptions.pop_back();
  }
}

ExprHandle STPBuilder::constructQuery(ref<Expr> e) {
  ExprHandle result = NULL;

  if (e->getWidth() != Expr::Bool) {
    result = construct(e);
  } else {
    // XXX ugly, but fixing would require more complete "forall" support
    bool isNegated = false;
    if (EqExpr *EQ = dyn_cast<EqExpr>(e)) {
      if (EQ->left == Expr::False) {
        e = EQ->right;
        isNegated = true;
      }
    }

    // For boolean exprs: do an initial scanning pass
    // This builds an initial list of quantifiers in the expression.
    // We save these on the side, then merge them in later.
    ExprHandleList extra;
    for (ExprNaryIterator
         it = ExprNaryIterator(e, Expr::And); it != ExprNaryIterator(); ++it) {
      scanTriggers(*it, &extra);
    }

    // Construct all terms, except foralls
    for (ExprNaryIterator
         it = ExprNaryIterator(e, Expr::And); it != ExprNaryIterator(); ++it) {
      if (!isa<ForallExpr>(*it)) {
        ExprHandle cur = construct(*it);
        result = result ? ExprHandle(vc_andExpr(vc, cur, result)) : cur;
      }
    }

    // Merge in the instantiated foralls
    while (!extra.empty()) {
      result = result ? ExprHandle(vc_andExpr(vc, extra.back(), result)) : extra.back();
      extra.pop_back();
    }

    // XXX ugly
    if (isNegated) {
      result = vc_notExpr(vc, result);
    }
  }

  // Assert all other instantiated foralls
  // e.g., this happens when the query triggers instantiations
  // of foralls that were scanned during assertAssumptions()
  while (!_assumptions.empty()) {
    vc_assertFormula(vc, _assumptions.back());
    _assumptions.pop_back();
  }

  assert(result);
  return result;
}
