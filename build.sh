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

echo "빌드 중..."
(cd bconn-ui && xcodegen generate --quiet && \
  xcodebuild -project BconnApp.xcodeproj \
             -scheme BconnApp \
             -configuration Release \
             -derivedDataPath build/arm64 \
             -arch arm64 \
             -quiet 2>&1 && \
  xcodebuild -project BconnApp.xcodeproj \
             -scheme BconnApp \
             -configuration Release \
             -derivedDataPath build/x86_64 \
             -arch x86_64 \
             -quiet 2>&1)

# 두 아키텍처를 lipo로 합쳐 Universal Binary 생성
ARM_APP=bconn-ui/build/arm64/Build/Products/Release/Bconn.app
X86_APP=bconn-ui/build/x86_64/Build/Products/Release/Bconn.app

rm -rf bin/Bconn.app
cp -R "$ARM_APP" bin/Bconn.app

lipo -create \
     "$ARM_APP/Contents/MacOS/Bconn" \
     "$X86_APP/Contents/MacOS/Bconn" \
     -output bin/Bconn.app/Contents/MacOS/Bconn

echo "-----------------------------------"
echo "빌드 성공! (Universal Binary: arm64 + x86_64)"
echo "확인: lipo -info bin/Bconn.app/Contents/MacOS/Bconn"
echo "실행: open bin/Bconn.app"
