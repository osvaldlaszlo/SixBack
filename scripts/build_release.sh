#!/usr/bin/env bash
# SixBack — build release artefacts for ESP-Web-Tools distribution.
#
# For each target (s3, c3, c6) it produces:
#   webflasher/sixback-<tgt>-factory.bin   — merged bootloader+parts+app+fs
#   webflasher/sixback-<tgt>-firmware.bin  — app-only (for OTA over WiFi)
#   webflasher/sixback-<tgt>-littlefs.bin  — Web-UI image (for FS-OTA)
# Plus:
#   webflasher/manifest.json                 — esp-web-tools manifest with VERSION
#
# Run from project root.  Requires PlatformIO env active.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PIO_BUILD="/root/pio-build/bosefix32"   # Build-Dir keep (path on disk, not user-visible)
OUT="$ROOT/webflasher"
mkdir -p "$OUT"

# --- 1) Compile all envs + their LittleFS images --------------------------
# Pio's pre-build-hook (version_bump.py) bumpt build_number bei JEDEM
# `pio run`-Aufruf — buildfs *und* firmware-build je einmal. Damit am Ende
# manifest.json und firmware.bin DIESELBE Version tragen, muss der zuletzt
# laufende Bump derjenige sein, der die firmware.bin produziert. Also:
# buildfs ZUERST, firmware DANACH. version.h-Stand nach beiden Schritten
# == FW_VERSION_STRING im aktuellen firmware.bin == manifest.json.
#
# Frueherer Bug (2026-05-19): umgekehrte Reihenfolge → manifest.json war
# um +1 (oder mehr bei wiederholten Builds) hoeher als die firmware.bin
# tatsaechlich war. C6 OTA-update zeigte "auf 0.5.422 aktualisiert" aber
# nach dem Reboot lief 0.5.418.
"$HOME/.platformio/penv/bin/pio" run -e esp32 -e s3 -e c3 -e c6 -t buildfs
"$HOME/.platformio/penv/bin/pio" run -e esp32 -e s3 -e c3 -e c6

# Resolve final version after both bumps (single source of truth in version.h).
VERSION="$(grep -oE '"[0-9]+\.[0-9]+\.[0-9]+"' "$ROOT/src/version.h" | tr -d '"')"
echo ">>> SixBack release build, version=$VERSION"

# --- 2) Per-target merge into single factory image -----------------------
ESPTOOL=( "$HOME/.platformio/penv/bin/pio" pkg exec --package "platformio/tool-esptoolpy" -- python -m esptool )

merge_target() {
  local tgt="$1" chip="$2" fsize="$3" spiffs_off="$4" boot_off="$5"
  local src="$PIO_BUILD/$tgt"
  local factory="$OUT/sixback-$tgt-factory.bin"

  echo ">>> Merging $tgt ($chip, $fsize, boot@$boot_off, spiffs@$spiffs_off)"
  "${ESPTOOL[@]}" --chip "$chip" merge_bin \
    -o "$factory" \
    --flash_mode dio --flash_size "$fsize" --flash_freq 80m \
    "$boot_off"   "$src/bootloader.bin" \
    0x8000        "$src/partitions.bin" \
    0x10000       "$src/firmware.bin" \
    "$spiffs_off" "$src/littlefs.bin"

  cp "$src/firmware.bin"  "$OUT/sixback-$tgt-firmware.bin"
  cp "$src/littlefs.bin"  "$OUT/sixback-$tgt-littlefs.bin"
}

# bootloader offset:
#   ESP32 (classic): 0x1000  (Boot-ROM springt dorthin)
#   S3 / C3 / C6 / S2 / C2 / C5 / C61 / H2 / P4: 0x0
# spiffs offsets must match the corresponding partition table:
#   partitions.csv      (16 MB) -> spiffs @ 0x610000
#   partitions-4mb.csv  ( 4 MB) -> spiffs @ 0x390000  (geaendert 2026-05-19,
#       app-Slots von 0x1B0000 -> 0x1C0000 vergroessert; spiffs auf 384 KB
#       verkleinert. Wer das mal wieder anpasst: hier mitziehen, sonst
#       landet das LittleFS-Image im falschen Flash-Bereich und der
#       Web-Flasher liefert kaputte Factory-Images aus.)
merge_target esp32 esp32   4MB  0x390000  0x1000
merge_target s3    esp32s3 16MB 0x610000  0x0
merge_target c3    esp32c3 4MB  0x390000  0x0
merge_target c6    esp32c6 4MB  0x390000  0x0

# --- 3) Generate esp-web-tools manifest with current version -------------
cat > "$OUT/manifest.json" <<EOF
{
  "name": "SixBack",
  "version": "$VERSION",
  "funding_url": "https://polyformproject.org/licenses/noncommercial/1.0.0/",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "sixback-esp32-factory.bin", "offset": 0 }
      ]
    },
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "sixback-s3-factory.bin", "offset": 0 }
      ]
    },
    {
      "chipFamily": "ESP32-C3",
      "parts": [
        { "path": "sixback-c3-factory.bin", "offset": 0 }
      ]
    },
    {
      "chipFamily": "ESP32-C6",
      "parts": [
        { "path": "sixback-c6-factory.bin", "offset": 0 }
      ]
    }
  ]
}
EOF

# --- 4) Summary -----------------------------------------------------------
echo
echo "=== Release artefacts (version $VERSION) ==="
ls -lh "$OUT"/*.bin "$OUT"/manifest.json
echo
echo "Public landing page:  https://install.busware.de/sixback/"
echo "Deploy command (user triggers manually):"
echo "  rsync -avr webflasher/ 10.10.22.1:/var/www/install/sixback/"
