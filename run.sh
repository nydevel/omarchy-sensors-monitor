#!/bin/sh
# Builds the helper from sensors.cpp on first use (and after every update),
# then runs it. The binary lives in the user's cache, never in the plugin
# folder, so building does not trigger the shell's plugin hot reload.
set -eu

dir=$(cd "$(dirname "$0")" && pwd)
src="$dir/sensors.cpp"
cache="${XDG_CACHE_HOME:-$HOME/.cache}/omarchy-sensors-monitor"
hash=$(sha256sum "$src" | cut -c1-16)
bin="$cache/sensors-$hash"

if [ ! -x "$bin" ]; then
  mkdir -p "$cache"
  # One build at a time: the shell starts a widget per monitor.
  exec 9>"$cache/build.lock"
  flock 9
  if [ ! -x "$bin" ]; then
    tmp="$bin.tmp.$$"
    if ! "${CXX:-c++}" -std=c++17 -O2 -Wall -o "$tmp" "$src" 2>"$cache/build.log"; then
      rm -f "$tmp"
      echo "sensors-monitor: build failed, see $cache/build.log" >&2
      exit 1
    fi
    mv -f "$tmp" "$bin"
    # Drop builds of older versions.
    find "$cache" -maxdepth 1 -type f -name 'sensors-*' ! -name "sensors-$hash" -delete
  fi
  exec 9>&-
fi

exec "$bin" "$@"
