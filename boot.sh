#!/bin/bash

set -ex

ENV_ONLY=false
SKIP_ENV=false
for arg in "$@"; do
    case $arg in
        --env-only)  ENV_ONLY=true ;;
        --skip-env)  SKIP_ENV=true ;;
    esac
done

get_script_dir () {
     SOURCE="${BASH_SOURCE[0]}"
     while [ -h "$SOURCE" ]; do
          DIR="$( cd -P "$( dirname "$SOURCE" )" && pwd )"
          SOURCE="$( readlink "$SOURCE" )"
          [[ $SOURCE != /* ]] && SOURCE="$DIR/$SOURCE"
     done
     DIR="$( cd -P "$( dirname "$SOURCE" )" && pwd )"
     echo "$DIR"
}

download_controller_grpc() {
    # Get the shared contracts from the controller repo:
    #   - controller.proto   (CLI-facing gRPC config service)
    #   - kafka-events.proto (node -> controller Kafka event schema)
    pushd $path/northbound/controller

    PROTO_DIR="proto"
    mkdir -p $PROTO_DIR

    # Get repository tag
    pushd $path > /dev/null
    CURRENT_TAG=$(git describe --tags --exact-match 2>/dev/null || git describe --tags --abbrev=0 2>/dev/null || echo "master")
    popd > /dev/null

    # Single source of truth = the controller repo. Try the matching tag first,
    # fall back to master.
    PROTO_NAMES=("controller.proto" "kafka-events.proto")
    for PROTO_NAME in "${PROTO_NAMES[@]}"; do
        PROTO_FILE="$PROTO_DIR/$PROTO_NAME"
        PROTO_URLS=(
            "https://raw.githubusercontent.com/FastResidentialGateway/fastrg-controller/$CURRENT_TAG/proto/$PROTO_NAME"
            "https://raw.githubusercontent.com/FastResidentialGateway/fastrg-controller/master/proto/$PROTO_NAME"
        )

        DOWNLOAD_SUCCESS=0
        for PROTO_URL in "${PROTO_URLS[@]}"; do
            echo "  Trying: $PROTO_URL"
            if curl -fsSL "$PROTO_URL" -o $PROTO_FILE.tmp 2>/dev/null; then
                if [ -s $PROTO_FILE.tmp ]; then
                    mv $PROTO_FILE.tmp $PROTO_FILE
                    echo "Downloaded $PROTO_NAME successfully from: $PROTO_URL"
                    DOWNLOAD_SUCCESS=1
                    break
                fi
            fi
            rm -f $PROTO_FILE.tmp
        done

        if [ $DOWNLOAD_SUCCESS -eq 0 ]; then
            # Fall back to the bundled copy under docs/contracts if present
            # (e.g. kafka-events.proto before it is published in the controller repo).
            if [ -f "$path/docs/contracts/$PROTO_NAME" ]; then
                cp "$path/docs/contracts/$PROTO_NAME" "$PROTO_FILE"
                echo "Using bundled docs/contracts/$PROTO_NAME (controller-repo download unavailable)"
            else
                echo "Failed to download $PROTO_NAME"
                echo "   Tried tag: $CURRENT_TAG and master branch; no local fallback"
                popd
                exit 1
            fi
        fi
    done

    popd
}

build_dpdk() {
    pushd $path/lib/dpdk && git checkout v24.11.4 && meson setup $path/lib/dpdk_build
    popd
    pushd $path/lib/dpdk_build
    meson configure -Dexamples=""
    meson configure -Dtests=false
    meson configure -Ddisable_apps="test-eventdev,test-gpudev,test-mldev,test-pipeline,test-regex,test-sad,test-security-perf,dumpcap,graph,proc-info,test-bbdev,test-compress-perf,test-dma-perf,test-a[...]"
    meson configure -Ddisable_libs="bbdev,compressdev,gpudev,mldev,rawdev,regexdev"
    meson configure -Ddisable_drivers="common/dpaax,common/octeontx,common/octeontx2,common/cpt,common/sfc_efx,bus/ifpga,net/ark,net/atlantic,net/axgbe,net/hinic,net/hns3,net/ngbe,net/txgbe,net/cxgbe,[...]"
    #meson configure -Denable_trace_fp=true -Dc_args='-DALLOW_EXPERIMENTAL_API'
    ninja && meson install
    ldconfig
    popd
}

build_libutil() {
    pushd $path/lib/libutil
    autoreconf --install
    ./configure
    make && make install
    ldconfig
    popd
}

build_fastrg() {
    download_controller_grpc

    pushd $path/northbound/controller
    # Generate protobuf and gRPC sources for controller
    if command -v protoc &> /dev/null && command -v grpc_cpp_plugin &> /dev/null; then
        protoc -I proto --cpp_out=proto --grpc_out=proto --plugin=protoc-gen-grpc=`which grpc_cpp_plugin` proto/controller.proto
        protoc -I proto --cpp_out=proto proto/kafka-events.proto
        echo "✅ Controller + Kafka-events protobuf files generated"
    else
        echo "❌ protoc or grpc_cpp_plugin not found. Please install protobuf-compiler and grpc tools"
        exit 1
    fi
    popd

    pushd $path
    make && make install
    mkdir -p /var/log/fastrg
    mkdir -p /var/run/fastrg
    mkdir -p /etc/fastrg
    cp config.cfg /etc/fastrg/config.cfg
    popd

    pushd $path/northbound/cmdline
    make && make install || true
    popd
}

path=$(get_script_dir)

pushd $path
git submodule update --init --recursive
popd

if [ "$SKIP_ENV" = false ]; then
    build_dpdk
    build_libutil
fi

if [ "$ENV_ONLY" = true ]; then
    echo "✅ Build environment (DPDK + libutil) ready."
    exit 0
fi

build_fastrg
echo "✅ FastRG installed successfully."
