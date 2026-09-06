#!/usr/bin/env python3
"""Create the Bafang M820 BL820 update container from a raw linked application binary.

This is a byte-for-byte Python port of scripts/prepare-m820-bl820.ps1 so the verified
build does not depend on PowerShell. It intentionally preserves the historical CRC/data
ordering rather than substituting zlib.crc32.
"""
from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

POLY = 0x04C11DB7
HEADER_PREFIX = bytes.fromhex("0145824040000000000000000000")
RESERVED = bytes(14)


def _crc32_table() -> list[int]:
    table: list[int] = []
    for i in range(256):
        v = (i << 24) & 0xFFFFFFFF
        for _ in range(8):
            v = (((v << 1) ^ POLY) if (v & 0x80000000) else (v << 1)) & 0xFFFFFFFF
        table.append(v)
    return table


_CRC32_TABLE = _crc32_table()


def _crc32_update_byte(crc: int, value: int) -> int:
    idx = ((crc >> 24) ^ value) & 0xFF
    return (((crc << 8) & 0xFFFFFFFF) ^ _CRC32_TABLE[idx]) & 0xFFFFFFFF


def stm32_crc32(data: bytes) -> int:
    """Exact equivalent of Get-Stm32Crc32 in prepare-m820-bl820.ps1."""
    crc = 0xFFFFFFFF
    offset = 0
    remaining = len(data)
    while remaining >= 4:
        value = int.from_bytes(data[offset:offset + 4], "big")
        crc = _crc32_update_byte(crc, value)
        crc = _crc32_update_byte(crc, value >> 8)
        crc = _crc32_update_byte(crc, value >> 16)
        crc = _crc32_update_byte(crc, value >> 24)
        offset += 4
        remaining -= 4

    if remaining:
        value = 0
        for i in range(remaining):
            value |= data[offset + i] << (24 - 8 * i)
        masks = {1: 0xFF000000, 2: 0xFFFF0000, 3: 0xFFFFFF00}
        value &= masks[remaining]
        crc = _crc32_update_byte(crc, value)
        crc = _crc32_update_byte(crc, value >> 8)
        crc = _crc32_update_byte(crc, value >> 16)
        crc = _crc32_update_byte(crc, value >> 24)
    return crc


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = (((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)) & 0xFFFF
    return crc


def build_container(raw: bytes) -> tuple[bytes, dict[str, int]]:
    crc32 = stm32_crc32(raw)
    payload = raw + struct.pack("<I", crc32)
    size_mod = len(payload) & 0xFFFF
    crc16 = crc16_ccitt_false(payload)
    out = (
        HEADER_PREFIX
        + struct.pack(">H", size_mod)
        + struct.pack(">H", crc16)
        + RESERVED
        + payload
    )
    assert len(HEADER_PREFIX) + 2 + 2 + len(RESERVED) == 32
    return out, {
        "raw_length": len(raw),
        "output_length": len(out),
        "stm32_crc32": crc32,
        "header_size_modulo": size_mod,
        "header_crc16": crc16,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input_bin", type=Path)
    ap.add_argument("output_bin", type=Path)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    raw = args.input_bin.read_bytes()
    out, info = build_container(raw)
    args.output_bin.parent.mkdir(parents=True, exist_ok=True)
    args.output_bin.write_bytes(out)
    if args.json:
        print(json.dumps(info, indent=2))
    else:
        print(f"Input: {args.input_bin}")
        print(f"Output: {args.output_bin}")
        print(f"RawLength: {info['raw_length']}")
        print(f"OutputLength: {info['output_length']}")
        print(f"Stm32Crc32: 0x{info['stm32_crc32']:08X}")
        print(f"HeaderSizeModulo: 0x{info['header_size_modulo']:04X}")
        print(f"HeaderCrc16: 0x{info['header_crc16']:04X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
