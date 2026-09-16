#!/bin/sh
set -eu

usage() {
    printf '%s\n' 'usage: tools/setup.sh [--flash /dev/...]' >&2
    exit 2
}

case $# in
    0) flash_port= ;;
    2) [ "$1" = --flash ] || usage; flash_port=$2; case $flash_port in /dev/*) [ -n "$flash_port" ] || usage ;; *) usage ;; esac ;;
    *) usage ;;
esac

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo"
objdump="$HOME/.platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-objdump"
for command in python3 pio ar c++; do
    command -v "$command" >/dev/null 2>&1 || { printf 'prerequisite missing: %s\n' "$command" >&2; exit 1; }
done
if [ ! -x "$objdump" ]; then
    objdump=$(command -v xtensa-esp32-elf-objdump || true)
fi
[ -n "$objdump" ] && [ -x "$objdump" ] || { printf '%s\n' 'prerequisite missing: usable xtensa-esp32-elf-objdump' >&2; exit 1; }

pio run -e rfclown_alt
python3 tools/patch_libnet80211.py --library "$HOME/.platformio/packages/framework-arduinoespressif32-libs/esp32/lib/libnet80211.a" --objdump "$objdump"
pio run -e rfclown_alt
c++ -std=c++17 -Wall -Wextra -Isrc -DWIFI_DEAUTH_ENABLE=1 -DJAM_ENABLE=1 test/test_host.cpp -o /tmp/rfclown_alt_host_tests
/tmp/rfclown_alt_host_tests

if [ -n "$flash_port" ]; then
    printf 'Flashing selected port: %s\n' "$flash_port"
    pio run -e rfclown_alt --target upload --upload-port "$flash_port"
fi
