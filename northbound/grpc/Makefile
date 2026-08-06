############################################################
# FastRG northbound makefile
############################################################

######################################
# Set variable
######################################	
CC = g++
INCLUDE =
CFLAGS = $(INCLUDE) -Wall -g -O3

LDFLAGS = -lutils

# Coverage build, driven by the top-level `make coverage` target (which
# exports COVERAGE=1): swap -O3 for -O0 so gcov line data maps 1:1 to source
# lines, and add gcov instrumentation.
ifeq ($(COVERAGE),1)
CFLAGS := $(filter-out -O3,$(CFLAGS)) -O0 --coverage
endif

# E2E coverage build, driven by the top-level `make coverage-e2e-build`
# (COVERAGE_E2E=1 arrives through MAKEFLAGS): keep -O3 production codegen
# and only add gcov instrumentation.
ifeq ($(COVERAGE_E2E),1)
CFLAGS += --coverage -fprofile-update=atomic
endif

PROTO_SRC = fastrg_node.proto
GRPC_SRC = $(wildcard *.cpp)
PB_HDR    = $(PB_SRC:.cc=.h)
PB_SRC = $(PROTO_SRC:.proto=.pb.cc) $(PROTO_SRC:.proto=.grpc.pb.cc)
GRPC_OBJ = $(GRPC_SRC:.cpp=.o)
PB_OBJ = $(PB_SRC:.cc=.o)

######################################
# Compile & Link
# 	Must use \tab key after new line
######################################
all:
	protoc --grpc_out=$(CURDIR) --plugin=protoc-gen-grpc=`which grpc_cpp_plugin` $(PROTO_SRC)
	protoc --cpp_out=$(CURDIR) $(PROTO_SRC)
	$(CC) $(CFLAGS) -c $(GRPC_SRC) $(PB_SRC)

######################################
# Clean 
######################################
clean:
	rm -rf $(PB_SRC) $(PB_HDR) $(GRPC_OBJ) $(PB_OBJ)
