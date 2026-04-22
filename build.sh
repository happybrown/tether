#!/bin/bash
echo "빌드 중..."

clang++ -std=c++17 -O3 bconn.cpp -o bconn -lpthread \
    -framework ApplicationServices -framework Cocoa

if [ $? -eq 0 ]; then
    echo "빌드 성공!"
    echo "-----------------------------------"
    echo "실행 방법:"
    echo "  [수신측] ./bconn client [port]"
    echo "  [송신측] ./bconn host <대상IP> <port>"
else
    echo "빌드 실패."
fi
