#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""OpenBreath installer/flasher for the BIGTREETECH Panda Breath (ESP32-C3).

Backs up the ENTIRE existing flash to a timestamped file BEFORE writing anything,
then flashes the OpenBreath build. The backup is a full 4 MB image, so you can
always return to stock:

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
    size = os.path.getsize(path) if os.path.exists(path) else 0
    if size != FLASH_SIZE:
        print(f"ERROR: backup is {size} bytes, expected {FLASH_SIZE}. NOT flashing.")
        return None
    print(f"      backup OK ({size} bytes). Keep this file safe — it restores stock.")
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
    print(f"\n[3/3] Flashing OpenBreath from {build_dir}")
    return run(esptool_cmd(port, baud,
                           "--before", "default_reset", "--after", "hard_reset",
                           "write_flash", "--flash_mode", "dio",
                           "--flash_size", "4MB", "--flash_freq", "80m", *args))


def do_restore(port, baud, image):
    if not os.path.exists(image):
        print(f"ERROR: backup image not found: {image}")
        return 1
    size = os.path.getsize(image)
    print(f"\nRestoring full flash image {image} ({size} bytes) at offset 0x0")
    if not confirm("This OVERWRITES the entire chip. Continue?"):
        print("Aborted.")
        return 1
    return run(esptool_cmd(port, baud,
                           "--before", "default_reset", "--after", "hard_reset",
                           "write_flash", "--flash_size", "detect", "0x0", image))


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

    print("OpenBreath flasher — this will replace the stock firmware.")
    print("A full stock backup is taken first so you can always restore it.")
    if args.no_backup:
        print("\n*** --no-backup: skipping the stock backup. You will NOT be able to")
        print("*** return to stock unless you already have a backup image. ***")
        if not confirm("Really flash without a backup?"):
            print("Aborted."); return 1
    else:
        path = do_backup(args.port, args.baud, args.backup_dir)
        if path is None:
            return 1

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
