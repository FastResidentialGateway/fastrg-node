CXX = g++
PROTOC = protoc
GRPC_CPP_PLUGIN = grpc_cpp_plugin
GRPC_CPP_PLUGIN_PATH ?= `which $(GRPC_CPP_PLUGIN)`

PROTOS_PATH = proto
PROTO_FILE = $(PROTOS_PATH)/controller.proto

CXXFLAGS = -std=c++17 -g -O3 -DNB_TEST $(shell pkg-config --cflags protobuf grpc++) $(shell pkg-config --cflags jsoncpp) $(shell pkg-config --cflags rdkafka)
LDFLAGS = $(shell pkg-config --libs protobuf grpc++) $(shell pkg-config --libs jsoncpp) -letcd-cpp-api $(shell pkg-config --libs rdkafka)

PROTO_SOURCES = $(PROTOS_PATH)/controller.pb.cc $(PROTOS_PATH)/controller.grpc.pb.cc
PROTO_HEADERS = $(PROTOS_PATH)/controller.pb.h $(PROTOS_PATH)/controller.grpc.pb.h
PROTO_OBJECTS = $(PROTOS_PATH)/controller.pb.o $(PROTOS_PATH)/controller.grpc.pb.o

# Kafka events protobuf (in-tree build source; keep identical to the controller's
# copy for wire compatibility — same field numbers/types).
EVENTS_PROTO_FILE = $(PROTOS_PATH)/kafka-events.proto
EVENTS_PROTO_SRC = $(PROTOS_PATH)/kafka-events.pb.cc
EVENTS_PROTO_HDR = $(PROTOS_PATH)/kafka-events.pb.h
EVENTS_PROTO_OBJ = $(PROTOS_PATH)/kafka-events.pb.o

SOURCES = controller_client.cpp etcd_client.cpp kafka_producer.cpp
OBJECTS = controller_client.o etcd_client.o kafka_producer.o $(PROTO_OBJECTS) $(EVENTS_PROTO_OBJ)

# Test related variables
TEST_DIR = test
TEST_CLIENT = $(TEST_DIR)/test_controller
TEST_SERVER = $(TEST_DIR)/test_controller_server
TEST_ETCD = $(TEST_DIR)/test_etcd_client
TEST_CLIENT_SRC = $(TEST_DIR)/test_controller.c
TEST_SERVER_SRC = $(TEST_DIR)/test_controller_server.cpp
TEST_ETCD_SRC = $(TEST_DIR)/test_etcd_client.cpp

.PHONY: all clean test test-client test-server test-etcd run-tests

all: $(OBJECTS)

# Generate protobuf and gRPC sources
$(PROTO_SOURCES) $(PROTO_HEADERS): $(PROTO_FILE)
	$(PROTOC) -I $(PROTOS_PATH) --cpp_out=$(PROTOS_PATH) --grpc_out=$(PROTOS_PATH) --plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN_PATH) $(PROTO_FILE)

# Compile protobuf sources
%.pb.o: %.pb.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile gRPC sources  
%.grpc.pb.o: %.grpc.pb.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile controller client
controller_client.o: controller_client.cpp $(PROTO_HEADERS)
	$(CXX) $(CXXFLAGS) -c controller_client.cpp -o controller_client.o

# Compile etcd client
etcd_client.o: etcd_client.cpp etcd_client.h
	$(CXX) $(CXXFLAGS) -c etcd_client.cpp -o etcd_client.o

# Generate Kafka events protobuf C++ from the shared contract
$(EVENTS_PROTO_SRC) $(EVENTS_PROTO_HDR): $(EVENTS_PROTO_FILE)
	$(PROTOC) -I $(dir $(EVENTS_PROTO_FILE)) --cpp_out=$(PROTOS_PATH) $(EVENTS_PROTO_FILE)

# Compile Kafka producer
kafka_producer.o: kafka_producer.cpp kafka_producer.h $(EVENTS_PROTO_HDR)
	$(CXX) $(CXXFLAGS) -c kafka_producer.cpp -o kafka_producer.o

# Test targets
test: test-client test-server test-etcd
	@echo "✅ All controller tests built successfully"
	@echo "Usage:"
	@echo "  ./$(TEST_CLIENT)         - Run controller client test"
	@echo "  ./$(TEST_SERVER)         - Run controller server test"
	@echo "  ./$(TEST_ETCD)           - Run etcd client test"
	@echo "  make run-tests           - Run automated test suite"
	@echo "  ./run_tests.sh           - Run automated test suite (alternative)"

run-tests: test
	@echo "🧪 Running automated test suite..."
	./run_tests.sh

test-client: $(TEST_CLIENT)
	@echo "✅ Controller client test built: $(TEST_CLIENT)"

test-server: $(TEST_SERVER)
	@echo "✅ Controller server test built: $(TEST_SERVER)"

test-etcd: $(TEST_ETCD)
	@echo "✅ Etcd client test built: $(TEST_ETCD)"

$(TEST_CLIENT): $(TEST_CLIENT_SRC) $(OBJECTS)
	gcc -I. -o $(TEST_CLIENT) $(TEST_CLIENT_SRC) controller_client.o $(PROTO_OBJECTS) \
		-lstdc++ $(LDFLAGS) -laddress_sorting -lpthread

$(TEST_SERVER): $(TEST_SERVER_SRC) $(PROTO_OBJECTS)
	$(CXX) $(CXXFLAGS) -I. -o $(TEST_SERVER) $(TEST_SERVER_SRC) $(PROTO_OBJECTS) \
		$(LDFLAGS) -laddress_sorting -lpthread

$(TEST_ETCD): $(TEST_ETCD_SRC) $(OBJECTS)
	$(CXX) $(CXXFLAGS) -I. -o $(TEST_ETCD) $(TEST_ETCD_SRC) etcd_client.o ../../src/dbg.o \
		$(LDFLAGS) -laddress_sorting -lpthread

clean:
	rm -f $(OBJECTS) $(PROTO_SOURCES) $(PROTO_HEADERS) $(EVENTS_PROTO_SRC) $(EVENTS_PROTO_HDR)
	rm -f $(TEST_CLIENT) $(TEST_SERVER) $(TEST_ETCD)