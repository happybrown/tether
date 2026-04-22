#!/bin/bash
echo "🔨 순수 C++ Host 및 Client 빌드 중..."

# Host 빌드 (Cocoa 프레임워크 제거)
clang++ -std=c++11 -O3 main.cpp -o host_app -lpthread -framework ApplicationServices -framework Cocoa

# Client 빌드
clang++ -std=c++11 -O3 client.cpp -o client_app -framework ApplicationServices

if [ $? -eq 0 ]; then
    echo "✅ 빌드 성공!"
    echo "-----------------------------------"
    echo "실행 방법:"
    echo "  [수신측] ./client_app"
    echo "  [송신측] ./host_app <대상IP> 9000"
else
    echo "❌ 빌드 실패."
fi