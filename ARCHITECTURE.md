# 아키텍처

macOS 메뉴바 앱. 두 Mac 사이에서 마우스/키보드/클립보드를 UDP로 공유한다.
화면 경계에 커서가 닿으면 자동으로 상대 Mac으로 전환된다.

손쉬운 사용(Accessibility) 권한은 Host(이벤트 캡처)·Client(이벤트 주입) 양쪽 모두 필요하다.

## 디렉터리 구조

```
├── project.yml                  # xcodegen 설정 (빌드 시스템 단일 소스)
├── Sources/
│   ├── TetherCore/              # C++17 정적 라이브러리
│   │   ├── include/
│   │   │   ├── tether_core.h    # 공개 C API (Swift에서 import하는 인터페이스)
│   │   │   └── module.modulemap # Swift용 모듈 정의
│   │   ├── protocol.h           # 패킷 구조체 정의
│   │   ├── NetworkEngine.h      # UDP 송수신 엔진 (header-only C++ class)
│   │   ├── CursorUtil.h         # 커서 표시/숨김 유틸리티 (공용)
│   │   ├── EdgeGeometry.h       # 경계 기하 계산 (순수 함수, 단위 테스트 대상)
│   │   ├── EventTap.h/cpp       # 호스트 전용: CGEventTap, 원격 모드, 디스플레이 레이아웃
│   │   ├── ClientHandler.h/cpp  # 클라이언트 전용: 패킷 수신, 커서/키보드 주입
│   │   └── tether_core.cpp      # 핵심 로직: 인증, 생명주기, 전역 상태
│   └── TetherApp/               # Swift macOS 앱
│       ├── TetherApp.swift      # @main 진입점, MenuBarExtra
│       ├── AppDelegate.swift    # 앱 생명주기, tetherStatusChanged 알림 정의
│       ├── TetherManager.swift  # C API 래퍼, 상태 관리 (ObservableObject)
│       ├── TetherStatus+Swift.swift   # TetherStatus C enum → Swift extension
│       ├── Settings.swift       # AppSettings (UserDefaults 영속화)
│       ├── Localization.swift   # 다국어: AppLanguage, L10n, L() 조회 함수
│       ├── Resources/           # 언어별 문자열 (en.lproj / ko.lproj)
│       ├── SettingsView.swift   # 설정 창 UI (연결/보안/일반/로그 탭)
│       ├── SettingsComponents.swift   # 설정 공용 폼 컴포넌트 (FormSection 등)
│       ├── MonitorLayoutView.swift    # 모니터 배치 시각 선택 UI
│       ├── MenuBarView.swift    # 메뉴바 팝오버 UI
│       ├── LogView.swift        # 로그 뷰어 (설정 창의 로그 탭)
│       └── LaunchAtLogin.swift  # 로그인 시 자동 시작 (ServiceManagement)
└── Tests/
    ├── TetherCoreTests/         # Objective-C++ XCTest (C++ 로직 직접 테스트)
    │   ├── ProtocolTests.mm     # 패킷 크기/레이아웃, HMAC 인증, 타임스탬프 검증
    │   ├── EdgeGeometryTests.mm # 경계 기하 계산 단위 테스트
    │   └── NetworkEngineTests.mm      # UDP 송수신 통합 테스트 (localhost)
    └── TetherAppTests/          # Swift XCTest
        └── SettingsTests.swift  # TetherConfig 브리징, C API, 생명주기
```

## 패킷 흐름

```
[Host Event Tap] → tether_core.cpp → NetworkEngine::send_*() → UDP
    → NetworkEngine recv_loop() → 콜백 → [Client 이벤트 주입]
```

## 인증 흐름 (PSK + Hostname)

```
Client  →  HelloPacket(hostname, timestamp, HMAC-SHA1)  →  Host
Host    →  HMAC 검증 → AuthAckPacket(accepted)          →  Client
Client: AUTH_ACK 수신 전까지 host 패킷 무시
Host:   set_peer() 호출 전까지 send 불가 (peer_known=false)
```

