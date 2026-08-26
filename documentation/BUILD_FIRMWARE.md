# OFFICIAL M820_BL820 BUILD SYSTEM

> **STATUS:** CURRENT SOURCE OF TRUTH FOR BUILDING.
>
> Canonical build engine: `scripts/build-firmware.ps1`
>
> Supported firmware variants: **NORMAL**, **DIAGNOSTIC**
>
> Final firmware directory: `.build/M820_BL820/`
>
> Internal BL820 packager: `scripts/prepare-m820-bl820.ps1`
>
> Deprecated compatibility wrapper: `build_firmware.ps1`

Updated: 2026-08-24.

## RULE FOR AI AGENTS / DEVELOPERS

For M820_BL820 do **NOT** search the repository for an alternative build script.

Always use:

```powershell
scripts\build-firmware.ps1
```

Supported firmware variants:

```text
normal
diagnostic
```

Final firmware files are always taken from:

```text
.build/M820_BL820/
```

Do **NOT** flash intermediate/raw binaries from build work directories.

`prepare_m820_bl820.ps1` / `prepare-m820-bl820.ps1` are packaging helpers,
not firmware build entrypoints.

## VERSION NUMBERING

Every canonical build invocation reserves a new sequential build number.

Counter storage: `.local/build-number.txt` (gitignored, persistent).

```text
0.0408 -> 0.0409 -> 0.0410 -> ...
```

Format: `0.` + 4-digit zero-padded counter.

Rules:

- each invocation (NORMAL or DIAG) reserves exactly one number;
- failed builds still consume the number (no rollback);
- `.build` deletion does not affect the counter;
- manual `-Version` override does not touch the counter;
- git describe is provenance only, never the firmware version.

Manual version override (special use only):

```powershell
.\scripts\build-firmware.ps1 -Target M820_BL820 -Profile debug -Variant normal -Version 0.9999
```

When `-Version` is provided explicitly:

- the counter is NOT incremented;
- `version_source` is set to `manual_override` in the manifest;
- `build_counter` is set to -1;
- a warning is displayed.

Do not use manual override for normal workflow.

## 1. Requirements

- Windows PowerShell 5.1 or PowerShell 7;
- Git;
- Arm GNU Toolchain `arm-none-eabi` version exactly `13.2.1`
  (`Arm GNU Toolchain 13.2 Rel1`);
- repository with complete `Firmware/`, `src/`, `inc/`,
  `gcc_startup/` and `ldscripts/` directories.

The script searches for the toolchain in `PATH`, then in:

```text
C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\13.2 Rel1\bin
```

A different location can be passed via `-Toolchain`. A different compiler
version fails the build to prevent accidentally creating different binaries.

## 2. Normal build for riding

From the project root directory:

```powershell
.\scripts\build-firmware.ps1 `
  -Target M820_BL820 `
  -Profile debug `
  -Variant normal
```

The script reserves the next auto-increment number and produces:

```text
.build\M820_BL820\0.0409_M820_BL820.bin
```

`normal` forces `CAN_DIAGNOSTICS_ENABLE=0`. After linking, the script verifies
the ELF does not contain `diag_session_dump_step`. No diagnostics does not
disable the required HMI communication or Canable configuration.

## 3. Diagnostic build

```powershell
.\scripts\build-firmware.ps1 `
  -Target M820_BL820 `
  -Profile debug `
  -Variant diagnostic
```

The script reserves the next auto-increment number and produces:

```text
.build\M820_BL820\0.0410_M820_BL820_DIAG.bin
```

`diagnostic` sets `CAN_DIAGNOSTICS_ENABLE=1`. The script requires the presence
of the `diag_session_dump_step` symbol, so incorrect or stripped diagnostics
settings will not go unnoticed.

## 4. Release profile

The `release` profile exists only for analysis and is blocked by default.
The `-Os` optimization revealed that the current ISR-shared code does not yet
have complete `volatile`/snapshot contracts.

Until AUD-200...AUD-211 are completed:

- use only `-Profile debug` for hardware;
- release artifacts must not be flashed or published;
- `-AllowExperimentalRelease` is for developer analysis only.

