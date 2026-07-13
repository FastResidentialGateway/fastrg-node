#!/usr/bin/env python3
"""Minimal deterministic UDP A-record responder for the DNS cache e2e test."""

import argparse
import ipaddress
import socket
import struct


def question_end(packet):
    offset = 12
    while offset < len(packet):
        label_len = packet[offset]
        offset += 1
        if label_len == 0:
            break
        if label_len & 0xC0 or offset + label_len > len(packet):
            return None
        offset += label_len
    if offset + 4 > len(packet):
        return None
    return offset + 4


def build_response(query, answer, ttl):
    if len(query) < 12:
        return None
    query_id, _flags, qdcount = struct.unpack("!HHH", query[:6])
    if qdcount != 1:
        return None
    end = question_end(query)
    if end is None:
        return None

    header = struct.pack("!HHHHHH", query_id, 0x8180, 1, 1, 0, 0)
    answer_record = struct.pack(
        "!HHHLH4s",
        0xC00C,
        1,
        1,
        ttl,
        4,
        ipaddress.IPv4Address(answer).packed,
    )
    return header + query[12:end] + answer_record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", required=True)
    parser.add_argument("--answer", required=True)
    parser.add_argument("--ttl", required=True, type=int)
    parser.add_argument("--log", required=True)
    args = parser.parse_args()

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock, open(
        args.log, "a", buffering=1, encoding="utf-8"
    ) as query_log:
        sock.bind((args.bind, 53))
        while True:
            query, peer = sock.recvfrom(4096)
            response = build_response(query, args.answer, args.ttl)
            if response is None:
                continue
            query_log.write(f"{peer[0]}:{peer[1]}\n")
            sock.sendto(response, peer)


if __name__ == "__main__":
    main()
