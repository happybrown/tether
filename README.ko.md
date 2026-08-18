# Tether

[English](README.md) | **한국어**

두 Mac 사이에서 마우스·키보드를 공유하는 메뉴 바 앱.
화면 경계로 커서를 밀면 다른 Mac으로 제어가 넘어가고, 반대 방향으로 밀면 돌아온다.

## 요구사항

- macOS 13 이상
- Xcode + [xcodegen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`)
- **손쉬운 사용(Accessibility) 권한** — Host·Client **양쪽 모두** 필요
  - Host: 키보드·마우스 이벤트 캡처(Event Tap)
  - Client: 원격 클릭·키 입력 주입(Event Post)
  - 시스템 설정 → 개인 정보 보호 및 보안 → 손쉬운 사용 → Tether 허용

## 빌드

```bash
bash build.sh          # 결과물: ./bin/Tether.app
bash build.sh clean    # 빌드 산출물 정리
bash build.sh cert     # 개발용 서명 인증서(tether-dev) 생성 (1회)
```

버전은 `project.yml`의 `MARKETING_VERSION`이 단일 소스이며, 앱의 메뉴 바 헤더와
설정 창에 표시된다. 빌드는 버전을 바꾸지 않는다 — 릴리스 시 태그에서 설정된다(아래 참고).

> `tether-dev` 인증서로 서명하면 재빌드해도 손쉬운 사용 권한이 유지된다.
> 인증서가 없으면 adhoc으로 서명되어 재빌드마다 권한을 다시 허용해야 한다.

## 실행

```bash
open bin/Tether.app
```

메뉴 바에 아이콘이 생기며 백그라운드로 동작한다(Dock 아이콘 없음).

## 두 대에 설치하기

Host와 Client 양쪽 Mac에 각각 설치해야 한다. 아키텍처가 달라도(arm64 / x86_64)
각 머신에서 빌드하면 된다.

```bash
bash build.sh              # 각 Mac에서 실행
bash make_dmg.sh           # dmg 패키징 (선택)
bash install_app.sh <dmg>  # /Applications 에 설치
```

## 릴리스

버전은 태그로 관리한다. `v*` 태그를 푸시하면 dmg 를 빌드해 GitHub 릴리스로 배포한다:

```bash
git tag v1.0.0
git push origin v1.0.0
```

워크플로가 빌드 전에 태그 버전을 `project.yml` 에 반영하므로, 앱 번들과 dmg
파일명이 항상 태그와 일치한다.

## 설정

메뉴 바 아이콘 클릭 → **설정...** (⌘,)

| 항목 | 설명 |
|------|------|
| 실행 모드 | **Host** — 입력을 보내는 쪽 / **Client** — 받는 쪽 |
| Client 위치 | Host 기준 Client 방향 (오른쪽 / 왼쪽 / 위쪽 / 아래쪽), 실행 중 변경 가능 |
| Host IP | Client 모드에서 접속할 Host의 IP |
| 포트 | 기본값 24900 |
| 공유 암호(PSK) | 설정 시 HMAC 인증. 비우면 인증 없음. **양쪽 동일하게** 입력 |
| 연결 확인 간격 | Client의 PING 주기 (연결 유지·재연결) |
| 로그인 시 자동 실행 / 자동 연결 | 로그인 후 앱·세션 자동 시작 |

## 사용 방법

1. **Host** 쪽: 모드 `Host` → 시작
2. **Client** 쪽: 모드 `Client` → Host IP 입력 → 시작
3. Host에서 커서를 설정 방향 경계까지 밀면 Client로 전환
4. Client 화면에서 반대 방향 경계까지 밀면 Host로 복귀

### 단축키 (Host)

- **Ctrl+Opt+Cmd+Esc** — 원격 모드에서 로컬로 강제 복귀
- **Cmd+Shift+Esc** — 세션 강제 중지 (완전 먹통 시)

## 공유 대상

- 마우스 이동 / 클릭 (좌·우·휠 클릭) / 스크롤
- 키보드 (modifier 키 포함)
- 클립보드 텍스트·이미지 (양방향)

## 프로젝트 구조

```
tether/
├── build.sh                 빌드 (버전 자동 증가, 서명)
├── test.sh                  테스트
├── project.yml              xcodegen 설정 (버전 단일 소스)
└── Sources/
    ├── TetherCore/           C++ 코어 (이벤트 캡처·주입, UDP 통신, 인증)
    └── TetherApp/            Swift UI (메뉴 바, 설정 화면)
```

자세한 아키텍처는 [ARCHITECTURE.md](ARCHITECTURE.md) 참고.

## 라이선스

[MIT](LICENSE) © Myeongjin Cho