## 5. Build results

Final files intended for flashing are in a single directory:

```text
.build/M820_BL820/
```

Example:

```text
.build/M820_BL820/
├── 0.0409_M820_BL820.bin
├── 0.0409_M820_BL820.manifest.json
├── 0.0410_M820_BL820_DIAG.bin
├── 0.0410_M820_BL820_DIAG.manifest.json
└── work/
    ├── normal/
    │   ├── *.o
    │   ├── *.elf
    │   ├── *.map
    │   └── raw.bin
    └── diagnostic/
        ├── *.o
        ├── *.elf
        ├── *.map
        └── raw.bin
```

## 6. Naming convention

### NORMAL

```text
<VERSION>_M820_BL820.bin
```

NORMAL has no variant suffix.

### DIAGNOSTIC

```text
<VERSION>_M820_BL820_DIAG.bin
```

DIAGNOSTIC has suffix `_DIAG` directly before `.bin`.

## 7. Manifest

Each final BIN has a manifest alongside it:

```text
<VERSION>_M820_BL820.manifest.json         (NORMAL)
<VERSION>_M820_BL820_DIAG.manifest.json    (DIAGNOSTIC)
```

Manifest contains:

```text
target, profile, variant, diagnostics_enabled
version, version_source, build_counter
git_commit, git_description, worktree_dirty
toolchain, toolchain_version, linker
source_manifest, source_count
flash (origin, image_end, limit, config_a, text_bytes, binary_bytes)
ram (data_bytes, bss_bytes, used_including_heap_stack_bytes)
artifacts (raw_binary, raw_binary_sha256, final_binary, final_binary_sha256)
```

`version_source` is one of: `auto_increment`, `manual_override`.
`build_counter` is the integer build number (-1 for manual override).

## 8. Memory map contract

```text
0x08005000-0x0803E7FF  application, 230 KiB
0x0803E800-0x0803EFFF  Config A, 2 KiB
0x0803F000-0x0803F7FF  Config B / current record, 2 KiB
0x0803F800-0x0803FFFF  SOC, 2 KiB
```

The linker contains boundary symbols and `ASSERT` statements that stop the
build before the application enters Config A. The script additionally:

- reads addresses from ELF;
- checks image end;
- rejects `RWE` segments;
- confirms correct M820/BL820 target.

## 9. Explicit source list

Compiled files are listed in:

```text
scripts/sources-m820.txt
```

A new `.c` file is not automatically included in firmware. It must be
deliberately added to the source manifest and both debug builds re-run.

## 10. Build summary

After each build the script outputs:

```text
==================================================
eVistDrive M820_BL820 BUILD
==================================================

BUILD VERSION:    0.0409
Version source:   auto_increment
Variant:          NORMAL
Git HEAD:         ba794beb8d89
Git describe:     v0.0286-23-gba794be
Git dirty:        True

Compiler:         13.2.1
Sources:          69 + startup

FLASH:            100140 B (image end: 0x0801D72C, limit: 0x0803E800)
RAM:              12032 B (includes heap + stack)

BUILD VERSION:    0.0409

FINAL FIRMWARE:
  .build\M820_BL820\0.0409_M820_BL820.bin

SHA256:           313F4D3A...

RESULT:           PASS
==================================================
```

## 11. Dirty tree

A dirty tree does not block test builds.

But it must be very visible:

```text
WARNING: WORKING TREE DIRTY
```

## 12. Host tests

Host tests remain a separate workflow:

```text
tests/host/run-host-tests.ps1
```

They are not a firmware variant.

## 13. Deprecated compatibility wrapper

Root `build_firmware.ps1` is a deprecated thin wrapper.

It passes parameters to `scripts/build-firmware.ps1`.

Do not use it in new instructions.

## 14. Known debug warnings

Earlier warnings remain:

- `char *` vs `uint8_t *` in `CAN_Display.c`;
- unused `fw_ver` in `main.c`.

Code warnings should be fixed in the appropriate phase, without mixing them
with infrastructure changes.
