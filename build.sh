#!/bin/bash
echo "빌드 중..."

clang++ -std=c++17 -O3 bconn.cpp -o bin/bconn -lpthread \
    -framework ApplicationServices -framework Cocoa

if [ $? -eq 0 ]; then
    echo "빌드 성공!"
    echo "-----------------------------------"
    echo "실행 방법:"
    echo "  [host측] ./bin/bconn host [port]"
    echo "  [client측] ./bin/bconn client <host_ip> [port]"
else
    echo "빌드 실패."
fi
