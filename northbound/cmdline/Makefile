############################################################
# FastRG makefile
############################################################

######################################
# Set variable
######################################	
CC = gcc
CXX = g++
INCLUDE = -I../../src -I../grpc -I../controller/proto
CFLAGS = $(INCLUDE) -Wall -g $(shell pkg-config --cflags libdpdk) -O3 -DALLOW_EXPERIMENTAL_API -DTEST_MODE #-Wextra -fsanitize=address
# C++ flags for the controller client (gRPC ConfigService + REST login via curl).
CXXFLAGS = -I../controller/proto $(shell pkg-config --cflags jsoncpp libcurl) -Wall -g -O3

LDFLAGS = $(shell pkg-config --static --libs libdpdk) -lutils -lconfig -Wl,--start-group -lstdc++ $(shell pkg-config --libs grpc++ protobuf jsoncpp libcurl) -laddress_sorting -lpthread -Wl,--end-group

TARGET = fastrg_cli
SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

# CLI-local C++ source: controller gRPC client.
CLI_CPP_OBJ = cli_controller_client.o

GRPCDIR = ../grpc
GRPC_SRC = $(GRPCDIR)/fastrg_grpc_client.cpp
PB_SRC = $(wildcard $(GRPCDIR)/*.cc)
GRPC_OBJ = $(GRPC_SRC:.cpp=.o)
PB_OBJ = $(PB_SRC:.cc=.o)

# Controller ConfigService protobuf (generated/compiled by the controller Makefile).
CTRL_PB_OBJ = ../controller/proto/controller.pb.o ../controller/proto/controller.grpc.pb.o

ifneq ($(shell pkg-config --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found")
endif

.PHONY: $(TARGET)
all: $(TARGET)
######################################
# Compile & Link
# 	Must use \tab key after new line
######################################
cli_controller_client.o: cli_controller_client.cpp cli_controller_client.h
	$(CXX) $(CXXFLAGS) -c cli_controller_client.cpp -o cli_controller_client.o

$(TARGET): $(OBJ) $(CLI_CPP_OBJ) $(GRPC_OBJ) $(PB_OBJ)
	$(CC) $(CFLAGS) $(OBJ) $(CLI_CPP_OBJ) $(GRPC_OBJ) $(PB_OBJ) $(CTRL_PB_OBJ) -o $(TARGET) $(LDFLAGS)

install:
	cp $(TARGET) /usr/local/bin/$(TARGET)

######################################
# Clean 
######################################
clean:
	rm -rf $(OBJ) $(CLI_CPP_OBJ) $(TARGET) .libs
