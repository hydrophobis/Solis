# build, interpret, minimal: compiler + VM. run_image: VM only.
# The compiler and runtime are C23 and compiled with $(CC); the C++ examples link the objects.

CC  ?= gcc
CXX ?= g++
ROOT := ../..
CFLAGS   := -std=c23 -O2 -Wall -Wextra -I$(ROOT)/compiler -I$(ROOT)/runtime
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -I$(ROOT)/compiler -I$(ROOT)/runtime

COMPILER_C := $(filter-out %/cli/solis.c,$(wildcard $(ROOT)/compiler/*.c))
RUNTIME_C  := $(ROOT)/runtime/solis.c $(ROOT)/runtime/solis_std.c $(ROOT)/runtime/solis_os.c

OBJ := obj
COMPILER_O := $(patsubst $(ROOT)/compiler/%.c,$(OBJ)/c_%.o,$(COMPILER_C))
RUNTIME_O  := $(patsubst $(ROOT)/runtime/%.c,$(OBJ)/r_%.o,$(RUNTIME_C))

all: build interpret run_image minimal

build: build.cpp $(COMPILER_O) $(RUNTIME_O)
	$(CXX) $(CXXFLAGS) $^ -o $@ -lm

interpret: interpret.cpp $(COMPILER_O) $(RUNTIME_O)
	$(CXX) $(CXXFLAGS) $^ -o $@ -lm

run_image: run_image.cpp $(RUNTIME_O)
	$(CXX) $(CXXFLAGS) $^ -o $@ -lm

minimal: minimal.c $(COMPILER_O) $(RUNTIME_O)
	$(CC) $(CFLAGS) $^ -o $@ -lm

$(OBJ)/c_%.o: $(ROOT)/compiler/%.c | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/r_%.o: $(ROOT)/runtime/%.c | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ):
	mkdir -p $@

clean:
	rm -rf $(OBJ) build interpret run_image minimal *.slb

.PHONY: all clean
