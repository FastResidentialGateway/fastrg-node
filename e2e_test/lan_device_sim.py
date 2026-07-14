#!/usr/bin/env python3
"""Send raw ARP and UDP frames for a virtual LAN device."""

import argparse
import ipaddress
import json
import os
import socket
import struct
import time


ETH_P_ALL = 0x0003
ETH_P_IP = 0x0800
ETH_P_ARP = 0x0806


def parse_mac(value):
    parts = value.split(":")
    if len(parts) != 6:
        raise argparse.ArgumentTypeError("MAC must contain six octets")
    try:
        return bytes(int(part, 16) for part in parts)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("invalid MAC address") from exc


def format_mac(value):
    return ":".join(f"{octet:02x}" for octet in value)


def checksum(data):
    if len(data) % 2:
        data += b"\x00"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def ipv4_bytes(value):
    return ipaddress.IPv4Address(value).packed


def build_arp_request(src_mac, src_ip, target_ip):
    broadcast_mac = b"\xff" * 6
    arp = struct.pack(
        "!HHBBH6s4s6s4s",
        1,
        ETH_P_IP,
        6,
        4,
        1,
        src_mac,
        ipv4_bytes(src_ip),
        b"\x00" * 6,
        ipv4_bytes(target_ip),
    )
    return broadcast_mac + src_mac + struct.pack("!H", ETH_P_ARP) + arp


def build_udp_frame(src_mac, dst_mac, src_ip, dst_ip, src_port, dst_port, payload, packet_id):
    src_ip_bytes = ipv4_bytes(src_ip)
    dst_ip_bytes = ipv4_bytes(dst_ip)
    udp_len = 8 + len(payload)
    udp_without_checksum = struct.pack("!HHHH", src_port, dst_port, udp_len, 0)
    pseudo_header = src_ip_bytes + dst_ip_bytes + struct.pack("!BBH", 0, socket.IPPROTO_UDP, udp_len)
    udp_checksum = checksum(pseudo_header + udp_without_checksum + payload)
    if udp_checksum == 0:
        udp_checksum = 0xFFFF
    udp = struct.pack("!HHHH", src_port, dst_port, udp_len, udp_checksum)

    total_len = 20 + udp_len
    ip_without_checksum = struct.pack(
        "!BBHHHBBH4s4s",
        0x45,
        0,
        total_len,
        packet_id & 0xFFFF,
        0,
        64,
        socket.IPPROTO_UDP,
        0,
        src_ip_bytes,
        dst_ip_bytes,
    )
    ip_header = (
        ip_without_checksum[:10]
        + struct.pack("!H", checksum(ip_without_checksum))
        + ip_without_checksum[12:]
    )
    return dst_mac + src_mac + struct.pack("!H", ETH_P_IP) + ip_header + udp + payload


def make_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", required=True)
    parser.add_argument("--src-mac", required=True, type=parse_mac)
    parser.add_argument("--src-ip", required=True)
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--target-ip")
    parser.add_argument("--dst-mac", type=parse_mac)
    parser.add_argument("--dst-ip")
    parser.add_argument("--dst-port", type=int)
    parser.add_argument("--src-port-start", type=int)
    parser.add_argument("--flow-count", type=int, default=1)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("action", choices=("arp-request", "udp"))
    return parser


def require(args, *names):
    missing = [name for name in names if getattr(args, name.replace("-", "_")) is None]
    if missing:
        raise ValueError(f"{args.action} requires: {', '.join('--' + name for name in missing)}")


def validate_port(port, name):
    if not 1 <= port <= 65535:
        raise ValueError(f"{name} must be between 1 and 65535")


def main():
    args = make_parser().parse_args()
    if os.geteuid() != 0:
        raise SystemExit("lan_device_sim.py must run as root")
    if args.count < 1 or args.flow_count < 1 or args.repeat < 1:
        raise ValueError("count, flow-count, and repeat must be positive")

    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    sock.bind((args.interface, 0))
    try:
        if args.action == "arp-request":
            require(args, "target-ip")
            frame = build_arp_request(args.src_mac, args.src_ip, args.target_ip)
            for _ in range(args.count):
                sock.send(frame)
                time.sleep(0.05)
            result = {
                "action": args.action,
                "count": args.count,
                "mac": format_mac(args.src_mac),
                "src_ip": args.src_ip,
                "target_ip": args.target_ip,
            }
        else:
            require(args, "dst-mac", "dst-ip", "dst-port", "src-port-start")
            validate_port(args.dst_port, "dst-port")
            validate_port(args.src_port_start, "src-port-start")
            validate_port(args.src_port_start + args.flow_count - 1, "last source port")
            ports = list(range(args.src_port_start, args.src_port_start + args.flow_count))
            for repeat_index in range(args.repeat):
                for flow_index, src_port in enumerate(ports):
                    payload = f"multi-lan:{format_mac(args.src_mac)}:{src_port}".encode("ascii")
                    frame = build_udp_frame(
                        args.src_mac,
                        args.dst_mac,
                        args.src_ip,
                        args.dst_ip,
                        src_port,
                        args.dst_port,
                        payload,
                        0x2400 + repeat_index * args.flow_count + flow_index,
                    )
                    sock.send(frame)
                    time.sleep(0.02)
            result = {
                "action": args.action,
                "dst_ip": args.dst_ip,
                "dst_port": args.dst_port,
                "flow_count": args.flow_count,
                "mac": format_mac(args.src_mac),
                "repeat": args.repeat,
                "src_ip": args.src_ip,
                "src_ports": ports,
            }
        print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    finally:
        sock.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as exc:
        print(json.dumps({"error": str(exc)}, sort_keys=True, separators=(",", ":")))
        raise SystemExit(1)
