#!/bin/bash

# Controller Test Script
# This script demonstrates how to run the controller tests

# Get the directory where this script is located and cd to it
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR" || exit 1
echo "📁 Working directory: $SCRIPT_DIR"

echo "🧪 Starting Controller Tests"
echo "================================"

TEST_FAILED=0
ETCD_READY=0
ETCD_MODE=""
ETCD_PID=""
ETCD_DATA_DIR=""

# Function to check if docker is available
check_docker() {
    if ! command -v docker &> /dev/null; then
        echo "❌ Docker not found. Please install Docker to run etcd tests."
        return 1
    fi
    
    if ! docker info &> /dev/null; then
        echo "❌ Docker daemon not running. Please start Docker."
        return 1
    fi
    
    return 0
}

# Function to start etcd in Docker
start_etcd() {
    echo "🐳 Starting etcd server in Docker..."
    ETCD_CONTAINER_NAME="test-etcd-$(date +%s)"
    
    # Start etcd container
    docker run -d --rm \
        --name "$ETCD_CONTAINER_NAME" \
        -p 2379:2379 \
        -p 2380:2380 \
        quay.io/coreos/etcd:v3.5.0 \
        /usr/local/bin/etcd \
        --name s1 \
        --data-dir /etcd-data \
        --listen-client-urls http://0.0.0.0:2379 \
        --advertise-client-urls http://0.0.0.0:2379 \
        --listen-peer-urls http://0.0.0.0:2380 \
        --initial-advertise-peer-urls http://0.0.0.0:2380 \
        --initial-cluster s1=http://0.0.0.0:2380 \
        --initial-cluster-token tkn \
        --initial-cluster-state new \
        --log-level info \
        --logger zap \
        --log-outputs stderr > /dev/null
    
    if [ $? -eq 0 ]; then
        echo "✅ etcd container started: $ETCD_CONTAINER_NAME"
        
        # Wait for etcd to be ready
        echo "⏳ Waiting for etcd to be ready..."
        for i in {1..30}; do
            if docker exec "$ETCD_CONTAINER_NAME" /usr/local/bin/etcdctl endpoint health &> /dev/null; then
                echo "✅ etcd is ready!"
                return 0
            fi
            sleep 1
        done
        
        echo "❌ etcd failed to start properly"
        stop_etcd
        return 1
    else
        echo "❌ Failed to start etcd container"
        return 1
    fi
}

# Function to start etcd as a local process
start_local_etcd() {
    echo "🖥️  Starting local etcd server..."

    if (exec 3<>/dev/tcp/127.0.0.1/2379) 2>/dev/null; then
        exec 3>&-
        exec 3<&-
        echo "⚠️  Port 2379 is already in use. Attempting to start etcd anyway."
    fi

    ETCD_DATA_DIR=$(mktemp -d)
    etcd \
        --name s1 \
        --data-dir "$ETCD_DATA_DIR" \
        --listen-client-urls http://127.0.0.1:2379 \
        --advertise-client-urls http://127.0.0.1:2379 \
        --listen-peer-urls http://127.0.0.1:2380 \
        --initial-advertise-peer-urls http://127.0.0.1:2380 \
        --initial-cluster s1=http://127.0.0.1:2380 \
        --initial-cluster-token tkn \
        --initial-cluster-state new \
        --log-level info \
        --logger zap \
        --log-outputs stderr > /dev/null 2>&1 &
    ETCD_PID=$!

    echo "✅ Local etcd process started: $ETCD_PID"
    echo "⏳ Waiting for etcd to be ready..."
    for i in {1..30}; do
        if etcdctl --endpoints=http://127.0.0.1:2379 endpoint health &> /dev/null; then
            echo "✅ etcd is ready!"
            return 0
        fi
        if ! kill -0 "$ETCD_PID" 2>/dev/null; then
            break
        fi
        sleep 1
    done

    echo "❌ etcd failed to start properly"
    stop_etcd
    return 1
}

# Run etcdctl through the active etcd provider.
etcdctl_command() {
    if [ "$ETCD_MODE" = "docker" ]; then
        docker exec "$ETCD_CONTAINER_NAME" /usr/local/bin/etcdctl "$@"
    else
        etcdctl --endpoints=http://127.0.0.1:2379 "$@"
    fi
}

# Function to stop etcd
stop_etcd() {
    if [ "$ETCD_MODE" = "docker" ] && [ ! -z "$ETCD_CONTAINER_NAME" ]; then
        echo "🧹 Stopping etcd container: $ETCD_CONTAINER_NAME"
        docker stop "$ETCD_CONTAINER_NAME" > /dev/null 2>&1
        echo "✅ etcd container cleaned up"
    elif [ "$ETCD_MODE" = "local" ]; then
        if [ ! -z "$ETCD_PID" ] && kill -0 "$ETCD_PID" 2>/dev/null; then
            echo "🧹 Stopping local etcd process: $ETCD_PID"
            kill -TERM "$ETCD_PID" 2>/dev/null
            wait "$ETCD_PID" 2>/dev/null
            echo "✅ Local etcd process cleaned up"
        fi
        if [ ! -z "$ETCD_DATA_DIR" ]; then
            rm -rf "$ETCD_DATA_DIR"
        fi
    fi
}

