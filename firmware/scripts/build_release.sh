#!/bin/sh
# Builds all three firmwares and writes single-file images (bootloader + partitions + app, offset 0)
# for the web flasher into flasher/firmware/. Needs PlatformIO.
set -eu
cd "$(dirname "$0")/.."
pio run -e loupe_side -e home_side_esp32 -e home_side_esp8266
out=../flasher/firmware
mkdir -p "$out"
ESPTOOL="python3 $(find "${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/tool-esptoolpy" -name esptool.py | head -1)"
BOOT_APP0=$(find "${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-arduinoespressif32" -name boot_app0.bin | head -1)
for env in loupe_side home_side_esp32; do
  b=.pio/build/$env
  $ESPTOOL --chip esp32 merge_bin -o "$out/${env%_esp32}_esp32.bin" --flash_mode dio --flash_size 4MB \
    0x1000 "$b/bootloader.bin" 0x8000 "$b/partitions.bin" 0xe000 "$BOOT_APP0" 0x10000 "$b/firmware.bin"
done
cp .pio/build/home_side_esp8266/firmware.bin "$out/home_side_esp8266.bin"
ls -l "$out"
