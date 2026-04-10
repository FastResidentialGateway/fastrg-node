# FastRG(Fast Residential Gateway) system (Data plane node)

[![BSD license](https://img.shields.io/badge/License-BSD-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)
[![FastRG Node CI](https://github.com/w180112/fastrg-node/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/w180112/fastrg-node/actions/workflows/ci.yml)

## Introduction:

- This is a C based high throughput open source software-defined residential gateway system data plane, part of the FastRG system.
- The FastRG system can be used as a residential gateway connected with BRAS(Broadband Remote Access Server) to provide PPPoE client, NAT and DHCP server function for subscribers.
- The FastRG system supports multiple PPPoE sessions, each PPPoE session is mapped to a unique VLAN ID in data plane.
- The FastRG system support DHCP server and NAT function for subscribers behind the FastRG system.
- For service provider, the FastRG system can help to reduce the CAPEX and OPEX of deploying residential gateway for subscribers by just deploying an ONT with L2 bridge in subscriber's home.
- Because of the centralized management of Residential Gateway system, service provider can easily maintain and update the FastRG system without going to subscriber's home. This can greatly reduce the network security risks of service provider.
- The FastRG system also provides both a command line interface and a [central controller](https://github.com/w180112/fastrg-controller) for administrator to manage. Administrator can easily deploy the FastRG system in a cloud native environment with Kubernetes and Docker image.
- The FastRG system data plane is implemented based on DPDK library to achieve high performance packet processing.

## System required:

- DPDK capable NIC with at least 2 ports
	- Suggest to use Intel E800 series NICs to acquire best performance.
- 8GB RAM
- At least 7 CPU cores.

## How to use:

- The FastRG system is consisted of control plane and data plane, the [FastRG controller](https://github.com/w180112/fastrg-controller) and this repository.
	- The control plane is used to manage the data plane node and network functions. 
	- The data plane is used to forward packets between LAN and WAN port.
- User can deploy both control plane and data plane or just data plane only.
	- The control plane and data plane can run on the same server or different servers.
	- The control plane and data plane communicate with each other through Etcd and gRPC.
- If user wants to deploy the data plane only, the data plane provides Unix domain socket to communicate with built-in CLI tool, user can modify the Unix socket path in ***config.cfg*** file.
- In the data plane, 2 DPDK ethernet ports are needed, the first one is used to Rx/Tx packets to LAN port and the second one is used to Rx/Tx packets to WAN port.
- To run the FastRG system data plane from scratch, please follow the steps below:

Git clone this repository

	# git clone https://github.com/w180112/fastrg-node.git
	# TAG=$(git describe --tags --abbrev=0)
    # git checkout $TAG

Type

	# cd fastrg-node
	# git submodule update --init --recursive

For first time build, please use ./essentials.sh to install dependencies and then run ./boot.sh to build DPDK library, libutil and FastRG

	# ./boot.sh

For just FastRG build, clean, install and uninstall, please use makefile

	# make && make install
	# make clean && make uninstall

Then

	# fastrg <dpdk eal options>

e.g.

	# fastrg -l 0-8 -n 4 -a 0000:04:00.0 -a 0000:08:00.0

For using FastRG system data plane in Docker,

	# docker build --no-cache -t fastrg:latest .
	# mount -t hugetlbfs -o pagesize=1G none /dev/hugepages1G
	# docker run -d --net=host --privileged -v /sys/bus/pci/devices:/sys/bus/pci/devices \
	-v /sys/kernel/mm/hugepages:/sys/kernel/mm/hugepages -v /sys/devices/system/node:/sys/devices/system/node -v /dev:/dev -v /etc/fastrg:/etc/fastrg fastrg:latest fastrg -l 0-8 -n 4 -a 0000:04:00.0 -a 0000:08:00.0

For Intel E800 series NICs, please use this command to start Docker container

	# docker run -d --net=host --privileged -v /sys/bus/pci/devices:/sys/bus/pci/devices \
	-v /sys/kernel/mm/hugepages:/sys/kernel/mm/hugepages -v /sys/devices/system/node:/sys/devices/system/node -v /dev:/dev -v /etc/fastrg:/etc/fastrg -v/lib/firmware/updates/intel/ice/ddp/:/lib/firmware/updates/intel/ice/ddp/ -v /lib/firmware/intel/ice/ddp:/lib/firmware/intel/ice/ddp fastrg:latest fastrg -l 0-8 -n 4 -a 0000:04:00.0 -a 0000:08:00.0

### SDN mode(Control plane + Data plane)

Update the endpoints of Etcd server and gRPC server address in configuration file ***config.cfg***.

Please refer to FastRG controller web page to manage the system.
 - The CLI tool ***fastrg-cli*** can also be used to connect to the FastRG system data plane while using SDN mode.

### Standalone mode(Only data plane)

If there is only FastRG system data plane is deployed, after data plane started, user can use cli tool ***fastrg-cli*** to connect to the system and input "?" command to show available commands.

To configure PPPoE subscriber account, DHCP server pool and VLAN ID mapping, please refer to command ***config***.

	FastRG> config add user 1 vlan 3 pppoe account admin password passwd dhcp pool 192.168.3.2~192.168.3.201 subnet 255.255.255.0 gateway 192.168.3.1
	FastRG> config del user 1

### Example CLI commands:
Use command ***exec*** to determine which user start/stop a PPPoE connection, e.g.:

To start specific subscriber 1 PPPoE connection and DHCP server.

	FastRG> exec hsi start 1

To disconnect specific subscriber 1 PPPoE connection and DHCP server.

	FastRG> exec hsi stop 1

To show current PPPoE connection status.

	FastRG> show hsi

To show current DHCP server status.

	FastRG> show dhcp

To show current system statistics.

	FastRG> show system info

To configure SNAT port forwarding for specific subscriber 1.

	FastRG> config add user 1 snat eport 55688 dip 192.168.3.2 iport 8080

For hugepages, NIC binding and other system configuration, please refer to DPDK documentation: [DPDK doc](http://doc.dpdk.org/guides/linux_gsg/)

## Note:

1. Subscriber devices behind FastRG should use DHCP to get IP address or set the default gateway address to their end device.
	- The DHCP ip address pool can be configured via control plane or FastRG CLI.
2. In configuration file ***config.cfg***, administrator should use the value ***UserCount*** to specify the initial number of FastRG subscribers. By default, there are only 10 subscribers.
	- This value can be configured via FastRG controller or FastRG CLI tool.
	- The value range can be set from 2 to 4000.
3. In data plane, all packets received at FastRG system should include a single tag vlan.
4. All DPDK EAL lcores should be on the same CPU socket.
5. The FastRG node supports PPPoE RSS while using Intel E800 series NICs. For every two more CPU cores, the FastRG node uses 1 more Rx queue pair for LAN and WAN port. Minimum Rx queue count is 2, the maximum is 16. 
	- Please replace DDP package to ice_comms.pkg in /lib/firmware/updates/intel/ice/ddp/ and /lib/firmware/intel/ice/ddp/ and rename it to ice.pkg to enable PPPoE RSS feature. Th DDP package can be download from https://www.intel.com/content/www/us/en/download/19660/intel-ethernet-800-series-dynamic-device-personalization-ddp-for-telecommunication-comms-package.html

## Test environment:

1. Ubuntu 24.04 with Intel E810, X710, X520 and Mellanox CX4 network card
2. Successfully test control plane and data plane with CHT(Chunghwa Telecom Co., Ltd.) BRAS, open source RP-PPPoE and Mikrotik RouterOS PPPoE server.
3. DPDK 24.11

## TODO:

1. Increase unit tests converage
2. Support IPv6
3. Support IGMP/IPTV