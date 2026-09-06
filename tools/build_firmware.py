#!/usr/bin/env python3
"""Cross-platform verified M820_BL820 developer/repro build.

The compile/link flags and memory gates mirror scripts/build-firmware.ps1, but the path no longer
requires PowerShell. Canonical release-number allocation intentionally remains outside this tool;
this builder is for deterministic Developer (DEV-NONCANONICAL) and explicit Repro identities.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPECTED_GCC = "13.2.1"
FLASH_ORIGIN = 0x08005000
CONFIG_A_ORIGIN = 0x0803E800
RAM_ORIGIN = 0x20000000

sys.path.insert(0, str(ROOT / "tools"))
from prepare_m820_bl820 import build_container


def run(cmd: list[str], *, capture=False) -> str:
    p = subprocess.run(cmd, cwd=ROOT, text=True,
                       stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.STDOUT if capture else None)
    if p.returncode:
        if capture and p.stdout:
            print(p.stdout, end="")
        raise SystemExit(f"command failed ({p.returncode}): {' '.join(cmd)}")
    return p.stdout if capture else ""


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git_value(*args: str) -> str:
    if not (ROOT / ".git").exists():
        return "NO-GIT"
    return run(["git", "-c", f"safe.directory={ROOT}", "-c", "core.excludesFile=",
                "-C", str(ROOT), *args], capture=True).strip()


def source_entries() -> list[str]:
    out: list[str] = []
    for line in (ROOT / "scripts" / "sources-m820.txt").read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            if not (ROOT / line).is_file():
                raise SystemExit(f"source manifest entry missing: {line}")
            out.append(line)
    if not out:
        raise SystemExit("source manifest is empty")
    return out


def find_tool(name: str, toolchain: str | None) -> str:
    suffix = ".exe" if os.name == "nt" else ""
    exe = name + suffix
    if toolchain:
        p = Path(toolchain)
        if p.is_file() and p.name.lower().startswith(name.lower()):
            return str(p.resolve())
        q = p / exe
        if q.is_file():
            return str(q.resolve())
        # Be tolerant when a Windows toolchain path is passed from a POSIX-like shell.
        q2 = p / name
        if q2.is_file():
            return str(q2.resolve())
        raise SystemExit(f"tool not found under --toolchain: {exe}")
    found = shutil.which(exe) or shutil.which(name)
    if not found and os.name == "nt":
        # Same canonical location used by the original PowerShell build. This keeps the Python
        # gate usable on a normal Windows install even when the Arm bin directory is not in PATH.
        default_bin = Path(r"C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\13.2 Rel1\bin")
        candidate = default_bin / exe
        if candidate.is_file():
            found = str(candidate)
    if not found:
        raise SystemExit(f"{name} not found; install Arm GNU Toolchain {EXPECTED_GCC} or pass --toolchain BIN_DIR")
    return found


def symbol_map(nm_text: str) -> dict[str, int]:
    d: dict[str, int] = {}
    for line in nm_text.splitlines():
        m = re.match(r"^\s*([0-9A-Fa-f]+)\s+\w\s+(\S+)\s*$", line)
        if m:
            d[m.group(2)] = int(m.group(1), 16)
    return d


def require_symbol(symbols: dict[str, int], name: str) -> int:
    if name not in symbols:
        raise SystemExit(f"required linker symbol not found: {name}")
    return symbols[name]


def check_tree() -> list[str]:
    entries = source_entries()
    required = [
        "gcc_startup/startup_gd32f30x_hd.S",
        "ldscripts/gd32f30x_flash.ld",
        "Firmware/CMSIS/arm_math.h",
        "Firmware/CMSIS/libarm_cortexM4lf_math.a",
        "Firmware/CMSIS/GD/GD32F30x/Include/gd32f30x.h",
    ]
    missing = [p for p in required if not (ROOT / p).is_file()]
    if missing:
        raise SystemExit("build tree incomplete: " + ", ".join(missing))
    print(f"BUILD TREE: PASS ({len(entries)} manifest sources + startup/linker/CMSIS/HAL)")
    return entries


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--toolchain", help="Arm GNU toolchain bin directory or arm-none-eabi-gcc path")
    ap.add_argument("--variant", choices=["normal", "diagnostic"], default="normal")
    ap.add_argument("--mode", choices=["developer", "repro"], default="developer")
    ap.add_argument("--version", default="", help="required for --mode repro")
    ap.add_argument("--output-dir", default=".build/python-target")
    ap.add_argument("--check-only", action="store_true", help="validate complete build inputs without requiring compiler")
    args = ap.parse_args()

    entries = check_tree()
    if args.check_only:
        # Exercise the portable container code on a nontrivial deterministic fixture too.
        fixture = bytes((i * 37 + 11) & 0xFF for i in range(4097))
        packed, info = build_container(fixture)
        if len(packed) != len(fixture) + 36 or info["raw_length"] != len(fixture):
            raise SystemExit("BL820 packager check failed")
        print("BL820 PACKAGER: PASS")
        return 0

    version = "DEV-NONCANONICAL" if args.mode == "developer" else args.version
    if args.mode == "repro" and not version:
        raise SystemExit("--mode repro requires --version")
    if not re.match(r"^[0-9A-Za-z][0-9A-Za-z._+-]{0,47}$", version):
        raise SystemExit("unsafe version string")

    gcc = find_tool("arm-none-eabi-gcc", args.toolchain)
    toolbin = str(Path(gcc).resolve().parent)
    objcopy = find_tool("arm-none-eabi-objcopy", toolbin)
    size = find_tool("arm-none-eabi-size", toolbin)
    nm = find_tool("arm-none-eabi-nm", toolbin)
    readelf = find_tool("arm-none-eabi-readelf", toolbin)
    gcc_version = run([gcc, "-dumpfullversion", "-dumpversion"], capture=True).strip().splitlines()[0]
    if gcc_version != EXPECTED_GCC:
        raise SystemExit(f"unsupported Arm GNU Toolchain {gcc_version}; expected {EXPECTED_GCC}")

    outroot = Path(args.output_dir)
    if not outroot.is_absolute():
        outroot = ROOT / outroot
    userdir = outroot / "M820_BL820"
    work = userdir / "work" / args.variant
    gen = work / "generated"
    objdir = work / "objects"
    gen.mkdir(parents=True, exist_ok=True)
    objdir.mkdir(parents=True, exist_ok=True)
    (gen / "build_version.h").write_text(
        '#ifndef BUILD_VERSION_H\n#define BUILD_VERSION_H\n'
        '/* Generated in the build directory; never edit or commit. */\n'
        f'#define EBICS_BUILD_VERSION "{version}"\n#endif\n', encoding="ascii")

    defs = ["-DGD32F30X_HD", "-DGD_ECLIPSE_GCC", "-DUSE_STDPERIPH_DRIVER", "-DBOOTLOADER=820",
            f"-DCAN_DIAGNOSTICS_ENABLE={1 if args.variant == 'diagnostic' else 0}"]
    common = ["-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv4-sp-d16"]
    inc = [f"-I{gen}", f"-I{ROOT/'inc'}", f"-I{ROOT/'Firmware/CMSIS'}",
           f"-I{ROOT/'Firmware/CMSIS/GD/GD32F30x/Include'}",
           f"-I{ROOT/'Firmware/GD32F30x_standard_peripheral/Include'}"]
    cflags = common + ["-O0", "-g3", "-fmessage-length=0", "-fsigned-char",
                       "-ffunction-sections", "-fdata-sections", "-Wall"] + defs + inc

    objects: list[str] = []
    for entry in entries:
        obj = objdir / (re.sub(r"[\\/:]", "_", entry) + ".o")
        run([gcc, *cflags, "-c", str(ROOT / entry), "-o", str(obj)])
        objects.append(str(obj))
    startup_obj = objdir / "startup_gd32f30x_hd.S.o"
    run([gcc, *common, *defs, *inc, "-x", "assembler-with-cpp", "-c",
         str(ROOT / "gcc_startup/startup_gd32f30x_hd.S"), "-o", str(startup_obj)])
    objects.append(str(startup_obj))

    base = version
    elf, rawbin, hexp = work/base, work/f"{base}.bin", work/f"{base}.hex"
    elf = elf.with_suffix(".elf")
    mapf = work/f"{base}.map"
    suffix = "_DIAG" if args.variant == "diagnostic" else ""
    final = userdir/f"{base}_M820_BL820{suffix}.bin"
    manifest = userdir/f"{base}_M820_BL820{suffix}.manifest.json"
    linker = ROOT / "ldscripts/gd32f30x_flash.ld"
    link = [gcc, *common, f"-T{linker}", "-Wl,--gc-sections", "-Wl,--print-memory-usage",
            f"-Wl,-Map,{mapf}", "-Wl,--start-group", *objects,
            f"-L{ROOT/'Firmware/CMSIS'}", "-larm_cortexM4lf_math", "-specs=nano.specs", "-specs=nosys.specs",
            "-Wl,--end-group", "-o", str(elf)]
    run(link)
    run([objcopy, "-O", "binary", str(elf), str(rawbin)])
    run([objcopy, "-O", "ihex", str(elf), str(hexp)])

    size_out = run([size, str(elf)], capture=True)
    size_a = run([size, "-A", str(elf)], capture=True)
    (work/f"{base}.size.txt").write_text(size_out + "\n" + size_a, encoding="ascii")
    ph = run([readelf, "-l", str(elf)], capture=True)
    (work/f"{base}.program-headers.txt").write_text(ph, encoding="ascii")
    if re.search(r"\bRWE\b", ph):
        raise SystemExit("ELF contains an RWE load segment")
    nm_text = run([nm, "--defined-only", str(elf)], capture=True)
    symbols = symbol_map(nm_text)
    app_start = require_symbol(symbols, "__app_flash_start")
    app_limit = require_symbol(symbols, "__app_flash_limit")
    image_end = require_symbol(symbols, "__flash_image_end")
    config_a = require_symbol(symbols, "__config_a_start")
    if app_start != FLASH_ORIGIN or config_a != CONFIG_A_ORIGIN or image_end > app_limit or app_limit > config_a:
        raise SystemExit("linked memory map does not match M820_BL820 target")
    diag_symbol = "diag_session_dump_step" in symbols
    if diag_symbol != (args.variant == "diagnostic"):
        raise SystemExit("diagnostic link-marker mismatch")

    cols = size_out.strip().splitlines()[-1].split()
    if len(cols) < 4:
        raise SystemExit("cannot parse arm-none-eabi-size output")
    text_b, data_b, bss_b = map(int, cols[:3])
    sdata, edata = require_symbol(symbols, "_sdata"), require_symbol(symbols, "_edata")
    sbss, ebss = require_symbol(symbols, "_sbss"), require_symbol(symbols, "_ebss")
    sp = require_symbol(symbols, "_sp")

    packed, packinfo = build_container(rawbin.read_bytes())
    final.parent.mkdir(parents=True, exist_ok=True)
    final.write_bytes(packed)
    if version.encode("ascii") not in final.read_bytes():
        raise SystemExit("firmware identity string missing from final BL820 artifact")

    commit = git_value("rev-parse", "HEAD")
    describe = git_value("describe", "--tags", "--always")
    dirty = False
    if (ROOT / ".git").exists():
        dirty = bool(git_value("status", "--porcelain", "--untracked-files=normal"))
    doc = {
        "schema_version": 1, "target": "M820_BL820", "profile": "debug", "variant": args.variant,
        "diagnostics_enabled": args.variant == "diagnostic", "version": version,
        "version_source": "developer_noncanonical" if args.mode == "developer" else "repro_explicit",
        "git_commit": commit, "git_description": describe, "worktree_dirty": dirty,
        "hardware_approved_profile": True,
        "toolchain": "Arm GNU Toolchain arm-none-eabi", "toolchain_version": gcc_version,
        "linker": "ldscripts/gd32f30x_flash.ld", "source_manifest": "scripts/sources-m820.txt",
        "source_count": len(entries),
        "flash": {"origin": f"0x{app_start:08X}", "image_end": f"0x{image_end:08X}",
                  "limit": f"0x{app_limit:08X}", "config_a": f"0x{config_a:08X}",
                  "text_bytes": text_b, "gnu_size_data_bytes": data_b,
                  "data_load_bytes": edata-sdata, "binary_bytes": rawbin.stat().st_size},
        "ram": {"data_bytes": edata-sdata, "bss_bytes": ebss-sbss,
                "heap_stack_reserved_bytes": sp-ebss,
                "used_including_heap_stack_bytes": sp-RAM_ORIGIN,
                "gnu_size_bss_bytes": bss_b},
        "packaging": {k: (f"0x{v:08X}" if k == "stm32_crc32" else f"0x{v:04X}" if k in ("header_size_modulo","header_crc16") else v)
                      for k,v in packinfo.items()},
        "artifacts": {"elf": str(elf), "map": str(mapf), "raw_binary": str(rawbin),
                      "raw_binary_sha256": sha256(rawbin), "final_binary": str(final),
                      "final_binary_bytes": final.stat().st_size, "final_binary_sha256": sha256(final)}
    }
    manifest.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print("\n==================================================")
    print("eVistDrive M820_BL820 CROSS-PLATFORM BUILD: PASS")
    print(f"Version:   {version}")
    print(f"Compiler:  {gcc_version}")
    print(f"Sources:   {len(entries)} + startup")
    print(f"Raw BIN:   {rawbin.stat().st_size} B")
    print(f"Final BIN: {final}")
    print(f"SHA256:    {sha256(final)}")
    print("==================================================")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
