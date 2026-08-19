#!/bin/bash
set -e

if [[ "$1" == "clean" ]]; then
    echo "클린 중..."
    rm -rf bin/Tether.app
    rm -rf build
    rm -rf TetherApp.xcodeproj
    echo "클린 완료"
    exit 0
fi

# 손쉬운 사용 권한 초기화는 재빌드마다 재허용을 강제해 번거롭다.
# 서명이 유지되면 권한도 유지되므로 기본 빌드에서는 초기화하지 않는다.
# 권한 캐시가 꼬였을 때만 `bash build.sh reset-tcc` 로 명시적으로 초기화한다.
if [[ "$1" == "reset-tcc" ]]; then
    echo "TCC 손쉬운 사용 권한 초기화..."
    tccutil reset Accessibility com.tether.app
    echo "초기화 완료 — 다음 실행 시 권한을 다시 허용해야 한다."
    exit 0
fi

# 개발용 self-signed 코드서명 인증서(tether-dev)를 로그인 키체인에 생성한다.
# adhoc 서명과 달리 재빌드해도 서명 주체가 같아 손쉬운 사용 권한이 유지된다.
# SSH 등 키체인이 잠긴 세션에서는 실패하므로 GUI 세션에서 실행할 것.
if [[ "$1" == "cert" ]]; then
    CERT_NAME="tether-dev"
    if security find-identity -v -p codesigning 2>/dev/null | grep -q "$CERT_NAME"; then
        echo "이미 존재: $CERT_NAME"
        exit 0
    fi
    W=$(mktemp -d); trap 'rm -rf "$W"' EXIT; cd "$W"
    cat > ext.cnf <<'CNF'
[req]
distinguished_name = dn
x509_extensions = v3
prompt = no
[dn]
CN = tether-dev
[v3]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
CNF
    openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
        -days 3650 -nodes -config ext.cnf >/dev/null 2>&1
    # 인증서/키를 모두 3DES(SHA1)로 암호화해야 macOS security 가 임포트 가능하다.
    # OpenSSL 3.x 는 -legacy 가 추가로 필요하고, LibreSSL(macOS 기본)은 -legacy 미지원이라
    # -descert 조합만 쓴다. 먼저 OpenSSL 경로(-legacy)를 시도하고 실패 시 LibreSSL 경로로 폴백.
    if ! openssl pkcs12 -export -legacy -inkey key.pem -in cert.pem \
            -out p.p12 -passout pass:tether -name "$CERT_NAME" \
            -certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg sha1 >/dev/null 2>&1; then
        openssl pkcs12 -export -descert -inkey key.pem -in cert.pem \
            -out p.p12 -passout pass:tether -name "$CERT_NAME" \
            -certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg sha1 >/dev/null 2>&1
    fi
    security import p.p12 -k "$HOME/Library/Keychains/login.keychain-db" -P tether -T /usr/bin/codesign
    security set-key-partition-list -S apple-tool:,apple: -k "" \
        "$HOME/Library/Keychains/login.keychain-db" >/dev/null 2>&1 || true
    security find-certificate -c "$CERT_NAME" -p "$HOME/Library/Keychains/login.keychain-db" > c.pem
    security add-trusted-cert -r trustRoot -p codeSign c.pem
    echo "생성 완료: $CERT_NAME"
    security find-identity -v -p codesigning | grep "$CERT_NAME"
    exit 0
fi

mkdir -p bin

# 버전은 태그로 관리한다 — 빌드는 project.yml 의 값을 그대로 읽어 쓴다.
# 릴리스는 v* 태그를 푸시하면 .github/workflows/release.yml 이 태그 버전을
# project.yml 에 반영해 빌드하므로, 로컬 빌드가 버전을 건드릴 이유가 없다.
PROJECT_YML="project.yml"
CUR_VER=$(grep 'MARKETING_VERSION:' "$PROJECT_YML" | head -1 | sed -E 's/.*"([0-9]+\.[0-9]+\.[0-9]+)".*/\1/')
echo "버전: ${CUR_VER:-unknown}"

# 빌드 구성/아키텍처 선택. 인자는 순서 무관하게 조합 가능.
#   debug                    → Debug(-O0, DEBUG 매크로 → TETHER_DEBUG 상세 로그 활성화)
#   universal|arm64|x86_64   → 대상 아키텍처 (기본: 현재 머신, universal = arm64+x86_64)
# 기본 Release(성능 최적화 -O3, DEBUG 로그 제거).
CONFIG="Release"
ARCH=$(uname -m)
for arg in "$@"; do
    case "$arg" in
        debug)                  CONFIG="Debug" ;;
        universal|arm64|x86_64) ARCH="$arg" ;;
    esac
done

# universal 은 두 아키텍처를 한 번들에 담는다. Debug 구성은 ONLY_ACTIVE_ARCH=YES 가
# 기본이라 한 아키텍처만 빌드되므로 명시적으로 꺼서 두 슬라이스를 강제한다.
if [[ "$ARCH" == "universal" ]]; then
    ARCH_FLAGS=(-arch arm64 -arch x86_64 ONLY_ACTIVE_ARCH=NO)
else
    ARCH_FLAGS=(-arch "$ARCH")
fi

echo "빌드 중... (아키텍처: $ARCH, 구성: $CONFIG)"

xcodegen generate --quiet
xcodebuild -project TetherApp.xcodeproj \
           -scheme TetherApp \
           -configuration "$CONFIG" \
           -derivedDataPath build \
           "${ARCH_FLAGS[@]}" \
           -quiet 2>&1

rm -rf bin/Tether.app
cp -R "build/Build/Products/$CONFIG/Tether.app" bin/Tether.app

# quarantine 속성 제거 후 재서명 — Gatekeeper가 adhoc 서명 앱을 거부하는 것을 방지
xattr -cr bin/Tether.app

# 참고: SSH(비대화형) 세션에서 login keychain 이 잠겨 있으면 codesign 이 개인키에 접근 못 해
# adhoc 으로 떨어진다. 원격 빌드 시에는 미리 `security unlock-keychain` 이 필요하다.

# 개발용 인증서(tether-dev)가 있으면 그걸로 서명한다.
# 인증서 기반 서명은 재빌드해도 코드 해시가 아닌 서명 주체로 TCC가 앱을 식별하므로
# 손쉬운 사용 권한이 유지된다. 인증서가 없으면 adhoc(-) 으로 폴백한다.
# (인증서 생성: bash build.sh cert)
if security find-identity -v -p codesigning 2>/dev/null | grep -q "tether-dev"; then
    SIGN_ID="tether-dev"
    echo "서명: tether-dev 인증서 (권한 유지)"
else
    SIGN_ID="-"
    echo "서명: adhoc (tether-dev 인증서 없음 — 재빌드 시 권한 재허용 필요. 'bash build.sh cert' 로 생성 가능)"
fi
codesign --force --deep --sign "$SIGN_ID" bin/Tether.app

echo "-----------------------------------"
echo "빌드 성공! ($(lipo -archs bin/Tether.app/Contents/MacOS/Tether))"
echo "실행: open bin/Tether.app"
