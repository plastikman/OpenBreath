#!/usr/bin/env bash
# package_release.sh — assemble reproducible DragonBreath release artifacts from a
# completed ESP-IDF build (Phase 0.2: release correctness).
#
# Produces, in $OUT (default ./dist):
#   dragonbreath-<VER>.bin              — application image (for web OTA / /update)
#   dragonbreath-<VER>-factory.bin      — single first-install image, flash @ 0x0
#   dragonbreath-<VER>-install-bundle.zip — complete first-install bundle
#       (bootloader, partition table, ota_data_initial, app, factory, flasher,
#        dependencies.lock, manifest.json, SHA256SUMS.txt, FLASHING.txt)
#   manifest.json                       — provenance + per-artifact SHA-256
#   SHA256SUMS.txt                      — checksums of the published assets
#
# Requires: python3, zip, git, sha256sum, and esptool (esptool.py or `python3 -m
# esptool`). Run from the repo root after `idf.py build`.
#
#   VERSION=v0.2.0 ./tools/package_release.sh
set -euo pipefail

VERSION="${VERSION:?set VERSION (e.g. v0.2.0)}"
BUILD_DIR="${BUILD_DIR:-build}"
OUT="${OUT:-dist}"
SOURCE_SHA="${SOURCE_SHA:-$(git rev-parse HEAD)}"
TARGET="${TARGET:-esp32c3}"
BOARD="${BOARD:-BIGTREETECH Panda Breath (ESP32-C3), V1.0.1}"
# IDF version: prefer an explicit env, else read the project's dependency lock.
IDF_VERSION="${IDF_VERSION:-$(sed -n '/^  idf:/,/version:/s/^ *version: *//p' dependencies.lock 2>/dev/null | tail -1)}"
IDF_VERSION="${IDF_VERSION:-unknown}"
BUILT_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

esptool() { if command -v esptool.py >/dev/null; then esptool.py "$@"; else python3 -m esptool "$@"; fi; }

for f in dragonbreath.bin bootloader/bootloader.bin partition_table/partition-table.bin ota_data_initial.bin flash_args; do
    [ -f "$BUILD_DIR/$f" ] || { echo "missing $BUILD_DIR/$f — run idf.py build first" >&2; exit 1; }
done

rm -rf "$OUT" bundle-stage
mkdir -p "$OUT" bundle-stage

APP="dragonbreath-${VERSION}.bin"
FACTORY="dragonbreath-${VERSION}-factory.bin"
BUNDLE="dragonbreath-${VERSION}-install-bundle.zip"

# 1. OTA application image (published separately for web OTA)
cp "$BUILD_DIR/dragonbreath.bin" "$OUT/$APP"

# 2. single-file first-install image (bootloader+ptable+otadata+app), flash @ 0x0
( cd "$BUILD_DIR" && esptool --chip "$TARGET" merge_bin -o "$OLDPWD/$OUT/$FACTORY" @flash_args >/dev/null )

# 3. stage the complete bundle — preserve the nested build/ layout so the bundled
#    flasher (tools/flash.py --build-dir .) works directly AND takes the stock backup.
mkdir -p bundle-stage/bootloader bundle-stage/partition_table
cp "$BUILD_DIR/bootloader/bootloader.bin"           bundle-stage/bootloader/
cp "$BUILD_DIR/partition_table/partition-table.bin" bundle-stage/partition_table/
cp "$BUILD_DIR/ota_data_initial.bin" "$BUILD_DIR/dragonbreath.bin" "$OUT/$FACTORY" bundle-stage/
cp tools/flash.py bundle-stage/ 2>/dev/null || true
cp dependencies.lock bundle-stage/ 2>/dev/null || true

cat > bundle-stage/FLASHING.txt <<EOF
DragonBreath ${VERSION} — flashing (${TARGET})

⚠️  Installing OVERWRITES the entire flash and ERASES the stock firmware. There is
    NO published stock image — the backup the flasher makes is the ONLY way back.

RECOMMENDED first install (backs up stock, then flashes) — from this bundle:
  python3 flash.py --build-dir .

ADVANCED single-image install — ONLY if you already have a stock backup
(this path does NOT back anything up):
  esptool.py --chip ${TARGET} write_flash 0x0 ${FACTORY}

Over-the-air update (device already on DragonBreath): upload dragonbreath-${VERSION}.bin
via the device web UI (/fw) — the app image only; never restores stock.

Verify downloads first:  sha256sum -c SHA256SUMS.txt
EOF

# 4. provenance manifest (per-artifact SHA-256)
export VERSION SOURCE_SHA IDF_VERSION TARGET BOARD BUILT_AT
python3 - "$OUT/$APP" "$OUT/$FACTORY" \
    "$BUILD_DIR/bootloader/bootloader.bin" \
    "$BUILD_DIR/partition_table/partition-table.bin" \
    "$BUILD_DIR/ota_data_initial.bin" <<PY > "$OUT/manifest.json"
import hashlib, json, os, sys
def h(p):
    with open(p,'rb') as f: return hashlib.sha256(f.read()).hexdigest()
arts=[{"name":os.path.basename(p),"size":os.path.getsize(p),"sha256":h(p)} for p in sys.argv[1:]]
print(json.dumps({
    "product":"dragonbreath","version":os.environ["VERSION"],
    "source_sha":os.environ["SOURCE_SHA"],
    "idf_version":os.environ["IDF_VERSION"],
    "target":os.environ["TARGET"],"board":os.environ["BOARD"],
    "vendored_core":{"source":"github.com/justinh-rahb/OpenVent","ref":"ec4691f (v0.3.0-4)","license":"MIT","components":["pb_evlog","pb_wifi","pb_moonraker"]},
    "built_at_utc":os.environ["BUILT_AT"],
    "note":"managed component versions in dependencies.lock (bundled)",
    "artifacts":arts,
}, indent=2))
PY
cp "$OUT/manifest.json" bundle-stage/

# 5. zip the complete first-install bundle (stdlib zipfile — no external `zip` dep)
( cd bundle-stage && python3 -m zipfile -c "$OLDPWD/$OUT/$BUNDLE" * )
rm -rf bundle-stage

# 6. top-level checksums for the published assets
( cd "$OUT" && sha256sum "$APP" "$FACTORY" "$BUNDLE" manifest.json > SHA256SUMS.txt )

echo ">> release artifacts in $OUT/:"
( cd "$OUT" && ls -la && echo && cat SHA256SUMS.txt )
