############################################################
# FastRG makefile
############################################################

######################################
# Set variable
######################################	
CC = gcc
INCLUDE = -Inorthbound/grpc -Inorthbound/controller

BUILD_TIME := $(shell date '+%Y/%b/%d %H:%M:%S %Z')
GIT_COMMIT := $(shell git describe --always --dirty --tags)

DPDK_CFLAGS := $(shell pkg-config --cflags libdpdk)
CFLAGS = $(INCLUDE) -Wall -Werror -g $(DPDK_CFLAGS) -O3 -DALLOW_EXPERIMENTAL_API -DTEST_MODE #-Wextra -fsanitize=address

# GCC 12/13 emits a false-positive -Warray-bounds inside DPDK's inlined
# rte_memcpy (SSE/emmintrin.h path) at -O3: the 128-255 byte branch issues fixed
# 16-byte loads at offsets the compiler can't prove are unreached, so it wrongly
# flags copies into 128-byte buffers. This only happens on the non-AVX
# (generic/SSE4.2) DPDK baseline used for portable CI builds; a native build uses
# the AVX path and never trips it. Suppress only when DPDK is not built -march=native.
ifeq (,$(findstring -march=native,$(DPDK_CFLAGS)))
CFLAGS += -Wno-array-bounds
endif

LDFLAGS = $(shell pkg-config --static --libs libdpdk) -lutils -lconfig -luuid -Wl,--start-group -lstdc++ $(shell pkg-config --libs grpc++ protobuf jsoncpp) -letcd-cpp-api $(shell pkg-config --libs rdkafka) -laddress_sorting -lpthread -larchive -Wl,--end-group

TARGET = fastrg
VERSION_H = src/version.h
SRC = $(wildcard src/*.c) $(wildcard src/pppd/*.c) $(wildcard src/dhcpd/*.c) $(wildcard src/dnsd/*.c)
OBJ = $(SRC:.c=.o)

GRPCDIR = northbound/grpc
GRPC_SRC = $(filter-out $(GRPCDIR)/%client.cpp, $(wildcard $(GRPCDIR)/*.cpp))
GRPC_OBJ = $(GRPC_SRC:.cpp=.o) ${GRPCDIR}/*pb.o

CONTROLLERDIR = northbound/controller
CONTROLLER_SRC = $(wildcard $(CONTROLLERDIR)/*.cpp)
CONTROLLER_OBJ = $(CONTROLLER_SRC:.cpp=.o) ${CONTROLLERDIR}/proto/*pb.o
TESTDIR = unit_test
TESTBIN = unit-tester

ifneq ($(shell pkg-config --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found")
endif
	
.PHONY: $(TARGET)
all: clean $(TARGET)
######################################
# Compile & Link
# 	Must use \tab key after new line
######################################
$(VERSION_H):
	@echo "Generating $@"
	@echo "#ifndef VERSION_H"              >  $@
	@echo "#define VERSION_H"              >> $@
	@echo ""                               >> $@
	@echo "#define GIT_COMMIT_ID \"$(GIT_COMMIT)\"" >> $@
	@echo "#define BUILD_TIME   \"$(BUILD_TIME)\""  >> $@
	@echo ""                               >> $@
	@echo "#endif"                         >> $@

$(TARGET): $(VERSION_H) $(OBJ)
	${MAKE} -C $(GRPCDIR)
	${MAKE} -C $(CONTROLLERDIR)
	$(CC) $(CFLAGS) $(OBJ) $(GRPC_OBJ) $(CONTROLLER_OBJ) -o $(TARGET) $(LDFLAGS)

install:
	cp $(TARGET) /usr/local/bin/$(TARGET)

test: CFLAGS += -DUNIT_TEST
test: clean $(TARGET)
	${MAKE} -C $(TESTDIR) $(TESTBIN)
	./$(TESTDIR)/$(TESTBIN)
	${MAKE} -C $(CONTROLLERDIR) run-tests

######################################
# Clean 
######################################
clean:
	rm -rf $(OBJ) $(TARGET) .libs $(VERSION_H)
	$(MAKE) -C $(TESTDIR) -f Makefile $@
	$(MAKE) -C $(GRPCDIR) -f Makefile $@
	$(MAKE) -C $(CONTROLLERDIR) -f Makefile $@

uninstall:
	rm -f /usr/local/bin/$(TARGET)
