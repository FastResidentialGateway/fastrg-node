#!/usr/bin/env python3
"""Minimal raw-packet DHCP client used by the FastRG e2e suite."""

import argparse
import ipaddress
import json
import os
import random
import socket
import struct
import time


ETH_P_ALL = 0x0003
ETH_P_IP = 0x0800
VLAN_TYPES = {0x8100, 0x88A8}
SOL_PACKET = 263
PACKET_ADD_MEMBERSHIP = 1
PACKET_DROP_MEMBERSHIP = 2
PACKET_MR_PROMISC = 1

DHCP_CLIENT_PORT = 68
DHCP_SERVER_PORT = 67
DHCP_MAGIC_COOKIE = b"\x63\x82\x53\x63"
DHCP_MESSAGE_TYPE = 53
DHCP_REQUESTED_IP = 50
DHCP_LEASE_TIME = 51
DHCP_SERVER_ID = 54
DHCP_PARAMETER_LIST = 55
DHCP_RENEWAL_TIME = 58
DHCP_REBINDING_TIME = 59
DHCP_CLIENT_ID = 61
DHCP_END = 255

DHCP_DISCOVER = 1
DHCP_OFFER = 2
DHCP_REQUEST = 3
DHCP_ACK = 5
DHCP_NAK = 6
DHCP_RELEASE = 7

MESSAGE_NAMES = {
    DHCP_DISCOVER: "DISCOVER",
    DHCP_OFFER: "OFFER",
    DHCP_REQUEST: "REQUEST",
    DHCP_ACK: "ACK",
    DHCP_NAK: "NAK",
    DHCP_RELEASE: "RELEASE",
}


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
    total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def option(code, value):
    return bytes((code, len(value))) + value


def ipv4_bytes(value):
    return ipaddress.IPv4Address(value).packed


def build_options(message_type, mac, requested_ip=None, server_id=None):
    result = option(DHCP_MESSAGE_TYPE, bytes((message_type,)))
    result += option(DHCP_CLIENT_ID, b"\x01" + mac)
    if requested_ip:
        result += option(DHCP_REQUESTED_IP, ipv4_bytes(requested_ip))
    if server_id:
        result += option(DHCP_SERVER_ID, ipv4_bytes(server_id))
    if message_type in (DHCP_DISCOVER, DHCP_REQUEST):
        result += option(
            DHCP_PARAMETER_LIST,
            bytes((1, 3, 6, DHCP_LEASE_TIME, DHCP_RENEWAL_TIME, DHCP_REBINDING_TIME)),
        )
    return result + bytes((DHCP_END,))


def build_frame(
    mac,
    dst_mac,
    xid,
    message_type,
    src_ip,
    dst_ip,
    ciaddr="0.0.0.0",
    requested_ip=None,
    server_id=None,
    broadcast=False,
):
    flags = 0x8000 if broadcast else 0
    bootp = struct.pack(
        "!BBBBIHH4s4s4s4s16s64s128s",
        1,
        1,
        6,
        0,
        xid,
        0,
        flags,
        ipv4_bytes(ciaddr),
        b"\x00" * 4,
        b"\x00" * 4,
        b"\x00" * 4,
        mac + b"\x00" * 10,
        b"\x00" * 64,
        b"\x00" * 128,
    )
    payload = bootp + DHCP_MAGIC_COOKIE + build_options(
        message_type, mac, requested_ip=requested_ip, server_id=server_id
    )
    udp_len = 8 + len(payload)
    udp = struct.pack("!HHHH", DHCP_CLIENT_PORT, DHCP_SERVER_PORT, udp_len, 0)
    total_len = 20 + udp_len
    ip_without_checksum = struct.pack(
        "!BBHHHBBH4s4s",
        0x45,
        0,
        total_len,
        xid & 0xFFFF,
        0,
        64,
        socket.IPPROTO_UDP,
        0,
        ipv4_bytes(src_ip),
        ipv4_bytes(dst_ip),
    )
    ip_header = ip_without_checksum[:10] + struct.pack("!H", checksum(ip_without_checksum)) + ip_without_checksum[12:]
    return dst_mac + mac + struct.pack("!H", ETH_P_IP) + ip_header + udp + payload


