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

void bconn_set_log_callback(BconnLogCallback cb, void* ctx);

// 세션 시작/중지
bool bconn_start(const BconnConfig* config, BconnStatusCallback cb, void* ctx);
void bconn_stop(void);

BconnStatus bconn_get_status(void);

// 현재 호스트의 로컬 IP 목록 — '\n'으로 구분된 문자열을 out 버퍼에 기록
// out_size: 버퍼 크기, 반환값: 기록된 IP 개수
int bconn_get_local_ips(char* out, int out_size);

#ifdef __cplusplus
}
#endif
