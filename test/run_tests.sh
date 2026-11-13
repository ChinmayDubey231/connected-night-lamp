#!/usr/bin/env sh
# Compiles sketch.ino against fake Arduino, WiFi and PubSubClient headers
# and runs the outage / reconnect / buffering tests.
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -I. -x c++ test_sketch.cpp -o /tmp/connected_lamp_tests
/tmp/connected_lamp_tests
