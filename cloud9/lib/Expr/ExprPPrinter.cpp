//===-- ExprPPrinter.cpp -   ----------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/util/ExprPPrinter.h"

#include "klee/Constraints.h"

#include "llvm/Support/CommandLine.h"

#include <map>
#include <vector>
#include <iostream>
#include <sstream>
#include <iomanip>

using namespace klee;

namespace {
  llvm::cl::opt<bool>
  PCWidthAsArg("pc-width-as-arg", llvm::cl::init(true));

  llvm::cl::opt<bool>
  PCAllWidths("pc-all-widths", llvm::cl::init(false));

  llvm::cl::opt<bool>
  PCPrefixWidth("pc-prefix-width", llvm::cl::init(true));

  llvm::cl::opt<bool>
  PCMultibyteReads("pc-multibyte-reads", llvm::cl::init(true));

  llvm::cl::opt<bool>
  PCAllConstWidths("pc-all-const-widths",  llvm::cl::init(false));

  llvm::cl::opt<bool>
  PCFullReadExprs("pc-full-read-exprs",  llvm::cl::init(true));

  llvm::cl::opt<bool>
  DbgPrintArrayAddrs("debug-print-array-addrs", llvm::cl::init(false));
}

/// PrintContext - Helper class for storing extra information for
/// the pretty printer.
class PrintContext {
private:
  std::ostream &os;
  std::stringstream ss;
  std::string newline;

public:
  /// Number of characters on the current line.
  unsigned pos;

public:
  PrintContext(std::ostream &_os) : os(_os), newline("\n"), pos(0) {}

  void setNewline(const std::string &_newline) {
    newline = _newline;
  }

  void breakLine(unsigned indent=0) {
    os << newline;
    if (indent)
      os << std::setw(indent) << ' ';
    pos = indent;
  }

  /// write - Output a string to the stream and update the
  /// position. The stream should not have any newlines.
  void write(const std::string &s) {
    os << s;
    pos += s.length();
  }

  template <typename T>
  PrintContext &operator<<(T elt) {
    ss.str("");
    ss << elt;
    write(ss.str());
    return *this;
  }
};

static void printArrayName(const Array *array, PrintContext &PC) {
  PC << array->name;
  if (DbgPrintArrayAddrs)
    PC << "." << array;
}

static void printArrayDimensions(const Array *array, PrintContext &PC) {
  PC << "w" << array->getDomain() << " -> " << "w" << array->getRange();
}

class PPrinter : public ExprPPrinter {
public:
  std::set<const Array*> usedArrays;
private:
  std::map<ref<Expr>, unsigned> bindings;
  std::map<const UpdateNode*, unsigned> updateBindings;
  std::set< ref<Expr> > couldPrint, shouldPrint;
  std::set<const UpdateNode*> couldPrintUpdates, shouldPrintUpdates;
  std::ostream &os;
  unsigned counter;
  unsigned updateCounter;
  bool hasScan;
  std::string newline;

  /// shouldPrintWidth - Predicate for whether this expression should
  /// be printed with its width.
  bool shouldPrintWidth(ref<Expr> e) {
    if (PCAllWidths)
      return true;
    return e->getWidth() != Expr::Bool;
  }

  bool isVerySimple(const ref<Expr> &e) { 
    return !e.get() || isa<ConstantExpr>(e) || bindings.find(e)!=bindings.end();
  }

  bool isVerySimpleUpdate(const UpdateNode *un) {
    return !un || updateBindings.find(un)!=updateBindings.end();
  }


  // document me!
  bool isSimple(const ref<Expr> &e) { 
    if (isVerySimple(e)) {
      return true;
    } else if (const ReadExpr *re = dyn_cast<ReadExpr>(e)) {
      return isVerySimple(re->index) && isVerySimpleUpdate(re->updates.head);
    } else {
      Expr *ep = e.get();
      for (unsigned i=0; i<ep->getNumKids(); i++)
        if (!isVerySimple(ep->getKid(i)))
          return false;
      return true;
    }
  }

  bool hasSimpleKids(const Expr *ep) {
    for (unsigned i=0; i<ep->getNumKids(); i++)
      if (!isSimple(ep->getKid(i)))
        return false;
    return true;
  }

  void scan1(const ref<Expr> &e) {
    if (!isa<ConstantExpr>(e)) {
      if (couldPrint.insert(e).second) {
        Expr *ep = e.get();
        for (unsigned i=0; i<ep->getNumKids(); i++)
          scan1(ep->getKid(i));
        if (const ReadExpr *re = dyn_cast<ReadExpr>(e))
          scanUpdates(re->updates);
      } else {
        shouldPrint.insert(e);
      }
    }
  }

