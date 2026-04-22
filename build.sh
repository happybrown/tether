#!/bin/bash
set -e

mkdir -p bin

echo "[1/2] CLI 빌드..."
clang++ -std=c++17 -O3 bconn.cpp -o bin/bconn -lpthread \
    -framework ApplicationServices -framework Cocoa

echo "[2/2] UI 빌드..."
(cd bconn-ui && swift build -c release 2>&1)
cp bconn-ui/.build/release/BconnApp bin/BconnApp

echo "-----------------------------------"
echo "빌드 성공!"
echo "실행 방법:"
echo "  [CLI host]   ./bin/bconn host [port]"
echo "  [CLI client] ./bin/bconn client <host_ip> [port]"
echo "  [UI 앱]      ./bin/BconnApp"
