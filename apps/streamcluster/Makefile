TARGET  = streamcluster
SRC     = streamcluster.cpp 
CSRC    = $(SRC)
CXXFLAGS += -pthread

TARGET_BC = $(TARGET).bc
TARGET_LL = $(TARGET).ll

CXXFLAGS += -fno-exceptions

all: $(TARGET_BC)

$(TARGET_LL): clean $(CSRC)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(LIBS) $(CSRC) -o $(TARGET_LL)

$(TARGET_BC): $(TARGET_LL)
	llvm-as $< -o $@

clean:
	rm -f $(TARGET_BC) $(TARGET_LL)
