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

# 손쉬운 사용 권한 초기화는 재빌드마다 재허용을 강제해 번거롭다.
# 서명이 유지되면 권한도 유지되므로 기본 빌드에서는 초기화하지 않는다.
# 권한 캐시가 꼬였을 때만 `bash build.sh reset-tcc` 로 명시적으로 초기화한다.
if [[ "$1" == "reset-tcc" ]]; then
    echo "TCC 손쉬운 사용 권한 초기화..."
    tccutil reset Accessibility com.bconn.app
    echo "초기화 완료 — 다음 실행 시 권한을 다시 허용해야 한다."
    exit 0
fi

# 개발용 self-signed 코드서명 인증서(bconn-dev)를 로그인 키체인에 생성한다.
# adhoc 서명과 달리 재빌드해도 서명 주체가 같아 손쉬운 사용 권한이 유지된다.
# SSH 등 키체인이 잠긴 세션에서는 실패하므로 GUI 세션에서 실행할 것.
if [[ "$1" == "cert" ]]; then
    CERT_NAME="bconn-dev"
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
CN = bconn-dev
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
            -out p.p12 -passout pass:bconn -name "$CERT_NAME" \
            -certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg sha1 >/dev/null 2>&1; then
        openssl pkcs12 -export -descert -inkey key.pem -in cert.pem \
            -out p.p12 -passout pass:bconn -name "$CERT_NAME" \
            -certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg sha1 >/dev/null 2>&1
    fi
    security import p.p12 -k "$HOME/Library/Keychains/login.keychain-db" -P bconn -T /usr/bin/codesign
    security set-key-partition-list -S apple-tool:,apple: -k "" \
        "$HOME/Library/Keychains/login.keychain-db" >/dev/null 2>&1 || true
    security find-certificate -c "$CERT_NAME" -p "$HOME/Library/Keychains/login.keychain-db" > c.pem
    security add-trusted-cert -r trustRoot -p codeSign c.pem
    echo "생성 완료: $CERT_NAME"
    security find-identity -v -p codesigning | grep "$CERT_NAME"
    exit 0
fi

mkdir -p bin

# 빌드마다 버전 patch 자리(x.y.Z)를 자동 증가시킨다.
# project.yml 의 MARKETING_VERSION 이 버전 단일 소스이며, Info.plist 는 이를 변수로 참조한다.
# BCONN_NO_VERSION_BUMP=1 이면 증가를 건너뛴다 — deploy.sh 가 원격(iMac) 빌드 시 사용해
# 로컬이 이미 올린 버전을 그대로 유지(양쪽 이중 증가 방지).
PROJECT_YML="bconn-ui/project.yml"
CUR_VER=$(grep 'MARKETING_VERSION:' "$PROJECT_YML" | head -1 | sed -E 's/.*"([0-9]+\.[0-9]+\.[0-9]+)".*/\1/')
if [ -n "$BCONN_NO_VERSION_BUMP" ]; then
    echo "버전 증가 건너뜀 (BCONN_NO_VERSION_BUMP): $CUR_VER 유지"
elif [ -n "$CUR_VER" ]; then
    MAJOR=$(echo "$CUR_VER" | cut -d. -f1)
    MINOR=$(echo "$CUR_VER" | cut -d. -f2)
    PATCH=$(echo "$CUR_VER" | cut -d. -f3)
    NEW_VER="$MAJOR.$MINOR.$((PATCH + 1))"
    # CURRENT_PROJECT_VERSION(빌드 번호)도 함께 증가
    CUR_BUILD=$(grep 'CURRENT_PROJECT_VERSION:' "$PROJECT_YML" | head -1 | sed -E 's/.*"([0-9]+)".*/\1/')
    NEW_BUILD=$((CUR_BUILD + 1))
    sed -i '' -E "s/(MARKETING_VERSION: )\"[0-9.]+\"/\1\"$NEW_VER\"/" "$PROJECT_YML"
    sed -i '' -E "s/(CURRENT_PROJECT_VERSION: )\"[0-9]+\"/\1\"$NEW_BUILD\"/" "$PROJECT_YML"
    echo "버전: $CUR_VER → $NEW_VER (빌드 $NEW_BUILD)"
fi

# 빌드 구성 선택. 기본 Release(성능 최적화 -O3, DEBUG 로그 제거).
# `bash build.sh debug` → Debug(-O0, DEBUG 매크로 → BCONN_DEBUG 상세 로그 활성화).
CONFIG="Release"
if [[ "$1" == "debug" ]]; then
    CONFIG="Debug"
fi

ARCH=$(uname -m)
echo "빌드 중... (아키텍처: $ARCH, 구성: $CONFIG)"

(cd bconn-ui && xcodegen generate --quiet && \
  xcodebuild -project BconnApp.xcodeproj \
             -scheme BconnApp \
             -configuration "$CONFIG" \
             -derivedDataPath build \
             -arch "$ARCH" \
             -quiet 2>&1)

rm -rf bin/Bconn.app
cp -R "bconn-ui/build/Build/Products/$CONFIG/Bconn.app" bin/Bconn.app

# quarantine 속성 제거 후 재서명 — Gatekeeper가 adhoc 서명 앱을 거부하는 것을 방지
xattr -cr bin/Bconn.app

# 참고: SSH(비대화형) 세션에서 login keychain 이 잠겨 있으면 codesign 이 개인키에 접근 못 해
# adhoc 으로 떨어진다. 원격(iMac) 빌드 시의 keychain unlock 은 deploy.sh 가 책임진다
# (로컬 keychain 에 저장한 iMac 암호를 SSH 세션에서 unlock). 여기서는 순수 빌드만 한다.

# 개발용 인증서(bconn-dev)가 있으면 그걸로 서명한다.
# 인증서 기반 서명은 재빌드해도 코드 해시가 아닌 서명 주체로 TCC가 앱을 식별하므로
# 손쉬운 사용 권한이 유지된다. 인증서가 없으면 adhoc(-) 으로 폴백한다.
# (인증서 생성: bash build.sh cert)
if security find-identity -v -p codesigning 2>/dev/null | grep -q "bconn-dev"; then
    SIGN_ID="bconn-dev"
    echo "서명: bconn-dev 인증서 (권한 유지)"
else
    SIGN_ID="-"
    echo "서명: adhoc (bconn-dev 인증서 없음 — 재빌드 시 권한 재허용 필요. 'bash build.sh cert' 로 생성 가능)"
fi
codesign --force --deep --sign "$SIGN_ID" bin/Bconn.app

echo "-----------------------------------"
echo "빌드 성공! ($ARCH)"
echo "실행: open bin/Bconn.app"
