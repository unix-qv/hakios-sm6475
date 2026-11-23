#!/usr/bin/env bash
# Auto-detect fastboot and boot iOS (HakiOS) on POCO M8 5G

set -euo pipefail

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
if [ -d "$SCRIPT_DIR/hakios" ]; then
    HAKIOS_DIR="$SCRIPT_DIR/hakios"
else
    HAKIOS_DIR="$SCRIPT_DIR"
fi

cd "$HAKIOS_DIR"

echo "===================================================="
echo "         HakiOS Auto-Boot for POCO M8 5G"
echo "===================================================="
echo ""

# Check if phone is booted into Android, and reboot to bootloader automatically
if adb devices 2>/dev/null | grep -E '\bdevice\b' >/dev/null; then
    echo "[*] Phone detected in Android via ADB. Rebooting to fastboot bootloader..."
    adb reboot bootloader || true
    sleep 3
fi

echo "[*] Waiting for device in fastboot mode..."

while true; do
    if fastboot devices 2>/dev/null | grep -q "fastboot"; then
        echo "[+] Device detected in fastboot!"
        break
    fi
    sleep 1
done

echo "[*] Entering loader..."
./xl enter

echo "[*] Ensuring resident APFS system image is loaded into RAM..."
./xl ramdisk userland/system.apfs

echo "[*] Launching HakiOS iOS desktop..."
DEVICE=parrot XL_CMDLINE="" XL_SECS=3600 ./xl run
