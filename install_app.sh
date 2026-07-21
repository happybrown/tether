#!/bin/bash
# install_app.sh — dmg 를 /Applications 에 설치한다 (로컬·원격 공용).
#
# 사용법: bash install_app.sh <dmg 경로> [launch]
#   launch=1 이면 설치 후 앱을 실행한다.
#
# 동작:
#   1) 실행 중인 앱 종료 (설치본을 덮어쓰려면 프로세스가 죽어야 함)
#   2) dmg 마운트 → /Applications 로 앱 통째 교체 → 언마운트
#   3) quarantine 속성 제거 (dmg 경유 Gatekeeper 격리 방지)
set -euo pipefail

DMG="${1:?dmg 경로가 필요합니다}"
LAUNCH="${2:-0}"
DEST="/Applications/Tether.app"

# 설치본을 덮어쓰기 전에 실행 중인 앱 종료 (완전히 사라질 때까지 최대 3초 대기).
pkill -x Tether 2>/dev/null || true
for _ in $(seq 1 30); do pgrep -x Tether >/dev/null || break; sleep 0.1; done

# dmg 마운트 (Finder 창 없이, 읽기전용). 마운트 지점을 파싱해 종료 시 언마운트.
MP=$(hdiutil attach "$DMG" -nobrowse -readonly | grep /Volumes/ | sed -E 's/.*(\/Volumes\/.*)/\1/')
trap 'hdiutil detach "$MP" -quiet 2>/dev/null || true' EXIT

# 기존 설치본 제거 후 새로 복사 (버전 잔재 없이 통째 교체).
rm -rf "$DEST"
cp -R "$MP/Tether.app" "$DEST"
xattr -cr "$DEST"

[ "$LAUNCH" = "1" ] && open "$DEST" || true
echo "설치 완료: $DEST"
