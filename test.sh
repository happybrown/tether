#!/bin/bash
set -e

echo "테스트 빌드 중..."
(cd bconn-ui && xcodegen generate --quiet)

run_tests() {
    local scheme=$1
    echo ""
    echo "▶ $scheme"
    (cd bconn-ui && xcodebuild test \
        -project BconnApp.xcodeproj \
        -scheme "$scheme" \
        -configuration Debug \
        -derivedDataPath build \
        -destination "platform=macOS" \
        -quiet 2>&1 | grep -E "Test Suite|Test Case|error:|passed|failed|PASS|FAIL" || true)
}

run_tests "BconnCoreTests"
run_tests "BconnAppTests"

echo ""
echo "-----------------------------------"
echo "테스트 완료"
