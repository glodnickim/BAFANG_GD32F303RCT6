#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./M820_REVERSE_BOOTSTRAP.sh firmware.bin 32 0x08005000 out_dir
#
# IMPORTANT:
# - HEADER_SIZE and APP_BASE must be proven from the vector table first.
# - Do not assume M820 2.1 values for another product/bootloader.

if [ "$#" -ne 4 ]; then
  echo "Usage: $0 <firmware.bin> <header_size_bytes> <app_base_hex> <out_dir>" >&2
  exit 2
fi

FW="$1"
HEADER="$2"
BASE="$3"
OUT="$4"

mkdir -p "$OUT"

NAME="$(basename "$FW")"
APP="$OUT/app.bin"
ASM="$OUT/blob.S"
OBJ="$OUT/blob.o"
ELF="$OUT/app.elf"
DIS="$OUT/app.dis"
META="$OUT/metadata.txt"

{
  echo "source=$FW"
  echo "name=$NAME"
  echo "size=$(stat -c%s "$FW")"
  echo "sha256=$(sha256sum "$FW" | awk '{print $1}')"
  echo "header_size=$HEADER"
  echo "app_base=$BASE"
} > "$META"

echo "== First 128 bytes ==" >> "$META"
od -Ax -tx1 -N128 "$FW" >> "$META"

dd if="$FW" of="$APP" bs=1 skip="$HEADER" status=none

cat > "$ASM" <<ASM
.syntax unified
.thumb
.section .text
.global _app_start
_app_start:
.incbin "app.bin"
ASM

(
  cd "$OUT"
  clang --target=armv7m-none-eabi -c blob.S -o blob.o
  ld.lld -e _app_start -Ttext="$BASE" blob.o -o app.elf
  llvm-objdump -d --triple=thumbv7m-none-eabi app.elf > app.dis
)

echo "Created:"
echo "  $META"
echo "  $APP"
echo "  $ELF"
echo "  $DIS"