def parse_options(data):
    options = {}
    offset = 0
    while offset < len(data):
        code = data[offset]
        offset += 1
        if code == 0:
            continue
        if code == DHCP_END:
            break
        if offset >= len(data):
            break
        length = data[offset]
        offset += 1
        if offset + length > len(data):
            break
        options[code] = data[offset : offset + length]
        offset += length
    return options


def parse_response(frame, xid, mac):
    if len(frame) < 14:
        return None
    eth_offset = 14
    ether_type = struct.unpack("!H", frame[12:14])[0]
    while ether_type in VLAN_TYPES:
        if len(frame) < eth_offset + 4:
            return None
        ether_type = struct.unpack("!H", frame[eth_offset + 2 : eth_offset + 4])[0]
        eth_offset += 4
    if ether_type != ETH_P_IP or len(frame) < eth_offset + 20:
        return None

    ip_header_len = (frame[eth_offset] & 0x0F) * 4
    udp_offset = eth_offset + ip_header_len
    if len(frame) < udp_offset + 8:
        return None
    src_port, dst_port = struct.unpack("!HH", frame[udp_offset : udp_offset + 4])
    if src_port != DHCP_SERVER_PORT or dst_port != DHCP_CLIENT_PORT:
        return None

    bootp_offset = udp_offset + 8
    if len(frame) < bootp_offset + 240 or frame[bootp_offset] != 2:
        return None
    response_xid = struct.unpack("!I", frame[bootp_offset + 4 : bootp_offset + 8])[0]
    response_mac = frame[bootp_offset + 28 : bootp_offset + 34]
    if response_xid != xid or response_mac != mac:
        return None
    if frame[bootp_offset + 236 : bootp_offset + 240] != DHCP_MAGIC_COOKIE:
        return None

    options = parse_options(frame[bootp_offset + 240 :])
    message_value = options.get(DHCP_MESSAGE_TYPE, b"")
    if len(message_value) != 1:
        return None
    message_type = message_value[0]

    def option_ip(code):
        value = options.get(code)
        return str(ipaddress.IPv4Address(value)) if value and len(value) == 4 else None

    def option_u32(code):
        value = options.get(code)
        return struct.unpack("!I", value)[0] if value and len(value) == 4 else None

    return {
        "received": MESSAGE_NAMES.get(message_type, str(message_type)),
        "xid": f"0x{xid:08x}",
        "yiaddr": str(ipaddress.IPv4Address(frame[bootp_offset + 16 : bootp_offset + 20])),
        "server_id": option_ip(DHCP_SERVER_ID),
        "server_mac": format_mac(frame[6:12]),
        "lease_time": option_u32(DHCP_LEASE_TIME),
        "renewal_time": option_u32(DHCP_RENEWAL_TIME),
        "rebinding_time": option_u32(DHCP_REBINDING_TIME),
    }


class PacketSocket:
    def __init__(self, interface):
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
        self.sock.bind((interface, 0))
        self.membership = struct.pack("IHH8s", socket.if_nametoindex(interface), PACKET_MR_PROMISC, 0, b"\x00" * 8)
        self.sock.setsockopt(SOL_PACKET, PACKET_ADD_MEMBERSHIP, self.membership)

    def close(self):
        try:
            self.sock.setsockopt(SOL_PACKET, PACKET_DROP_MEMBERSHIP, self.membership)
        finally:
            self.sock.close()

    def exchange(self, frame, xid, mac, timeout):
        deadline = time.monotonic() + timeout
        next_send = 0.0
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_send:
                self.sock.send(frame)
                next_send = now + 1.5
            self.sock.settimeout(max(0.05, min(next_send, deadline) - time.monotonic()))
            try:
                packet = self.sock.recv(4096)
            except socket.timeout:
                continue
            response = parse_response(packet, xid, mac)
            if response:
                return response
        raise TimeoutError(f"no DHCP response within {timeout:.1f}s")