# Build tests
echo "📦 Building tests..."
make test
echo ""

# Test 1: Run client test without server (should show connection errors)
echo "🔧 Test 1: Client test without server (expecting connection errors)"
echo "---------------------------------------------------------------"
./test/test_controller
echo ""

# Test 2: Run integrated test with server
echo "🔧 Test 2: Integrated test with server"
echo "---------------------------------------"
echo "Starting server in background..."
./test/test_controller_server &
SERVER_PID=$!

# Give server time to start
sleep 2

echo "Running client test..."
./test/test_controller

# Cleanup server
echo "🧹 Cleaning up controller server..."
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null
echo ""

# Test 3: Run etcd client test
echo "🔧 Test 3: etcd client test"
echo "----------------------------"

if check_docker; then
    ETCD_MODE="docker"
elif command -v etcd &> /dev/null && command -v etcdctl &> /dev/null; then
    ETCD_MODE="local"
fi

if [ ! -z "$ETCD_MODE" ]; then
    # Set up cleanup trap
    trap 'stop_etcd' EXIT

    if { [ "$ETCD_MODE" = "docker" ] && start_etcd; } || { [ "$ETCD_MODE" = "local" ] && start_local_etcd; }; then
        ETCD_READY=1
        echo "Running etcd client test with simulated key changes..."
        if [ -f "./test/test_etcd_client" ]; then
            # Start etcd client test in background
            ./test/test_etcd_client &
            ETCD_CLIENT_PID=$!
            
            # Give the client time to start and establish watchers
            sleep 2
            
            echo "📝 Simulating etcd key changes..."
            
            # Test HSI config changes
            echo "  Creating HSI config for user1..."
            etcdctl_command put "configs/test-node-12345/hsi/user1" \
                '{"user_id":"user1","vlan_id":"100","account_name":"user1@test.com","password":"secret123","dhcp_addr_pool":"192.168.1.10-192.168.1.50","dhcp_subnet":"192.168.1.0/24","dhcp_gateway":"192.168.1.1"}'
            
            sleep 1
            
            echo "  Updating HSI config for user1..."
            etcdctl_command put "configs/test-node-12345/hsi/user1" \
                '{"user_id":"user1","vlan_id":"101","account_name":"user1@test.com","password":"newsecret456","dhcp_addr_pool":"192.168.2.10-192.168.2.50","dhcp_subnet":"192.168.2.0/24","dhcp_gateway":"192.168.2.1"}'
            
            sleep 1
            
            echo "  Creating HSI config for user2..."
            etcdctl_command put "configs/test-node-12345/hsi/user2" \
                '{"user_id":"user2","vlan_id":"200","account_name":"user2@test.com","password":"password789","dhcp_addr_pool":"192.168.3.10-192.168.3.50","dhcp_subnet":"192.168.3.0/24","dhcp_gateway":"192.168.3.1"}'
            
            sleep 1
            
            echo "  Deleting HSI config for user1..."
            etcdctl_command del "configs/test-node-12345/hsi/user1"
            
            sleep 1
            
            
            # Stop the etcd client test
            echo "🛑 Stopping etcd client test..."
            kill $ETCD_CLIENT_PID 2>/dev/null
            wait $ETCD_CLIENT_PID 2>/dev/null
            
            echo ""
            echo "📊 Test Summary - Simulated Events:"
            echo "   ✅ HSI Config: Created user1 (VLAN 100)"
            echo "   ✅ HSI Config: Updated user1 (VLAN 100→101)"
            echo "   ✅ HSI Config: Created user2 (VLAN 200)"
            echo "   ✅ HSI Config: Deleted user1"
            echo ""
            echo "✅ etcd client test completed with simulated events!"
        else
            echo "⚠️  test_etcd_client executable not found. Skipping etcd test."
            echo "   Make sure to build the etcd client test first."
        fi
    else
        echo "❌ Failed to start etcd server. Skipping etcd test."
    fi
else
    echo "⚠️  Docker and local etcd are not available. Skipping etcd test."
    echo "   Install Docker or etcd to enable etcd testing."
fi

echo ""
echo "🔧 Test 4: etcd CAS put test"
echo "-----------------------------"
if [ "$ETCD_READY" -eq 1 ]; then
    if [ -f "./test/test_etcd_cas" ]; then
        ./test/test_etcd_cas
        if [ $? -ne 0 ]; then
            echo "❌ etcd CAS put test failed."
            TEST_FAILED=1
        else
            echo "✅ etcd CAS put test completed!"
        fi
    else
        echo "❌ test_etcd_cas executable not found."
        TEST_FAILED=1
    fi
else
    echo "⚠️  etcd server is not available. Skipping etcd CAS put test."
fi

echo ""
echo "✅ All tests completed!"
exit "$TEST_FAILED"
