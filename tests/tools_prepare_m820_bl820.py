#!/usr/bin/env python3
"""Independent invariants for the cross-platform BL820 packager."""
from __future__ import annotations
import random
import struct
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from prepare_m820_bl820 import build_container, crc16_ccitt_false, stm32_crc32, POLY


def bit_update(crc: int, byte: int) -> int:
    crc ^= (byte & 0xFF) << 24
    for _ in range(8):
        crc = (((crc << 1) ^ POLY) if (crc & 0x80000000) else (crc << 1)) & 0xFFFFFFFF
    return crc


def slow_reference_crc32(data: bytes) -> int:
    # Independent bitwise implementation of the exact PowerShell word feeding order.
    crc = 0xFFFFFFFF
    off = 0
    while off + 4 <= len(data):
        word = data[off:off+4]
        for b in reversed(word):
            crc = bit_update(crc, b)
        off += 4
    rem = data[off:]
    if rem:
        padded = rem + bytes(4 - len(rem))
        for b in reversed(padded):
            crc = bit_update(crc, b)
    return crc


def main() -> int:
    rng = random.Random(0x820141)
    for n in list(range(0, 20)) + [31, 32, 33, 255, 256, 257, 4097]:
        raw = bytes(rng.randrange(256) for _ in range(n))
        a = stm32_crc32(raw)
        b = slow_reference_crc32(raw)
        if a != b:
            raise SystemExit(f"CRC32 mismatch length={n}: {a:08x} != {b:08x}")
        out, info = build_container(raw)
        if len(out) != len(raw) + 36:
            raise SystemExit(f"container length mismatch {n}")
        if out[:14] != bytes.fromhex("0145824040000000000000000000"):
            raise SystemExit("header prefix mismatch")
        if int.from_bytes(out[14:16], "big") != ((len(raw) + 4) & 0xFFFF):
            raise SystemExit("size modulo mismatch")
        payload = out[32:]
        if payload[:-4] != raw:
            raise SystemExit("payload mismatch")
        if struct.unpack("<I", payload[-4:])[0] != a:
            raise SystemExit("trailing CRC32 endian mismatch")
        if int.from_bytes(out[16:18], "big") != crc16_ccitt_false(payload):
            raise SystemExit("header CRC16 mismatch")
    print("BL820 Python packager: PASS (independent CRC + container invariants)")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
