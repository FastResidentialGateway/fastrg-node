############################################################
# FastRG unit test makefile
############################################################

######################################
# Set variable
######################################	
CC = gcc
INCLUDE = -I../northbound/grpc -I../northbound/controller $(shell pkg-config --cflags jsoncpp 2>/dev/null)
CFLAGS = $(INCLUDE) -Wall -g $(shell pkg-config --cflags libdpdk) -DALLOW_EXPERIMENTAL_API -DUNIT_TEST

LDFLAGS = $(shell pkg-config --static --libs libdpdk) -lutils -lconfig -luuid -Wl,--start-group -lstdc++ $(shell pkg-config --libs grpc++ protobuf jsoncpp) -letcd-cpp-api -lgrpc_unsecure -lgrpc++_unsecure -laddress_sorting -lpthread -Wl,--end-group

TARGET = unit-tester
SRC = $(wildcard *.c) $(wildcard pppd/*.c) $(wildcard dhcpd/*.c) $(wildcard dnsd/*.c)
OBJ = $(SRC:.c=.o)

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
$(TARGET): $(OBJ)
	main_obj="$(OBJ) $(wildcard ../src/pppd/*.o) $(wildcard ../src/dhcpd/*.o) $(wildcard ../src/dnsd/*.o) \
        $(filter-out ../src/main.o, $(wildcard ../src/*.o)) \
        $(filter-out ../northbound/grpc/fastrg_grpc_client.o, $(wildcard ../northbound/grpc/*.o)) \
        $(wildcard ../northbound/controller/*.o) $(wildcard ../northbound/controller/proto/*.o)"; \
	$(CC) $(CFLAGS) $$main_obj -o $(TARGET) $(LDFLAGS)

######################################
# Clean 
######################################
clean:
	rm -rf $(OBJ) $(TARGET) .libs 
