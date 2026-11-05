#!/usr/bin/env bash
# Builds src/tapo_client.cpp for the PC and runs it against mock_plug.py.
# Needs: g++, libmbedtls-dev, python-kasa (pip), and one `pio run` so the
# ESP32 core (for Arduino's String class) and ArduinoJson are downloaded.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=../..
CORE=${ARDUINO_CORE:-$HOME/.platformio/packages/framework-arduinoespressif32/cores/esp32}
JSON=$ROOT/.pio/libdeps/matrixbit/ArduinoJson/src
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"; kill $(jobs -p) 2>/dev/null || true' EXIT

cp "$CORE"/WString.h "$CORE"/WString.cpp "$CORE"/stdlib_noniso.h "$CORE"/stdlib_noniso.c "$BUILD"/
cp shim/* "$BUILD"/

FLAGS="-std=gnu++17 -O1 -DARDUINO=10800 -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0 -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0 -DARDUINOJSON_ENABLE_PROGMEM=0 -I$BUILD -I$JSON"
gcc -c -I"$BUILD" "$BUILD"/stdlib_noniso.c -o "$BUILD"/noniso.o
gcc -c -I"$BUILD" "$BUILD"/host_libc.c -o "$BUILD"/host_libc.o
g++ $FLAGS -w -c "$BUILD"/WString.cpp -o "$BUILD"/wstring.o
g++ $FLAGS -Wall -c $ROOT/src/tapo_client.cpp -o "$BUILD"/tapo.o
g++ $FLAGS -Wall klap_test.cpp "$BUILD"/tapo.o "$BUILD"/wstring.o "$BUILD"/noniso.o "$BUILD"/host_libc.o -lmbedcrypto -o "$BUILD"/klap_test

export TAPO_TEST_PORT=18080
status=0
run() {
    local name=$1 expect=$2 password=$3; shift 3
    python3 mock_plug.py --port $TAPO_TEST_PORT "$@" > "$BUILD/mock.log" 2>&1 &
    local pid=$!
    sleep 1
    echo "=== $name"
    if "$BUILD"/klap_test user@example.com "$password" "$expect" > "$BUILD/test.log" 2>&1; then
        grep -E "^(PASS|FAIL)" "$BUILD/test.log"
    else
        cat "$BUILD/test.log"; echo "--- mock log"; cat "$BUILD/mock.log"; status=1
    fi
    kill $pid; wait $pid 2>/dev/null || true
}

run "cookie as one header"          ok        secret --cookie-style combined
run "cookies as separate headers"   ok        secret --cookie-style separate
run "TIMEOUT cookie listed first"   ok        secret --cookie-style timeout-first
run "session expires mid-run"       ok        secret --expire-after 3
run "wrong password"                auth-fail wrong
exit $status
