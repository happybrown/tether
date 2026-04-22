#!/bin/bash
set -e

mkdir -p bin

echo "빌드 중..."
(cd bconn-ui && swift build -c release 2>&1)
cp bconn-ui/.build/release/BconnApp bin/BconnApp

echo "-----------------------------------"
echo "빌드 성공!"
echo "실행: ./bin/BconnApp"
