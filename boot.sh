#!/bin/bash

set -ex

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
    # Get controller gRPC interface
    pushd $path/northbound/controller

    PROTO_DIR="proto"
    PROTO_FILE="$PROTO_DIR/controller.proto"
    mkdir -p $PROTO_DIR

    # Get repository tag
    pushd $path > /dev/null
    CURRENT_TAG=$(git describe --tags --exact-match 2>/dev/null || git describe --tags --abbrev=0 2>/dev/null || echo "master")
    popd > /dev/null

    # Try to use tag, use master if not found
    PROTO_URLS=(
        "https://raw.githubusercontent.com/FastResidentialGateway/fastrg-controller/$CURRENT_TAG/proto/controller.proto"
        "https://raw.githubusercontent.com/FastResidentialGateway/fastrg-controller/master/proto/controller.proto"
    )

    DOWNLOAD_SUCCESS=0
    for PROTO_URL in "${PROTO_URLS[@]}"; do
        echo "  Trying: $PROTO_URL"
        
        if curl -fsSL "$PROTO_URL" -o $PROTO_FILE.tmp 2>/dev/null; then
            if [ -s $PROTO_FILE.tmp ]; then
                mv $PROTO_FILE.tmp $PROTO_FILE
                echo "Downloaded controller.proto successfully from: $PROTO_URL"
                DOWNLOAD_SUCCESS=1
                break
            fi
        fi
        
        rm -f $PROTO_FILE.tmp
    done

    if [ $DOWNLOAD_SUCCESS -eq 0 ]; then
        echo "Failed to download controller.proto"
        echo "   Tried tag: $CURRENT_TAG and master branch"
        popd
        exit 1
    fi
    
    popd
}

path=$(get_script_dir)
pushd $path
git submodule update --init --recursive
popd

# Uncomment this line to enable downloading controller.proto after release
download_controller_grpc

pushd $path/lib/dpdk && git checkout v24.11.4 && meson setup $path/lib/dpdk_build
popd
pushd $path/lib/dpdk_build
meson configure -Denable_kmods=true
meson configure -Dexamples="" 
meson configure -Dtests=false 
meson configure -Ddisable_apps="test-eventdev,test-gpudev,test-mldev,test-pipeline,test-regex,test-sad,test-security-perf,dumpcap,graph,proc-info,test-bbdev,test-compress-perf,test-dma-perf,test-acl,test-cmdline,test-fib,test-flow-perf,test-crypto-perf,test-pmd" 
meson configure -Ddisable_libs="bbdev,compressdev,gpudev,mldev,rawdev,regexdev"
meson configure -Ddisable_drivers="common/dpaax,common/octeontx,common/octeontx2,common/cpt,common/sfc_efx,bus/ifpga,net/ark,net/atlantic,net/axgbe,net/hinic,net/hns3,net/ngbe,net/txgbe,net/cxgbe,net/enic,net/fm10k,net/qede,net/sfc,net/thunderx,net/zxdh,raw/ioat,raw/ntb,raw/skeleton,raw/cnxk_bphy,raw/cnxk_gpio,crypto/ccp,crypto/openssl,crypto/nitrox,crypto/null,crypto/scheduler,crypto/bcmfs,crypto/cnxk,baseband/null,baseband/acc,baseband/la12xx,baseband/fpga_5gnr_fec,baseband/turbo_sw,baseband/fpga_lte_fec,event/cnxk,event/dlb2,event/dpaa,event/dpaa2,event/octeontx2,event/opdl,event/skeleton,event/sw,event/dsw,event/octeontx,compress/octeontx_compress,compress/qat,compress/zlib,regex/cn9k,ml/cnxk,vdpa/ifc,vdpa/nfp,vdpa/sfc"
#meson configure -Denable_trace_fp=true -Dc_args='-DALLOW_EXPERIMENTAL_API'
ninja && meson install
ldconfig
popd
pushd $path/lib/libutil
autoreconf --install
./configure
make && make install
ldconfig
popd
pushd $path/northbound/controller
# Generate protobuf and gRPC sources for controller
if command -v protoc &> /dev/null && command -v grpc_cpp_plugin &> /dev/null; then
    protoc -I proto --cpp_out=proto --grpc_out=proto --plugin=protoc-gen-grpc=`which grpc_cpp_plugin` proto/controller.proto
    echo "✅ Controller protobuf files generated"
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
echo "✅ FastRG installed successfully."