  void printWidth(PrintContext &PC, ref<Expr> e) {
    if (!shouldPrintWidth(e))
      return;

    if (PCWidthAsArg) {
      PC << ' ';
      if (PCPrefixWidth)
        PC << 'w';
    }

    PC << e->getWidth();
  }

#if 0
  /// hasAllByteReads - True iff all children are byte level reads or
  /// concats of byte level reads.
  bool hasAllByteReads(const Expr *ep) {
    switch (ep->kind) {
      Expr::Read: {
        // right now, all Reads are byte reads but some
        // transformations might change this
        return ep->getWidth() == Expr::Int8;
      }
      Expr::Concat: {
        for (unsigned i=0; i<ep->getNumKids(); ++i) {
          if (!hashAllByteReads(ep->getKid(i)))
            return false;
        }
      }
    default: return false;
    }
  }
#endif

  void printRead(const ReadExpr *re, PrintContext &PC, unsigned indent) {
    print(re->index, PC);
    printSeparator(PC, isVerySimple(re->index), indent);
    printUpdateList(re->updates, PC);
  }

  void printExtract(const ExtractExpr *ee, PrintContext &PC, unsigned indent) {
    PC << ee->offset << ' ';
    print(ee->expr, PC);
  }

  void printExpr(const Expr *ep, PrintContext &PC, unsigned indent, bool printConstWidth=false) {
    bool simple = hasSimpleKids(ep);

    // For QVarExpr, just print the namee
    if (const QVarExpr *qv = dyn_cast<QVarExpr>(ep)) {
      PC << qv->name;
      return;
    }
    
    // For AndExpr and OrExpr, print chains as a single expr
    if (ep->getKind() == Expr::And || ep->getKind() == Expr::Or) {
      ref<Expr> sub = const_cast<Expr*>(ep);
      do {
        print(sub->getKid(0), PC, printConstWidth);
        printSeparator(PC, simple, indent);
        sub = sub->getKid(1).get();
      } while (sub->getKind() == ep->getKind());

      print(sub, PC, printConstWidth);
      return;
    }

    // Anything else: just print the kids
    print(ep->getKid(0), PC, printConstWidth);
    for (unsigned i=1; i<ep->getNumKids(); i++) {
      printSeparator(PC, simple, indent);
      print(ep->getKid(i), PC, printConstWidth);
    }
  }

public:
  PPrinter(std::ostream &_os) : os(_os), newline("\n") {
    reset();
  }

  void setNewline(const std::string &_newline) {
    newline = _newline;
  }

  void reset() {
    counter = 0;
    updateCounter = 0;
    hasScan = false;
    bindings.clear();
    updateBindings.clear();
    couldPrint.clear();
    shouldPrint.clear();
    couldPrintUpdates.clear();
    shouldPrintUpdates.clear();
  }

  void scan(const ref<Expr> &e) {
    hasScan = true;
    assert(e.get());
    scan1(e);
  }

  void scanUpdates(const UpdateList &updates) {
    hasScan = true;
    assert(updates.root);
    usedArrays.insert(updates.root);
    for (const UpdateNode *un = updates.head; un; un = un->next) {
      if (couldPrintUpdates.insert(un).second) {
        scan1(un->getGuard());
        if (un->isSingleByte()) {
          scan1(un->getByteIndex());
          scan1(un->getByteValue());
        } else if (PCFullReadExprs) {
          const Array *src = un->getSrcArray();
          assert(src);
          usedArrays.insert(src);
          if (src->type == Array::ConstrainedInitialValues && src->getNextSrc().root)
            scanUpdates(src->getNextSrc());
        }
      } else {
        shouldPrintUpdates.insert(un);
        break;
      }
    }
  }

  void print(const ref<Expr> &e, unsigned level=0) {
    PrintContext PC(os);
    PC.pos = level;
    print(e, PC);
  }

  void printConst(const ref<ConstantExpr> &e, PrintContext &PC, 
                  bool printWidth) {
    if (e->getWidth() == Expr::Bool)
      PC << (e->isTrue() ? "true" : "false");
    else {
      if (PCAllConstWidths)
        printWidth = true;
    
      if (printWidth)
        PC << "(w" << e->getWidth() << " ";

      if (e->getWidth() <= 64) {
        PC << e->getZExtValue();
      } else {
        std::string S;
        e->toString(S);
        PC << S;
      }

      if (printWidth)
        PC << ")";
    }    
  }