- PSK 미설정 시 open mode (HMAC 검증 건너뜀)
- Client가 주기적으로 HelloPacket 재전송 → Host 재시작 후 자동 재연결
- 타임스탬프 ±30초 윈도우로 재전송 공격 방지
- 인증 후 Client는 PING, Host는 PONG으로 연결 생존 확인.
  원격 모드 중에는 PING 주기를 1초로 촘촘히 해 host watchdog 오탐 복귀를 막는다.
  Host watchdog의 원격 복귀 타임아웃은 PING 주기의 2배(최소 10초)로 마진을 둔다.

## 커서 동기화

- Host 커서가 설정된 경계(edge)에 3px 이내 진입 → `enter_remote()`
  - 드래그 중에는 진입 차단 (의도치 않은 전환 방지)
  - peer가 연결되어 있지 않으면 진입 차단
  - 진입 후 원격 중에는 커서를 offscreen으로 warp해 host 화면에서 보이지 않게 유지한다
    (생략하면 양쪽에 커서가 보인다).
- `EVENT_SYNC_CURSOR`: 진입 위치 비율(ratio) + 방향(edge) 전달
- Client: 비율을 자신의 화면 크기에 매핑해 커서 워프. 이동뿐 아니라 **클릭에도**
  커서를 warp한 뒤 이벤트를 주입한다 (위치 불일치로 클릭이 무시되는 것 방지).
- 복귀 판정: 실제 커서 위치를 아는 Client가 반대편 경계 이탈을 감지해
  LeavePacket(복귀 비율)으로 요청한다 (`request_leave_remote`).
- Host 커서가 복귀 → `EVENT_SHOW_CURSOR` → Client 커서 숨김

### 유실 대비 상태 동기화

UDP라 제어 신호가 유실되면 양쪽 커서 상태가 어긋난다. 방어 수단:

- 제어 신호(SYNC/SHOW_CURSOR)는 3회 연속 전송
- PONG에 host의 원격 활성 상태(active) 비트를 실어, Client가 자신의 상태와 다르면
  자가교정한다 (SHOW_CURSOR 유실로 양쪽에 커서가 남는 것을 수 초 내 자동 복구)
- 원격 복귀 시 `EVENT_RESET_MODIFIERS`를 보내 client에 고착된 modifier를 해제한다

### 커서 warp와 이벤트 억제

macOS는 커서 warp 후 기본 ~250ms 동안 하드웨어 이벤트의 커서 반영을 억제한다
(local events suppression). 이 억제는 상황에 따라 필요하기도, 해롭기도 하다:

- **원격 모드 중에는 필수** — offscreen 주차 커서가 물리 이동으로 화면에 되돌아오는 것과,
  warp 변위가 다음 이벤트 delta에 오염되는 것을 막아준다
- **복귀 순간에는 해로움** — 복귀 warp 직후 커서가 경계에 멈춰 보이는 끊김의 원인

따라서 전역으로 끄지 않고, 복귀 시점에만 0으로 낮추고 진입 시 기본값으로 복원한다.

### 디스플레이 변경

모니터 연결/해제/배치 변경은 Swift의 `didChangeScreenParametersNotification` 관찰자
→ `tether_displays_changed()`로 전달된다. Host는 레이아웃 재계산, Client는 화면 경계
재계산 후 새 크기를 host에 재전송한다(원격 중이면 로컬 복원). Host는 CGDisplay
reconfiguration 콜백도 있어 이중 안전망이다. Client는 recv/ping 스레드로 위임해 경합을 막는다.

### 정렬 미세조정

`client_offset`(-1.0~+1.0, client 화면 크기 대비 비율)으로 경계 축 방향 어긋남을 보정한다.
host→client 진입 비율에 -offset, client→host 복귀 비율에 +offset을 적용한다.
`tether_set_client_offset()`으로 실행 중 즉시 변경 가능하다.

실행 중 client 위치(edge) 변경은 `tether_set_client_edge()`로 재시작 없이 반영된다.

### 멀티 모니터 전환 기준

`trigger_display_uuid`로 특정 디스플레이의 특정 변을 전환 경계로 지정한다
(예: 맥북+외장 구성에서 "맥북 화면 오른쪽"). 빈 값이면 자동 = 데스크톱 최외곽 경계.

- 최외곽 경계는 커서가 멈추므로 3px 근접으로 판정하지만, **내부 경계**(지정 화면이
  데스크톱 안쪽)는 커서가 통과해 버리므로 직전 위치→현재 위치의 **경계선 통과**로
  판정한다(`interior_edge`). 진입 비율은 통과 직전 위치(경계선상)로 계산한다.
