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

# --- 1.5) Size gate: refuse to publish images that don't fit -------------
# v0.7.3 shipped c3/c6 factory-images that exceeded the 0x1C0000 app-slot
# by 8-16 KB. Boot-ROM rejected them with "Image length doesn't fit in
# partition length" -> dead boot-loop on every freshly flashed device.
# Hotfix v0.7.4 enlarged the slot and added this gate so it can't happen
# again silently.
#
# Limits MUST stay in sync with the partition CSVs:
#   partitions-4mb.csv  -> APP_4MB / FS_4MB
#   partitions.csv      -> APP_16MB / FS_16MB
APP_4MB=$((0x1D0000))      # 1.900.544  — app0/app1 in partitions-4mb.csv
FS_4MB=$((0x40000))        #   262.144  — spiffs   in partitions-4mb.csv
APP_16MB=$((0x300000))     # 3.145.728  — app0/app1 in partitions.csv
FS_16MB=$((0x9E0000))      # 10.354.688 — spiffs   in partitions.csv

size_errors=0
check_size() {
  local bin="$1" limit="$2" label="$3"
  if [ ! -f "$bin" ]; then
    echo "[size-gate] MISSING: $label ($bin)" >&2
    size_errors=$((size_errors+1)); return 0
  fi
  local sz; sz=$(stat -c '%s' "$bin")
  if [ "$sz" -gt "$limit" ]; then
    printf "[size-gate] OVER: %-12s %8d > %8d (over by %d B) -> %s\n" \
      "$label" "$sz" "$limit" "$((sz - limit))" "$bin" >&2
    size_errors=$((size_errors+1))
  else
    printf "[size-gate] ok:   %-12s %8d / %8d (%d B headroom)\n" \
      "$label" "$sz" "$limit" "$((limit - sz))"
  fi
}

check_size "$PIO_BUILD/esp32/firmware.bin" $APP_4MB  "esp32 app"
check_size "$PIO_BUILD/esp32/littlefs.bin" $FS_4MB   "esp32 fs"
check_size "$PIO_BUILD/c3/firmware.bin"    $APP_4MB  "c3 app"
check_size "$PIO_BUILD/c3/littlefs.bin"    $FS_4MB   "c3 fs"
check_size "$PIO_BUILD/c6/firmware.bin"    $APP_4MB  "c6 app"
check_size "$PIO_BUILD/c6/littlefs.bin"    $FS_4MB   "c6 fs"
check_size "$PIO_BUILD/s3/firmware.bin"    $APP_16MB "s3 app"
check_size "$PIO_BUILD/s3/littlefs.bin"    $FS_16MB  "s3 fs"

if [ "$size_errors" -gt 0 ]; then
  echo >&2
  echo "ABORT: $size_errors size violation(s) — refusing to publish." >&2
  echo "  Fix partition table (partitions*.csv) or shrink the build."  >&2
  echo "  Do NOT bypass — devices boot-loop and brick on first flash." >&2
  exit 2
fi

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
#   partitions-4mb.csv  ( 4 MB) -> spiffs @ 0x3B0000  (geaendert 2026-05-23 v0.7.4,
#       app-Slots auf 0x1D0000 vergroessert; spiffs auf 256 KB.
#       Wer das mal wieder anpasst: hier mitziehen, sonst landet das
#       LittleFS-Image im falschen Flash-Bereich und der Web-Flasher
#       liefert kaputte Factory-Images aus.)
merge_target esp32 esp32   4MB  0x3B0000  0x1000
merge_target s3    esp32s3 16MB 0x610000  0x0
merge_target c3    esp32c3 4MB  0x3B0000  0x0
merge_target c6    esp32c6 4MB  0x3B0000  0x0

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
