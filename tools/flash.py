#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""OpenBreath installer/flasher for the BIGTREETECH Panda Breath (ESP32-C3).

*** READ THIS FIRST ***
Installing OpenBreath OVERWRITES THE ENTIRE FLASH and ERASES the stock firmware.
BIGTREETECH does not publish stock images, so THE FULL BACKUP THIS TOOL TAKES IS
THE ONLY WAY BACK TO STOCK. If you skip the backup or lose the backup file, THERE
IS NO GOING BACK. Store the backup somewhere safe (copy it off this machine).

Backs up the ENTIRE existing flash to a timestamped file BEFORE writing anything,
then flashes the OpenBreath build. The backup is a full 4 MB image, so you can
return to stock:

    # First-time install (back up stock, then flash OpenBreath):
    python3 tools/flash.py

    # Restore a previous backup (e.g. go back to stock):
    python3 tools/flash.py --restore backups/stock-YYYYmmdd-HHMMSS.bin

The Panda Breath has no exposed native-USB (GPIO18 is the SSR), so flashing is
over the on-board CH340K USB-C UART bridge. Plug the board's USB-C into your PC;
esptool auto-detects the port (override with --port).

Requires esptool (`pip install esptool`, or the copy bundled with ESP-IDF).
Tested against a V1.0.1 board — verify your board revision first.
"""
import argparse
import datetime
import hashlib
import os
import subprocess
import sys

CHIP = "esp32c3"
FLASH_SIZE = 0x400000  # 4 MB
# OpenBreath image layout (matches build/flash_args / partitions.csv).
IMAGES = [
    ("0x0",      "bootloader/bootloader.bin"),
    ("0x8000",   "partition_table/partition-table.bin"),
    ("0xf000",   "ota_data_initial.bin"),
    ("0x20000",  "openbreath.bin"),
]
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def esptool_cmd(port, baud, *args):
    cmd = [sys.executable, "-m", "esptool", "--chip", CHIP]
    if port:
        cmd += ["--port", port]
    cmd += ["--baud", str(baud), *args]
    return cmd


def run(cmd):
    print("  $ " + " ".join(cmd))
    return subprocess.call(cmd)


def check_esptool():
    try:
        subprocess.check_output([sys.executable, "-m", "esptool", "version"],
                                stderr=subprocess.STDOUT)
        return True
    except Exception:
        print("ERROR: esptool not found. Install it with:  pip install esptool")
        print("       (or run this from an ESP-IDF environment: . ~/esp/esp-idf/export.sh)")
        return False


def confirm(prompt):
    try:
        return input(prompt + " [y/N] ").strip().lower() in ("y", "yes")
    except (EOFError, KeyboardInterrupt):
        print()
        return False


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def do_backup(port, baud, backup_dir):
    os.makedirs(backup_dir, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = os.path.join(backup_dir, f"stock-{stamp}.bin")
    print(f"\n[1/3] Backing up the full {FLASH_SIZE // (1024*1024)} MB flash -> {path}")
    print("      (this reads the WHOLE chip; ~1-2 min. Do not unplug.)")
    rc = run(esptool_cmd(port, baud, "read_flash", "0", hex(FLASH_SIZE), path))
    if rc != 0:
        print("ERROR: backup read failed — NOT flashing. Fix the connection and retry.")
        return None

    # 1) exact size, 2) not a blank/failed read (real image starts with the ESP
    # magic byte 0xE9), 3) contents actually match the chip (on-device hash via
    # esptool verify_flash, not just the file size).
    size = os.path.getsize(path) if os.path.exists(path) else 0
    if size != FLASH_SIZE:
        print(f"ERROR: backup is {size} bytes, expected exactly {FLASH_SIZE}. NOT flashing.")
        return None
    with open(path, "rb") as f:
        magic = f.read(1)
    if magic != b"\xe9":
        print(f"ERROR: backup does not start with the ESP image magic (0xE9); got "
              f"0x{magic.hex() or '??'}. The read looks blank/corrupt — NOT flashing.")
        return None
    print("      verifying backup against the chip (hash)...")
    if run(esptool_cmd(port, baud, "verify_flash", "0x0", path)) != 0:
        print("ERROR: backup failed hash verification against the chip — NOT flashing.")
        return None

    digest = sha256_file(path)
    print(f"      backup OK: {size} bytes, verified.")
    print(f"      SHA-256: {digest}")
    print("      Keep this file safe — it is the ONLY way back to stock.")
    return path


def do_flash(port, baud, build_dir):
    args = []
    for offset, rel in IMAGES:
        p = os.path.join(build_dir, rel)
        if not os.path.exists(p):
            print(f"ERROR: missing build artifact {p}")
            print("       Build first:  idf.py set-target esp32c3 && idf.py build")
            return 1
        args += [offset, p]
    app = os.path.join(build_dir, "openbreath.bin")
    print(f"\n[3/3] Flashing OpenBreath from {build_dir}")
    print(f"      app image SHA-256: {sha256_file(app)}")
    return run(esptool_cmd(port, baud,
                           "--before", "default_reset", "--after", "hard_reset",
                           "write_flash", "--flash_mode", "dio",
                           "--flash_size", "4MB", "--flash_freq", "80m", *args))


def do_restore(port, baud, image):
    if not os.path.exists(image):
        print(f"ERROR: backup image not found: {image}")
        return 1
    size = os.path.getsize(image)
    # A full-chip restore must be exactly the flash size — a wrong-size file would
    # write a truncated/misaligned image and brick the board.
    if size != FLASH_SIZE:
        print(f"ERROR: {image} is {size} bytes; a full restore must be exactly "
              f"{FLASH_SIZE} bytes (4 MB). Refusing.")
        return 1
    with open(image, "rb") as f:
        if f.read(1) != b"\xe9":
            print("ERROR: image does not start with the ESP magic (0xE9) — not a "
                  "valid full-flash image. Refusing.")
            return 1
    print(f"\nRestoring full flash image {image}")
    print(f"  size: {size} bytes   SHA-256: {sha256_file(image)}")
    if not confirm("This OVERWRITES the entire chip. Continue?"):
        print("Aborted.")
        return 1
    rc = run(esptool_cmd(port, baud,
                         "--before", "default_reset", "--after", "hard_reset",
                         "write_flash", "--flash_size", "detect", "0x0", image))
    if rc != 0:
        return rc
    print("  verifying restored image against the chip (hash)...")
    if run(esptool_cmd(port, baud, "verify_flash", "0x0", image)) != 0:
        print("WARNING: post-restore verification FAILED — the chip may not match "
              "the image. Re-run the restore.")
        return 1
    print("  restore verified OK.")
    return 0


def main():
    ap = argparse.ArgumentParser(description="OpenBreath flasher (backs up stock first).")
    ap.add_argument("--port", help="serial port (default: esptool auto-detect)")
    ap.add_argument("--baud", type=int, default=460800, help="baud rate (default 460800)")
    ap.add_argument("--build-dir", default=os.path.join(REPO_ROOT, "build"),
                    help="ESP-IDF build dir (default: <repo>/build)")
    ap.add_argument("--backup-dir", default=os.path.join(REPO_ROOT, "backups"),
                    help="where to write the stock backup (default: <repo>/backups)")
    ap.add_argument("--restore", metavar="IMAGE",
                    help="restore a full flash image (e.g. a prior backup) and exit")
    ap.add_argument("--no-backup", action="store_true",
                    help="skip the pre-flash backup (NOT recommended)")
    args = ap.parse_args()

    if not check_esptool():
        return 1

    if args.restore:
        return do_restore(args.port, args.baud, args.restore)

    print("=" * 70)
    print(" OpenBreath flasher")
    print(" !! THIS OVERWRITES THE WHOLE DEVICE AND ERASES THE STOCK FIRMWARE !!")
    print(" There is NO way back to stock without a full backup, and BIGTREETECH")
    print(" does not publish stock images. The backup taken below is your ONLY")
    print(" recovery path — keep it safe and copy it off this machine.")
    print("=" * 70)
    if args.no_backup:
        print("\n*** --no-backup: SKIPPING the stock backup. ***")
        print("*** If you do not ALREADY have a known-good backup stored safely, you")
        print("*** will PERMANENTLY lose the ability to return to stock firmware. ***")
        if not confirm("Really flash WITHOUT taking a backup?"):
            print("Aborted."); return 1
        if not confirm("Are you SURE? This cannot be undone without a backup."):
            print("Aborted."); return 1
    else:
        path = do_backup(args.port, args.baud, args.backup_dir)
        if path is None:
            return 1
        print(f"\n*** IMPORTANT: keep {path} safe — it is the ONLY way back to")
        print("*** stock. Copy it off this machine (cloud/USB) before continuing. ***")

    print("\n[2/3] Ready to flash OpenBreath.")
    if not confirm("Proceed with flashing?"):
        print("Aborted. Your backup (if taken) is kept.")
        return 1

    rc = do_flash(args.port, args.baud, args.build_dir)
    if rc == 0:
        print("\nDone. The board reboots into OpenBreath. On first boot with no saved")
        print("Wi-Fi it starts an 'OpenPanda_XXXX' AP — connect and open http://192.168.4.1")
    else:
        print("\nFlash FAILED. If the board is unresponsive, restore your backup:")
        print("  python3 tools/flash.py --restore <backup.bin>")
    return rc


if __name__ == "__main__":
    sys.exit(main())
