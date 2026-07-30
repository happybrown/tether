#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TETHER_EDGE_RIGHT  = 0,
    TETHER_EDGE_LEFT   = 1,
    TETHER_EDGE_TOP    = 2,
    TETHER_EDGE_BOTTOM = 3,
} TetherEdge;

typedef enum {
    TETHER_STATUS_IDLE        = 0,
    TETHER_STATUS_WAITING     = 1, // host: client 대기 중
    TETHER_STATUS_CONNECTED   = 2, // 연결됨
    TETHER_STATUS_REMOTE      = 3, // 원격 모드 활성
    TETHER_STATUS_ERROR       = 4,
    TETHER_STATUS_AUTH_FAILED = 5, // PSK 불일치
} TetherStatus;

typedef struct {
    bool      is_host;
    char      host_ip[64];  // client 모드: 접속할 host IP
    uint16_t  port;
    TetherEdge client_edge;  // client가 host 기준 어느 방향에 있는지
    char      psk[128];     // 공유 암호 (빈 문자열이면 인증 없음)
    uint16_t  reconnect_interval_s; // client: PING/HELLO 재전송 간격(초), 0이면 기본값(5초)
    bool      predict_leave; // host: 자체 복귀 예측 사용 여부 (false면 LeavePacket 안전망만)
} TetherConfig;

#ifndef TETHER_LOG_LEVEL_DEFINED
#define TETHER_LOG_LEVEL_DEFINED
typedef enum {
    TETHER_LOG_DEBUG = 0,
    TETHER_LOG_INFO  = 1,
    TETHER_LOG_ERROR = 2,
} TetherLogLevel;
#endif

// 상태 변경 콜백 (Swift → 등록, C++ → 호출)
typedef void (*TetherStatusCallback)(TetherStatus status, const char* message, void* ctx);

// 로그 콜백 — 레벨, 카테고리, 메시지를 Swift로 전달
typedef void (*TetherLogCallback)(TetherLogLevel level, const char* category, const char* message, void* ctx);

// 클립보드 페이로드 형식 — 페이로드 첫 바이트로 구분한다.
// C 코어는 바이트를 그대로 나르고, 형식 해석은 양쪽 Swift 계층이 담당한다.
typedef enum {
    TETHER_CLIP_TEXT = 0, // UTF-8 텍스트
    TETHER_CLIP_PNG  = 1, // PNG 이미지
} TetherClipFormat;

// 클립보드 페이로드 최대 크기 (형식 바이트 포함).
// protocol.h 의 CLIPBOARD_MAX_BYTES 와 반드시 일치해야 한다 (tether_core.cpp 에서 검증).
enum { TETHER_CLIPBOARD_MAX_BYTES = 8 * 1024 * 1024 };

// 클립보드 수신 콜백 — 상대방 클립보드 변경 시 Swift로 전달
typedef void (*TetherClipboardCallback)(const uint8_t* data, uint32_t len, void* ctx);

void tether_set_log_callback(TetherLogCallback cb, void* ctx);

// 클립보드 수신 콜백 등록 — tether_start 전에 호출
void tether_set_clipboard_callback(TetherClipboardCallback cb, void* ctx);

// 클립보드 페이로드([형식 1B] + 내용)를 상대방에게 전송 (인증 완료 상태에서만 동작).
// 큰 이미지 전송은 내부 페이싱으로 수백 ms 걸릴 수 있으므로 백그라운드 스레드에서 호출할 것.
void tether_send_clipboard(const uint8_t* data, uint32_t len);

// 세션 시작/중지
bool tether_start(const TetherConfig* config, TetherStatusCallback cb, void* ctx);
void tether_stop(void);

// atexit + 시그널 핸들러 설치 (crash/정상 종료 시 이벤트 탭 비활성화 + 커서 연동 복구).
// 반복 호출해도 실제 설치는 1회만 이뤄진다. tether_start 진입부에서 자동 호출된다.
void tether_install_safety_hooks(void);

// 사용자 escape hatch: 원격 모드에 갇혔을 때 로컬로 강제 복귀.
// host 는 원격 모드를 종료, client 는 인증/원격 상태를 리셋해 재연결한다.
void tether_force_local(void);

// client 위치(host 기준 방향)를 실시간으로 변경한다 (host 모드 전용).
// 실행 중에도 세션 재시작 없이 커서 전환 경계를 즉시 갱신한다.
// 실행 전에 호출하면 다음 tether_start 에 반영된다.
void tether_set_client_edge(TetherEdge edge);

// host 자체 복귀 예측 사용 여부를 실시간으로 변경한다 (host 모드 전용).
// 끄면 client 의 LeavePacket 안전망으로만 복귀한다(왕복 지연 있음, 오탐 없음).
void tether_set_predict_leave(bool enabled);

// Cmd+Shift+Esc 감지 시 호출될 콜백 등록 (host 모드 전용).
// cb 는 CGEventTap RunLoop 스레드에서 호출되므로 내부에서 비동기 처리해야 한다.
typedef void (*TetherForceStopCallback)(void* ctx);
void tether_set_force_stop_callback(TetherForceStopCallback cb, void* ctx);

// 모니터 연결/해제/배치 변경 시 호출한다 (host/client 공용).
// Swift 의 didChangeScreenParametersNotification 관찰자가 호출한다.
// host: 디스플레이 레이아웃(trigger edge 등)을 재계산한다. 원격 모드 중이면 먼저 복귀.
// client: 화면 경계를 재계산하고 새 크기를 host 에 재전송한다. 원격 모드 중이면 먼저 로컬 복원.
// 여러 스레드에서 안전하게 호출 가능 (내부에서 적절한 스레드로 위임).
void tether_displays_changed(void);

TetherStatus tether_get_status(void);

// 현재 호스트의 로컬 IP 목록 — '\n'으로 구분된 문자열을 out 버퍼에 기록
// out_size: 버퍼 크기, 반환값: 기록된 IP 개수
int tether_get_local_ips(char* out, int out_size);

// ── 네트워크 성능(RTT) 측정 ───────────────────────────────────────────
// 연결된 상대 Mac 으로 probe 패킷을 왕복시켜 실제 UDP 경로의 지연을 측정한다.
// 앱이 실제 쓰는 소켓/경로를 그대로 사용하므로 마우스 지연을 가장 정확히 반영한다.

typedef struct {
    int    sent;         // 보낸 probe 수
    int    received;     // 응답받은 수
    double loss_pct;     // 손실률 (%)
    double min_ms;       // 최소 RTT
    double avg_ms;       // 평균 RTT
    double max_ms;       // 최대 RTT
    double stddev_ms;    // 표준편차 (지터)
} TetherNetPerfResult;

// 측정 완료 콜백. result 는 콜백 반환 후 무효화되므로 필요하면 복사할 것.
typedef void (*TetherNetPerfCallback)(const TetherNetPerfResult* result, void* ctx);

// 비동기로 count 회 probe 를 interval_ms 간격으로 보내 RTT 를 측정한다.
// 인증 완료(연결) 상태에서만 동작하며, 미연결 시 콜백에 sent=0 결과를 전달한다.
// 측정은 백그라운드 스레드에서 수행되고, 완료 시 cb 가 그 스레드에서 호출된다.
// 이미 측정이 진행 중이면 무시된다(false 반환). 시작하면 true.
bool tether_run_netperf(int count, int interval_ms, TetherNetPerfCallback cb, void* ctx);

#ifdef __cplusplus
}
#endif