  void print(const ref<Expr> &e, PrintContext &PC, bool printConstWidth = false) {
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(e))
      printConst(CE, PC, printConstWidth);
    else {
      std::map<ref<Expr>, unsigned>::iterator it = bindings.find(e);
      if (it != bindings.end()) {
        PC << 'N' << it->second;
      } else {
        if (!hasScan || shouldPrint.count(e)) {
          PC << 'N' << counter << ':';
          bindings.insert(std::make_pair(e, counter++));
        }

        // Detect multibyte reads.
        // FIXME: Hrm. One problem with doing this is that we are
        // masking the sharing of the indices which aren't
        // visible. Need to think if this matters... probably not
        // because if they are offset reads then its either constant,
        // or they are (base + offset) and base will get printed with
        // a declaration.
        if (PCMultibyteReads && e->getKind() == Expr::Concat) {
          const ReadExpr *base = Expr::hasOrderedReads(e, -1).get();
          int isLSB = (base != NULL);
          if (!isLSB)
            base = Expr::hasOrderedReads(e, 1).get();
          if (base) {
            PC << "(Read" << (isLSB ? "LSB" : "MSB");
            printWidth(PC, e);
            PC << ' ';
            printRead(base, PC, PC.pos);
            PC << ')';
            return;
          }
        }

        PC << '(' << e->getKind();
        printWidth(PC, e);
        PC << ' ';

        // Indent at first argument and dispatch to appropriate print
        // routine for exprs which require special handling.
        unsigned indent = PC.pos;
        if (const ReadExpr *re = dyn_cast<ReadExpr>(e)) {
          printRead(re, PC, indent);
        } else if (const ExtractExpr *ee = dyn_cast<ExtractExpr>(e)) {
          printExtract(ee, PC, indent);
        } else if (e->getKind() == Expr::Concat || e->getKind() == Expr::SExt)
          printExpr(e.get(), PC, indent, true);
        else
          printExpr(e.get(), PC, indent);        
        PC << ")";
      }
    }
  }

  void printUpdateList(const UpdateList &updates, PrintContext &PC) {
    const UpdateNode *head = updates.head;

    // Special case empty list.
    if (!head) {
      printArrayName(updates.root, PC);
      return;
    }

    // FIXME: Explain this breaking policy.
    bool openedList = false, nextShouldBreak = false;
    unsigned outerIndent = PC.pos;
    unsigned middleIndent = 0;
    for (const UpdateNode *un = head; un; un = un->next) {      
      nextShouldBreak = false;

      // We are done if we hit the cache.
      std::map<const UpdateNode*, unsigned>::iterator it = updateBindings.find(un);
      if (it!=updateBindings.end()) {
        if (openedList)
          PC << "] @ ";
        PC << "U" << it->second;
        return;
      } else if (!hasScan || shouldPrintUpdates.count(un)) {
        if (openedList)
          PC << "] @";
        if (un != head)
          PC.breakLine(outerIndent);
        PC << "U" << updateCounter << ":"; 
        updateBindings.insert(std::make_pair(un, updateCounter++));
        openedList = false;
      }

      if (!openedList) {
        openedList = true;
        PC << '[';
        middleIndent = PC.pos;
      } else {
        PC << ',';
        printSeparator(PC, !nextShouldBreak, middleIndent);
      }

      if (!un->getGuard()->isTrue()) {
        PC << "(if ";
        unsigned innerIndent = PC.pos;
        print(un->getGuard(), PC);
        printSeparator(PC, isSimple(un->getGuard()), innerIndent);
        nextShouldBreak = true;
      }

      if (un->isSingleByte()) {
        print(un->getByteIndex(), PC);
        PC << "=";
        print(un->getByteValue(), PC);
        nextShouldBreak |= (!isa<ConstantExpr>(un->getByteIndex()) ||
                            !isa<ConstantExpr>(un->getByteValue()));
      } else {
        if (!PCFullReadExprs) {
          const UpdateNode *x;
          unsigned cnt = 0;
          for (x = un->next; x && x->next && x->isMultiByte(); x = x->next)
            ++cnt;
          if (cnt > 3) {
            PC << "(" << cnt << "Copies ...))";
            un = x;
            continue;
          }
        }
        const Array *src = un->getSrcArray();
        PC << "(Copy @";
        printArrayName(src, PC);
        if (src->type == Array::ConstrainedInitialValues) {
          PC << " ";
          print(src->getConstraint(), PC);
          nextShouldBreak = true;
        }
        PC << ")";
      }

      if (!un->getGuard()->isTrue()) {
        PC << ")";
      }
    }

    if (openedList)
      PC << ']';

    PC << " @ ";
    printArrayName(updates.root, PC);
    if (updates.root->type == Array::ConstrainedInitialValues) {
      PC << ":";
      print(updates.root->getConstraint(), PC);
    }
  }

  /* Public utility functions */

  void printSeparator(PrintContext &PC, bool simple, unsigned indent) {
    if (simple) {
      PC << ' ';
    } else {
      PC.breakLine(indent);
    }
  }
};

ExprPPrinter *klee::ExprPPrinter::create(std::ostream &os) {
  return new PPrinter(os);
}

