############################################################
# FastRG unit test makefile
############################################################

######################################
# Set variable
######################################	
CC = gcc
INCLUDE = -I. -I../northbound/grpc -I../northbound/controller $(shell pkg-config --cflags jsoncpp 2>/dev/null)
CFLAGS = $(INCLUDE) -Wall -g $(shell pkg-config --cflags libdpdk) -DALLOW_EXPERIMENTAL_API -DUNIT_TEST

LDFLAGS = $(shell pkg-config --static --libs libdpdk) -lutils -lconfig -luuid -Wl,--start-group -lstdc++ $(shell pkg-config --libs grpc++ protobuf jsoncpp) -letcd-cpp-api $(shell pkg-config --libs rdkafka) -lgrpc_unsecure -lgrpc++_unsecure -laddress_sorting -lpthread -Wl,--end-group

# Coverage build, driven by the top-level `make coverage` target (which
# exports COVERAGE=1): the ../src and ../northbound objects are
# gcov-instrumented there, so the link needs --coverage to pull in libgcov.
# Instrumenting the local objects also covers product sources compiled here
# (e.g. ../northbound/cmdline/ip_pool_range.c); the unit_test/ harness files
# themselves are filtered out of the report by the top-level target.
ifeq ($(COVERAGE),1)
CFLAGS += --coverage
# -lcpprest: at -O0 the pplx/cpprest calls in ../northbound/controller/
# etcd_client.o are no longer inlined away, so the link needs libcpprest
# spelled out explicitly.
LDFLAGS += --coverage -lcpprest
endif

TARGET = unit-tester
SRC = $(wildcard *.c) $(wildcard pppd/*.c) $(wildcard dhcpd/*.c) $(wildcard dhcp6/*.c) $(wildcard dnsd/*.c) $(wildcard cmdline/*.c)
CLI_TEST_OBJ = ip_pool_range_test_impl.o
OBJ = $(SRC:.c=.o) $(CLI_TEST_OBJ)

ifneq ($(shell pkg-config --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found")
endif

.PHONY: main $(TARGET)
all: main $(TARGET)
######################################
# Compile & Link
# 	Must use \tab key after new line
######################################
main: 
	# Ensure all dependencies are built
	@make -C ..
$(CLI_TEST_OBJ): ../northbound/cmdline/ip_pool_range.c ../northbound/cmdline/ip_pool_range.h
	$(CC) $(CFLAGS) -c $< -o $@
$(TARGET): $(OBJ)
	main_obj="$(OBJ) $(wildcard ../src/pppd/*.o) $(wildcard ../src/dhcpd/*.o) $(wildcard ../src/dhcp6/*.o) $(wildcard ../src/dnsd/*.o) \
        $(filter-out ../src/main.o, $(wildcard ../src/*.o)) \
        $(filter-out ../northbound/grpc/fastrg_grpc_client.o, $(wildcard ../northbound/grpc/*.o)) \
        $(wildcard ../northbound/controller/*.o) $(wildcard ../northbound/controller/proto/*.o)"; \
	$(CC) $(CFLAGS) $$main_obj -o $(TARGET) $(LDFLAGS)

######################################
# Clean 
######################################
clean:
	rm -rf $(OBJ) $(TARGET) .libs 
