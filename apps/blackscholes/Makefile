SRC	= blackscholes.c
M4_SRC  = blackscholes.m4.cpp
M4_BASE = .
MACROS  = ../c.m4.pthreads
CSRC    = $(M4_SRC)

TARGET_BC = blackscholes.bc
TARGET_LL = blackscholes.ll

MT = -DENABLE_THREADS

# Default build single precision version
NCO     = -DNCO=4

ifdef chk_err
ERR     = -DERR_CHK
endif

ifdef single
NCO = -DNCO=4
endif

ifdef size
SZ = -DN=$(size)
else
SZ = -DN=960
endif

ifdef double
NCO = -DNCO=2
endif

CXXFLAGS += $(MT) $(SZ) $(NCO) $(FUNC) $(ERR) $(CSRC)
CXXFLAGS += -fno-exceptions

all: $(TARGET_BC)

$(TARGET_LL): clean $(CSRC)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(LIBS) -o $(TARGET_LL)

$(TARGET_BC): $(TARGET_LL)
	llvm-as $< -o $@

$(M4_SRC): $(SRC) $(MACROS)
	$(M4) $(M4_BASE)/$(MACROS) $< > $@

clean:
	rm -f $(TARGET_BC) $(TARGET_LL) $(M4_SRC)

