#!/bin/bash
set -e
cd "$(dirname "$0")/../.."
adb get-state >/dev/null
adb shell "su -c '
S=/data/local/tmp/fw
rm -rf \$S
mkdir -p \$S/gpu \$S/wifi \$S/audio \$S/haptic \$S/modem
cp /vendor/firmware/gmu_gen70000.bin /vendor/firmware/a710_sqe.fw /vendor/firmware/a710_zap.* \$S/gpu/ 2>/dev/null || true
cp /vendor/firmware_mnt/image/wpss.* \$S/wifi/ 2>/dev/null || true
cp /vendor/firmware_mnt/image/adsp.* \$S/audio/ 2>/dev/null || true
cp /vendor/firmware/*haptic* \$S/haptic/ 2>/dev/null || true
cp /vendor/firmware_mnt/image/modem.* \$S/modem/ 2>/dev/null || true
chmod -R 777 \$S
'"
rm -rf fw-stage
adb pull /data/local/tmp/fw fw-stage >/dev/null
rm -rf fw-gpu fw-wifi fw-audio fw-haptic fw-modem
mv fw-stage/gpu fw-gpu; mv fw-stage/wifi fw-wifi; mv fw-stage/audio fw-audio
mv fw-stage/haptic fw-haptic; mv fw-stage/modem fw-modem; rmdir fw-stage 2>/dev/null || true
adb shell "su -c 'rm -rf /data/local/tmp/fw'"
for d in fw-gpu fw-wifi fw-audio fw-haptic fw-modem; do
	echo "pull-firmware.sh: $d: $(ls -1 "$d" 2>/dev/null | wc -l | tr -d ' ') files"
done
