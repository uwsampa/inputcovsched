TARGET_BC = swaptions.bc
TARGET_LL = swaptions.ll

DEF = -DENABLE_THREADS -fno-exceptions
CSRC = CumNormalInv.cpp MaxFunction.cpp RanUnif.cpp nr_routines.o icdf.cpp \
	HJM_SimPath_Forward_Blocking.cpp HJM.cpp HJM_Swaption_Blocking.cpp  \
	HJM_Securities.cpp

OBJS = $(patsubst %.cpp,obj/%.bc,$(CSRC))

all: $(TARGET_BC) $(TARGET_LL)

obj/%.ll: %.cpp
	mkdir -p obj
	$(CXX) $(CXXFLAGS) $(DEF) $(LIBS) $< -o $@

obj/%.bc: obj/%.ll
	llvm-as $< -o $@

$(TARGET_LL): $(TARGET_BC)
	llvm-dis $< -o $@

$(TARGET_BC): $(OBJS)
	llvm-link $(OBJS) -o $(TARGET_BC)

clean:
	rm -rf $(TARGET_BC) $(TARGET_LL) obj
