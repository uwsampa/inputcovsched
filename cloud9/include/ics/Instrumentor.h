/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * Instrumentation for constrained execution
 *
 */

#ifndef ICS_INSTRUMENTOR_H
#define ICS_INSTRUMENTOR_H

#include "ics/SearchStrategy.h"

namespace ics {

void InstrumentModuleForICS(klee::Executor *executor,
                            const klee::SymbolicObjectTable *symbolicObjects,
                            const klee::SymbolicPtrResolutionTable *resolvedSymbolicPtrs,
                            const DepthFirstICS::RegionsInfoTy &regions);

}  // namespace ics

#endif