- 경계 기하 계산은 `EdgeGeometry.h`의 순수 함수로 분리했다 — 실제 모니터 없이 배치를
  rect로 구성해 단위 테스트(`EdgeGeometryTests.mm`)로 검증한다.
- `tether_get_displays()` / `tether_set_trigger_display()`로 UI가 host 디스플레이 배치를
  실제 좌표대로 그리고, 상대 Mac 사각형을 원하는 화면의 변에 붙여 기준 화면·방향·오프셋을
  한 번에 지정한다.

## 주요 전역 상태 (tether_core.cpp)

| 변수 | 설명 |
|------|------|
| `g_engine` | NetworkEngine 인스턴스 |
| `g_status` | 현재 연결 상태 (atomic) |
| `g_remote` | 원격 모드 진입/복귀 상태 |
| `g_client` | Client 커서 위치 및 화면 정보 |
| `g_psk` / `g_hostname` | 인증용 PSK, 호스트명 |
| `g_auth_ok` | Client 인증 완료 여부 (atomic) |
| `g_ping_thread` | Client: 인증 전 HELLO 재전송, 인증 후 PING(원격 중 1초). 디스플레이 dirty 처리 |
| `g_watchdog_thread` | Host: PING 무응답 감지 → 원격 복귀/peer 초기화 |
| `g_client_displays_dirty` | Client: 디스플레이 변경 플래그 (메인 스레드→ping 스레드 위임, atomic) |

## 패킷 구조 요약

| 타입 | 크기 | 방향 | 용도 |
|------|------|------|------|
| MousePacket | 10B | 양방향 | 마우스 이동/클릭/스크롤(버튼 번호 포함, 사이드 버튼 지원), 커서 동기화 이벤트(SYNC/SHOW) |
| KeyPacket | 11B | Host→Client | 키보드 입력, modifier 해제 |
| HelloPacket | 64B | Client→Host | 인증 요청 + heartbeat |
| AuthAckPacket | 34B | Host→Client | 인증 결과 응답 |
| ClientInfoPacket | 9B | Client→Host | 클라이언트 화면 크기 (너비/높이) |
| LeavePacket | 8B | Client→Host | 반대편 경계 이탈 복귀 요청 (비율) |
| ProbePacket | 12B | 양방향 | 네트워크 성능(RTT) 측정 |
| PING/PONG | 1B / 2B | 상호 | 연결 생존 확인. PONG 2번째 바이트 = host 원격 활성 상태 |
| Clipboard | 가변 | 양방향 | 클립보드 텍스트/이미지(PNG), 첫 바이트=형식 (MTU 안전 청크, 최대 8MB) |

## Swift ↔ C++ 경계

- `TetherCore`는 C ABI(`extern "C"`)로 노출 → Swift에서 `import TetherCore`
- `TetherManager`가 `TetherConfig` 조립 및 콜백 등록 담당
- 상태 변경은 `TetherStatusCallback` → `DispatchQueue.main.async` → `@Published` 프로퍼티
- 상태 콜백의 message 는 사람이 읽는 문구가 아니라 **안정적인 메시지 키**("connected",
  "waiting_client" 등)다. Swift 가 `status.<키>` 로 현지화해 표시한다.
- 상대 호스트명은 메시지에 싣지 않고 `tether_get_peer_hostname()` 로 조회한다.

## 다국어 (i18n)

- 문자열은 `Resources/<code>.lproj/Localizable.strings` 에 언어별로 둔다 (현재 en/ko).
  새 언어 추가 = lproj 폴더 + `AppLanguage` case 하나.
- `L10n`(ObservableObject) 이 선택 언어의 번들을 들고 `L("key")` 로 조회한다.
  뷰가 `L10n.shared` 를 관찰하므로 설정 > 일반 > 언어 변경이 재시작 없이 즉시 반영된다.
- 언어 설정: "시스템 기본"(OS 언어 따름) / English / 한국어. UserDefaults `appLanguage`.
- 로그(TETHER_INFO 등)는 진단용이라 현지화하지 않고 UI 언어와 무관하게 영어로 통일한다.
