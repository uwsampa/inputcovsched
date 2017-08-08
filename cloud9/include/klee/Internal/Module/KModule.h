//===-- KModule.h -----------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_KMODULE_H
#define KLEE_KMODULE_H

#include "klee/Interpreter.h"
#include "klee/util/StringUtil.h"

#include <map>
#include <set>
#include <vector>
#include <istream>

namespace llvm {
  class BasicBlock;
  class Constant;
  class Function;
  class GlobalValue;
  class Instruction;
  class Module;
  class TargetData;

  class AliasAnalysis;
  class DSAA;
}

namespace klee {
  struct Cell;
  class Executor;
  class Expr;
  class InterpreterHandler;
  class InstructionInfoTable;
  struct KInstruction;
  class KModule;
  template<class T> class ref;

  struct KFunction {
    llvm::Function *function;

    unsigned numArgs, numRegisters;

    unsigned numInstructions;
    KInstruction **instructions;

    std::map<llvm::BasicBlock*, unsigned> basicBlockEntry;

    /// Whether instructions in this function should count as
    /// "coverable" for statistics and search heuristics.
    bool trackCoverage;

  private:
    KFunction(const KFunction&);
    KFunction &operator=(const KFunction&);

  public:
    explicit KFunction(llvm::Function*, KModule *);
    ~KFunction();

    unsigned getArgRegister(unsigned index) { return index; }
  };


  class KConstant {
  public:
    /// Actual LLVM constant this represents.
    llvm::Constant* ct;

    /// The constant ID.
    unsigned id;

    /// First instruction where this constant was encountered, or NULL
    /// if not applicable/unavailable.
    KInstruction *ki;

    KConstant(llvm::Constant*, unsigned, KInstruction*);
  };


  class KModule {
  public:
    llvm::Module *module;
    llvm::TargetData *targetData;

    // Analyses
    llvm::AliasAnalysis *AA;
    llvm::DSAA *DSAA;
    
    // Some useful functions to know the address of
    llvm::Function *dbgStopPointFn, *kleeMergeFn;

    // Our shadow versions of LLVM structures.
    std::vector<KFunction*> functions;
    std::map<const llvm::Function*, KFunction*> functionMap;

    // Functions which escape (may be called indirectly)
    // XXX change to KFunction
    std::set<llvm::Function*> escapingFunctions;

    InstructionInfoTable *infos;

    std::map<llvm::Constant*, KConstant*> constantMap;
    std::vector<std::pair<KConstant*, Cell> > constantTable;
    KConstant* getKConstant(llvm::Constant *c);

  private:
    typedef std::pair<std::string, int> program_point_t;
    typedef std::map<std::string, std::set<program_point_t> > vpoints_t;
    typedef std::set<program_point_t> cov_points_t;

    vpoints_t   vulnerablePoints;

    void readVulnerablePoints(std::istream &is);
    bool isVulnerablePoint(KInstruction *kinst);

    typedef std::set<std::string>  cov_list_t;

    cov_list_t  coverableFiles;
    cov_list_t  exceptedFunctions;

    void readCoverableFiles(std::istream &is);
    bool isFunctionCoverable(KFunction *kf);

    cov_points_t coveredLines;

    void readInitialCoverage(std::istream &is);

  public:
    KModule(llvm::Module *_module);
    ~KModule();

    /// Initialize local data structures.
    //
    // FIXME: ihandler should not be here
    void prepare(const Interpreter::ModuleOptions &opts, 
                 InterpreterHandler *ihandler);

    /// Return an id for the given constant, creating a new one if necessary.
    unsigned getConstantID(llvm::Constant *c, KInstruction* ki);
  };

  bool ShouldLoweringPassesIgnoreFunction(const llvm::Function *f);

  extern bool DbgDeleteGlobals;
  void DeleteGlobal(llvm::GlobalValue *G, const bool eraseFromParent = true);
  void DeleteGlobals(llvm::Module *module, const StringMatcher pattern);
  void RemoveFromUsedList(llvm::Module *module, const StringMatcher pattern);
  void AddToUsedList(llvm::Module *module, const std::set<llvm::Constant*> &newItems);

} // End klee namespace

#endif
