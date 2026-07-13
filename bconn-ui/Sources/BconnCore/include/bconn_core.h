#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BCONN_EDGE_RIGHT  = 0,
    BCONN_EDGE_LEFT   = 1,
    BCONN_EDGE_TOP    = 2,
    BCONN_EDGE_BOTTOM = 3,
} BconnEdge;

typedef enum {
    BCONN_STATUS_IDLE        = 0,
    BCONN_STATUS_WAITING     = 1, // host: client 대기 중
    BCONN_STATUS_CONNECTED   = 2, // 연결됨
    BCONN_STATUS_REMOTE      = 3, // 원격 모드 활성
    BCONN_STATUS_ERROR       = 4,
    BCONN_STATUS_AUTH_FAILED = 5, // PSK 불일치
} BconnStatus;

typedef struct {
    bool      is_host;
    char      host_ip[64];  // client 모드: 접속할 host IP
    uint16_t  port;
    BconnEdge client_edge;  // client가 host 기준 어느 방향에 있는지
    char      psk[128];     // 공유 암호 (빈 문자열이면 인증 없음)
    uint16_t  reconnect_interval_s; // client: PING/HELLO 재전송 간격(초), 0이면 기본값(5초)
    bool      predict_leave; // host: 자체 복귀 예측 사용 여부 (false면 LeavePacket 안전망만)
} BconnConfig;

#ifndef BCONN_LOG_LEVEL_DEFINED
#define BCONN_LOG_LEVEL_DEFINED
typedef enum {
    BCONN_LOG_DEBUG = 0,
    BCONN_LOG_INFO  = 1,
    BCONN_LOG_ERROR = 2,
} BconnLogLevel;
#endif

// 상태 변경 콜백 (Swift → 등록, C++ → 호출)
typedef void (*BconnStatusCallback)(BconnStatus status, const char* message, void* ctx);

// 로그 콜백 — 레벨, 카테고리, 메시지를 Swift로 전달
typedef void (*BconnLogCallback)(BconnLogLevel level, const char* category, const char* message, void* ctx);

// 클립보드 수신 콜백 — 상대방 클립보드 변경 시 Swift로 전달
typedef void (*BconnClipboardCallback)(const uint8_t* data, uint32_t len, void* ctx);

void bconn_set_log_callback(BconnLogCallback cb, void* ctx);

// 클립보드 수신 콜백 등록 — bconn_start 전에 호출
void bconn_set_clipboard_callback(BconnClipboardCallback cb, void* ctx);

// 클립보드 텍스트를 상대방에게 전송 (UTF-8, 인증 완료 상태에서만 동작)
void bconn_send_clipboard(const uint8_t* data, uint32_t len);

// 세션 시작/중지
bool bconn_start(const BconnConfig* config, BconnStatusCallback cb, void* ctx);
void bconn_stop(void);

// atexit + 시그널 핸들러 설치 (crash/정상 종료 시 이벤트 탭 비활성화 + 커서 연동 복구).
// 반복 호출해도 실제 설치는 1회만 이뤄진다. bconn_start 진입부에서 자동 호출된다.
void bconn_install_safety_hooks(void);

// 사용자 escape hatch: 원격 모드에 갇혔을 때 로컬로 강제 복귀.
// host 는 원격 모드를 종료, client 는 인증/원격 상태를 리셋해 재연결한다.
void bconn_force_local(void);

// client 위치(host 기준 방향)를 실시간으로 변경한다 (host 모드 전용).
// 실행 중에도 세션 재시작 없이 커서 전환 경계를 즉시 갱신한다.
// 실행 전에 호출하면 다음 bconn_start 에 반영된다.
void bconn_set_client_edge(BconnEdge edge);

// host 자체 복귀 예측 사용 여부를 실시간으로 변경한다 (host 모드 전용).
// 끄면 client 의 LeavePacket 안전망으로만 복귀한다(왕복 지연 있음, 오탐 없음).
void bconn_set_predict_leave(bool enabled);

// Cmd+Shift+Esc 감지 시 호출될 콜백 등록 (host 모드 전용).
// cb 는 CGEventTap RunLoop 스레드에서 호출되므로 내부에서 비동기 처리해야 한다.
typedef void (*BconnForceStopCallback)(void* ctx);
void bconn_set_force_stop_callback(BconnForceStopCallback cb, void* ctx);

// 모니터 연결/해제/배치 변경 시 호출한다 (host/client 공용).
// Swift 의 didChangeScreenParametersNotification 관찰자가 호출한다.
// host: 디스플레이 레이아웃(trigger edge 등)을 재계산한다. 원격 모드 중이면 먼저 복귀.
// client: 화면 경계를 재계산하고 새 크기를 host 에 재전송한다. 원격 모드 중이면 먼저 로컬 복원.
// 여러 스레드에서 안전하게 호출 가능 (내부에서 적절한 스레드로 위임).
void bconn_displays_changed(void);

BconnStatus bconn_get_status(void);

// 현재 호스트의 로컬 IP 목록 — '\n'으로 구분된 문자열을 out 버퍼에 기록
// out_size: 버퍼 크기, 반환값: 기록된 IP 개수
int bconn_get_local_ips(char* out, int out_size);

#ifdef __cplusplus
}
#endif
