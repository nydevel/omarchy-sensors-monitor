#!/bin/bash
# Tests for the C++ helper: tests/test_helper.sh
# Builds it, runs its self-test, then checks it against a fake sysfs tree and
# this machine's real sensors.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

bin="$work/sensors"
c++ -std=c++17 -O2 -Wall -Wextra -Werror -o "$bin" "$here/sensors.cpp"
"$bin" --self-test >/dev/null || fail "self-test"

# A fake hwmon tree with a processor, a drive past its limit, a board chip
# and two memory modules.
hw="$work/hwmon"
chip() { mkdir -p "$hw/$1"; echo "$2" >"$hw/$1/name"; }
chip hwmon0 k10temp
echo 61125 >"$hw/hwmon0/temp1_input"; echo Tctl >"$hw/hwmon0/temp1_label"
chip hwmon1 nvme
mkdir -p "$work/dev/nvme0"; echo "Fake SSD 1TB   " >"$work/dev/nvme0/model"; ln -s "$work/dev/nvme0" "$hw/hwmon1/device"
echo 86850 >"$hw/hwmon1/temp1_input"; echo Composite >"$hw/hwmon1/temp1_label"
echo 85850 >"$hw/hwmon1/temp1_max"; echo 87850 >"$hw/hwmon1/temp1_crit"
echo 50850 >"$hw/hwmon1/temp2_input"; echo "Sensor 1" >"$hw/hwmon1/temp2_label"; echo 65261850 >"$hw/hwmon1/temp2_max"
chip hwmon2 thinkpad
echo 60000 >"$hw/hwmon2/temp1_input"; echo CPU >"$hw/hwmon2/temp1_label"
echo 60000 >"$hw/hwmon2/temp3_input"; echo 0 >"$hw/hwmon2/temp4_input"
echo 4567 >"$hw/hwmon2/fan1_input"; echo 4400 >"$hw/hwmon2/fan2_input"
chip hwmon3 spd5118; echo 44250 >"$hw/hwmon3/temp1_input"
chip hwmon4 spd5118; echo 45000 >"$hw/hwmon4/temp1_input"
chip hwmon5 AC
printf 'processor\t: 0\nmodel name\t: AMD Ryzen 7 7840U w/ Radeon 780M Graphics\n' >"$work/cpuinfo"

snap=$("$bin" --once --sysfs "$hw" --cpuinfo "$work/cpuinfo")
check() { echo "$snap" | jq -e "$1" >/dev/null || fail "$2: $snap"; }
check '.v == 1 and .cpu == 61.1 and .hot == true' "summary"
check '[.groups[].kind] == ["cpu","memory","storage","system"]' "group order"
check '.groups[0].title == "AMD Ryzen 7 7840U"' "cpu title"
check '.groups[1].temps | length == 2 and .[0].n == 1 and .[1].n == 2' "memory modules"
check '.groups[2].title == "Fake SSD 1TB" and .groups[2].temps[0].hot and .groups[2].temps[0].limit == 85.8' "drive limit"
check '.groups[2].temps[1].high == null' "garbage threshold dropped"
check '.groups[3].temps | length == 1 and .[0].label == "CPU"' "board keeps labeled sensors"
check '.fans | length == 2 and .[0].rpm == 4567' "fans"
check '.alerts | length <= 1' "alerts"

# --warn moves the processor limit.
"$bin" --once --sysfs "$hw" --cpuinfo "$work/cpuinfo" --warn 60 |
  jq -e '.groups[0].temps[0].hot and .groups[0].temps[0].limit == 60' >/dev/null || fail "--warn"

# A missing tree is not an error, just no sensors.
"$bin" --once --sysfs "$work/nope" | jq -e '.groups == [] and .cpu == null' >/dev/null || fail "missing sysfs"

# --demo produces the documented fields.
"$bin" --demo --once | jq -e '.groups[0].temps[0] | has("n","label","t","high","crit","limit","hot")' >/dev/null ||
  fail "demo fields"

# This machine's own sensors produce a valid snapshot.
"$bin" --once | jq -e '.v == 1 and (.groups | type == "array") and (.fans | type == "array")' >/dev/null ||
  fail "real sensors"

# run.sh builds into the cache and runs.
XDG_CACHE_HOME="$work/cache" sh "$here/run.sh" --once --sysfs "$hw" --cpuinfo "$work/cpuinfo" |
  jq -e '.cpu == 61.1' >/dev/null || fail "run.sh"

if [ "$fails" -gt 0 ]; then
  echo "$fails failure(s)" >&2
  exit 1
fi
echo "helper: all tests passed"
