# bconn

두 Mac 사이에서 마우스·키보드를 공유하는 menu bar 앱.  
화면 경계로 커서를 밀면 다른 Mac으로 제어가 넘어가고, 다시 반대 방향으로 밀면 돌아온다.

## 요구사항

- macOS 13 이상
- Xcode Command Line Tools (`xcode-select --install`)
- **손쉬운 사용(Accessibility) 권한** — Event Tap 사용에 필요
  - 시스템 설정 → 개인 정보 보호 및 보안 → 손쉬운 사용 → BconnApp 허용

## 빌드

```bash
bash build.sh
# 결과물: ./bin/BconnApp
```

## 실행

```bash
./bin/BconnApp
```

메뉴 바에 아이콘이 생기며 백그라운드로 동작한다.

## 설정

메뉴 바 아이콘 클릭 → **설정...** (⌘,)

| 항목 | 설명 |
|------|------|
| 실행 모드 | **Host** — 입력 장치를 보내는 쪽 / **Client** — 받는 쪽 |
| Client 위치 | Host 기준으로 Client가 있는 방향 (오른쪽 / 왼쪽 / 위쪽 / 아래쪽) |
| Host IP | Client 모드에서 접속할 Host의 IP 주소 |
| 포트 | 기본값 24900 |
| 로그인 시 자동 시작 | 로그인 후 앱 자동 실행 |

## 사용 방법

1. **Host** 쪽에서 앱 실행 → 모드를 `Host`로 설정 → 시작
2. **Client** 쪽에서 앱 실행 → 모드를 `Client`로 설정 → Host IP 입력 → 시작
3. Host에서 커서를 설정한 방향 경계까지 밀면 Client로 전환
4. Client 화면에서 반대 방향 경계까지 밀면 Host로 복귀

## 공유 대상

- 마우스 이동 / 클릭 (좌·우·휠 클릭)
- 스크롤 휠
- 키보드 (modifier 키 포함)

## 프로젝트 구조

```
bconn/
├── build.sh
└── bconn-ui/
    ├── Package.swift
    └── Sources/
        ├── BconnCore/          C++ 코어 (이벤트 캡처·주입, UDP 통신)
        └── BconnApp/           Swift UI (menu bar, 설정 화면)
```
