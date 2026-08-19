#!/bin/bash
# make_dmg.sh — bin/Tether.app 을 배포용 .dmg 로 패키징한다.
#
# 표준 macOS 배포 형태: 앱과 /Applications 드래그 링크를 담은 압축 dmg.
# 사용자는 dmg 를 열어 앱을 Applications 로 끌어다 놓으면 설치된다.
# deploy.sh 가 이 dmg 를 만들어 로컬/원격에 자동 설치한다.
#
# 사용법: bash make_dmg.sh   → bin/Tether-<버전>-<아키텍처>.dmg 생성 (경로를 stdout 마지막 줄로 출력)
set -euo pipefail
cd "$(dirname "$0")"

APP="bin/Tether.app"
[ -d "$APP" ] || { echo "앱이 없습니다: $APP (먼저 build.sh 실행)" >&2; exit 1; }

# 버전은 앱 번들에서 직접 읽어 파일명에 넣는다 (project.yml 과 항상 일치).
VER=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$APP/Contents/Info.plist" 2>/dev/null || echo "0")

# 아키텍처 접미사도 바이너리에서 직접 판별한다 — 릴리스 자산 3종
# (universal/arm64/x86_64)을 파일명만으로 구분할 수 있게 한다.
ARCHS=$(lipo -archs "$APP/Contents/MacOS/Tether")
if [[ "$ARCHS" == *arm64* && "$ARCHS" == *x86_64* ]]; then
    SUFFIX="universal"
else
    SUFFIX="$ARCHS"
fi
DMG="bin/Tether-$VER-$SUFFIX.dmg"

# staging 디렉터리에 앱 + Applications 링크만 담는다 (임시, 종료 시 정리).
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"

rm -f "$DMG"
# UDZO: zlib 압축 read-only 이미지 (배포 표준). 볼륨명은 앱 이름으로.
hdiutil create -quiet \
    -volname "Tether" \
    -srcfolder "$STAGE" \
    -fs HFS+ \
    -format UDZO \
    "$DMG"

echo "dmg 생성 완료: $DMG" >&2
# 마지막 줄에 경로만 출력 → 호출자(deploy.sh)가 캡처해 쓴다.
echo "$DMG"