def make_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", required=True)
    parser.add_argument("--mac", required=True, type=parse_mac)
    parser.add_argument("--xid", type=lambda value: int(value, 0))
    parser.add_argument("--timeout", type=float, default=6.0)
    parser.add_argument("--ip")
    parser.add_argument("--server-id")
    parser.add_argument("--server-mac", type=parse_mac)
    parser.add_argument("--bogus-ip")
    parser.add_argument(
        "action",
        choices=("discover", "request", "request-renew", "request-rebind", "release", "request-bogus"),
    )
    return parser


def require(args, *names):
    missing = [name for name in names if getattr(args, name.replace("-", "_")) is None]
    if missing:
        raise ValueError(f"{args.action} requires: {', '.join('--' + name for name in missing)}")


def action_frame(args, xid):
    broadcast_mac = b"\xff" * 6
    if args.action == "discover":
        return build_frame(
            args.mac, broadcast_mac, xid, DHCP_DISCOVER, "0.0.0.0", "255.255.255.255", broadcast=True
        )
    if args.action == "request":
        require(args, "ip", "server-id")
        return build_frame(
            args.mac,
            broadcast_mac,
            xid,
            DHCP_REQUEST,
            "0.0.0.0",
            "255.255.255.255",
            requested_ip=args.ip,
            server_id=args.server_id,
            broadcast=True,
        )
    if args.action == "request-renew":
        require(args, "ip", "server-id", "server-mac")
        return build_frame(
            args.mac,
            args.server_mac,
            xid,
            DHCP_REQUEST,
            args.ip,
            args.server_id,
            ciaddr=args.ip,
            server_id=args.server_id,
        )
    if args.action == "request-rebind":
        require(args, "ip")
        return build_frame(
            args.mac,
            broadcast_mac,
            xid,
            DHCP_REQUEST,
            args.ip,
            "255.255.255.255",
            ciaddr=args.ip,
            broadcast=True,
        )
    if args.action == "release":
        require(args, "ip", "server-id", "server-mac")
        return build_frame(
            args.mac,
            args.server_mac,
            xid,
            DHCP_RELEASE,
            args.ip,
            args.server_id,
            ciaddr=args.ip,
            server_id=args.server_id,
        )
    require(args, "bogus-ip", "server-id")
    return build_frame(
        args.mac,
        broadcast_mac,
        xid,
        DHCP_REQUEST,
        "0.0.0.0",
        "255.255.255.255",
        requested_ip=args.bogus_ip,
        server_id=args.server_id,
        broadcast=True,
    )


def main():
    args = make_parser().parse_args()
    if os.geteuid() != 0:
        raise SystemExit("dhcp_client_sim.py must run as root")
    xid = args.xid if args.xid is not None else random.SystemRandom().randrange(1, 2**32)
    frame = action_frame(args, xid)
    packet_socket = PacketSocket(args.interface)
    try:
        if args.action == "release":
            for _ in range(3):
                packet_socket.sock.send(frame)
                time.sleep(0.2)
            result = {
                "action": args.action,
                "sent": "RELEASE",
                "xid": f"0x{xid:08x}",
                "ip": args.ip,
                "server_id": args.server_id,
                "mac": format_mac(args.mac),
            }
        else:
            result = packet_socket.exchange(frame, xid, args.mac, args.timeout)
            result["action"] = args.action
            result["sent"] = "DISCOVER" if args.action == "discover" else "REQUEST"
            result["mac"] = format_mac(args.mac)
            if args.action == "request-bogus":
                result["requested_ip"] = args.bogus_ip
        print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    finally:
        packet_socket.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, TimeoutError, ValueError) as exc:
        print(json.dumps({"error": str(exc)}, sort_keys=True, separators=(",", ":")))
        raise SystemExit(1)