void ExprPPrinter::printOne(std::ostream &os,
                            const char *message, 
                            const ref<Expr> &e) {
  PPrinter p(os);
  p.scan(e);

  // FIXME: Need to figure out what to do here. Probably print as a
  // "forward declaration" with whatever syntax we pick for that.
  PrintContext PC(os);
  PC << message << ": ";
  p.print(e, PC);
  PC.breakLine();
}

void ExprPPrinter::printSingleExpr(std::ostream &os, const ref<Expr> &e) {
  PPrinter p(os);
  p.scan(e);

  // FIXME: Need to figure out what to do here. Probably print as a
  // "forward declaration" with whatever syntax we pick for that.
  PrintContext PC(os);
  p.print(e, PC);
}

void ExprPPrinter::printSingleUpdateList(std::ostream &os, const UpdateList &updates) {
  PPrinter p(os);
  p.scanUpdates(updates);

  // FIXME: Need to figure out what to do here. Probably print as a
  // "forward declaration" with whatever syntax we pick for that.
  PrintContext PC(os);
  p.printUpdateList(updates, PC);
}

void ExprPPrinter::printConstraints(std::ostream &os,
                                    const ConstraintManager &constraints) {
  printQuery(os, constraints, ConstantExpr::alloc(false, Expr::Bool));
}

void ExprPPrinter::printQuery(std::ostream &os,
                              const ConstraintManager &constraints,
                              const ref<Expr> &q,
                              const ref<Expr> *evalExprsBegin,
                              const ref<Expr> *evalExprsEnd,
                              const Array * const *evalArraysBegin,
                              const Array * const *evalArraysEnd,
                              bool printArrayDecls) {
  PPrinter p(os);
  
  for (ConstraintManager::iterator
       it = constraints.begin(), ie = constraints.end(); it != ie; ++it) {
    p.scan(*it);
  }
  p.scan(q);

  for (const ref<Expr> *it = evalExprsBegin; it != evalExprsEnd; ++it)
    p.scan(*it);

  PrintContext PC(os);
  
  // Print array declarations.
  if (printArrayDecls) {
    for (const Array * const* it = evalArraysBegin; it != evalArraysEnd; ++it)
      p.usedArrays.insert(*it);
    for (std::set<const Array*>::iterator
         it = p.usedArrays.begin(), ie = p.usedArrays.end(); it != ie; ++it) {
      const Array *A = *it;
      PC << "array ";
      printArrayName(A, PC);
      switch (A->type) {
      case Array::Symbolic:
        PC << " [] : ";
        printArrayDimensions(A, PC);
        PC << " = symbolic";
        break;
      case Array::ConstantInitialValues:
        PC << " [ " << A->getConstantInitialValues().size() << " ] : ";
        printArrayDimensions(A, PC);
        PC << " = [";
        for (size_t i = 0; i < A->getConstantInitialValues().size(); ++i) {
          if (i)
            PC << " ";
          PC << A->getConstantInitialValues()[i];
        }
        PC << "]";
        break;
      case Array::ConstrainedInitialValues:
        PC << " [] : ";
        printArrayDimensions(A, PC);
        PC << " = constarray ";
        p.print(A->getConstraint(), PC);
        break;
      default:
        PC << " [] : ???";
        break;
      }
      PC.breakLine();
    }
  }

  PC << "(query [";
  
  // Ident at constraint list;
  unsigned indent = PC.pos;
  for (ConstraintManager::iterator it = constraints.begin(),
         ie = constraints.end(); it != ie;) {
    p.print(*it, PC, true);
    ++it;
    if (it != ie)
      PC.breakLine(indent);
  }
  PC << ']';

  p.printSeparator(PC, constraints.empty(), indent-1);
  p.print(q, PC, true);

  // Print expressions to obtain values for, if any.
  if (evalExprsBegin != evalExprsEnd) {
    p.printSeparator(PC, q->isFalse(), indent-1);
    PC << '[';
    for (const ref<Expr> *it = evalExprsBegin; it != evalExprsEnd; ++it) {
      p.print(*it, PC, /*printConstWidth*/true);
      if (it + 1 != evalExprsEnd)
        PC.breakLine(indent);
    }
    PC << ']';
  }

  // Print arrays to obtain values for, if any.
  if (evalArraysBegin != evalArraysEnd) {
    if (evalExprsBegin == evalExprsEnd)
      PC << " []";

    PC.breakLine(indent - 1);
    PC << '[';
    for (const Array * const* it = evalArraysBegin; it != evalArraysEnd; ++it) {
      PC << (*it)->name;
      if (it + 1 != evalArraysEnd)
        PC.breakLine(indent);
    }
    PC << ']';
  }

  PC << ')';
  PC.breakLine();
}
