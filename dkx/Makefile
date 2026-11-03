# dkx: MAME-0.264-exact Donkey Kong emulator and tools. Needs g++ (C++17) and zlib.
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -march=native -Wall -Wno-unused-result -Wno-unused-variable -Wno-unused-but-set-variable
CORE = unity.cpp dk.cpp dk.h core.h sched.h i8257.h z80.cpp z80.h mcs48.cpp mcs48.h
all: build/dkx build/play build/trace build/snaptest build/bench
build/%: %.cpp $(CORE)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< unity.cpp -lz
build/dkx: dkxtool.cpp $(CORE)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ dkxtool.cpp unity.cpp -lz
# regenerate the CPU cores from a MAME 0.264 source tree (MAMESRC)
gen:
	python3 gen/port_z80.py && python3 gen/port_mcs48.py
clean:
	rm -rf build
.PHONY: all gen clean
