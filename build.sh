#!/bin/bash
set -e

if [[ "$1" == "clean" ]]; then
    echo "클린 중..."
    rm -rf bin/Bconn.app
    rm -rf bconn-ui/build
    rm -rf bconn-ui/BconnApp.xcodeproj
    echo "클린 완료"
    exit 0
fi

mkdir -p bin

ARCH=$(uname -m)
echo "빌드 중... (아키텍처: $ARCH)"

(cd bconn-ui && xcodegen generate --quiet && \
  xcodebuild -project BconnApp.xcodeproj \
             -scheme BconnApp \
             -configuration Release \
             -derivedDataPath build \
             -arch "$ARCH" \
             -quiet 2>&1)

rm -rf bin/Bconn.app
cp -R "bconn-ui/build/Build/Products/Release/Bconn.app" bin/Bconn.app

# quarantine 속성 제거 후 재서명 — Gatekeeper가 adhoc 서명 앱을 거부하는 것을 방지
xattr -cr bin/Bconn.app
codesign --force --deep --sign - bin/Bconn.app

# TCC 권한 초기화 — 재빌드 시 이전 권한 캐시가 남아 Event Tap 등이 오작동하는 것을 방지
tccutil reset All com.bconn.app

echo "-----------------------------------"
echo "빌드 성공! ($ARCH)"
echo "실행: open bin/Bconn.app"
