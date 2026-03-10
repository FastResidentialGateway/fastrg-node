#!/usr/bin/env bash
set -euo pipefail

# Detect OS (Ubuntu / RHEL / CentOS / Rocky / AlmaLinux)
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS_ID=$ID
    OS_VERSION_ID=$VERSION_ID
else
    echo "❌ Cannot detect OS"
    exit 1
fi

echo "Detected OS: $OS_ID $OS_VERSION_ID"

if [[ "$OS_ID" =~ (rhel|centos|rocky|almalinux) ]]; then
    echo "➡ Installing dependencies for RHEL/CentOS family"

    # Extract major version (e.g. 8, 9)
    OS_MAJOR=$(echo "$OS_VERSION_ID" | cut -d. -f1)

    # Install EPEL (automatically match major version)
    EPEL_URL="https://dl.fedoraproject.org/pub/epel/epel-release-latest-${OS_MAJOR}.noarch.rpm"
    echo "Downloading EPEL from: $EPEL_URL"
    wget -q "$EPEL_URL" -O /tmp/epel-release.rpm
    rpm -Uvh /tmp/epel-release.rpm

    # Install packages
    dnf -y --enablerepo=crb install protobuf-devel || true
    dnf -y --enablerepo=devel install meson libconfig-devel protobuf-compiler || true
    dnf -y install grpc grpc-cpp grpc-devel
    dnf -y install python3-pyelftools

elif [[ "$OS_ID" == "ubuntu" ]]; then
    echo "➡ Installing dependencies for Ubuntu (tested on 24.04)"

    apt-get update -y
    apt-get install -y \
        libnuma-dev \
        linux-headers-$(uname -r) \
        git gcc make libtool-bin pkg-config pciutils iproute2 \
        kmod vim net-tools libconfig-dev libjsoncpp-dev \
        cmake libcurl4-openssl-dev libssl-dev \
        libgrpc++-dev protobuf-compiler-grpc libabsl-dev meson \
        python3-pyelftools ninja-build python3-setuptools \
        libcpprest-dev uuid-dev curl libarchive-dev 

    apt-get clean -y
    apt-get autoclean -y
    apt-get autoremove -y

    cd /tmp
    git clone https://github.com/etcd-cpp-apiv3/etcd-cpp-apiv3.git
    cd etcd-cpp-apiv3
    TAG=$(git describe --tags --abbrev=0)
    git checkout $TAG
    mkdir build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j"$(nproc)"
    make install
    ldconfig

else
    echo "❌ Unsupported OS: $OS_ID"
    exit 1
fi

echo "✅ Dependencies installed successfully."
