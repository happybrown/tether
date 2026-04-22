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
    BCONN_STATUS_IDLE       = 0,
    BCONN_STATUS_WAITING    = 1, // host: client 대기 중
    BCONN_STATUS_CONNECTED  = 2, // 연결됨
    BCONN_STATUS_REMOTE     = 3, // 원격 모드 활성
    BCONN_STATUS_ERROR      = 4,
} BconnStatus;

typedef struct {
    bool     is_host;
    char     host_ip[64];  // client 모드: 접속할 host IP
    uint16_t port;
    BconnEdge client_edge; // client가 host 기준 어느 방향에 있는지
} BconnConfig;

// 상태 변경 콜백 (Swift → 등록, C++ → 호출)
typedef void (*BconnStatusCallback)(BconnStatus status, const char* message, void* ctx);

// 세션 시작/중지
bool bconn_start(const BconnConfig* config, BconnStatusCallback cb, void* ctx);
void bconn_stop(void);

BconnStatus bconn_get_status(void);

// 현재 호스트의 로컬 IP 목록 (null-terminated 문자열 배열, 최대 8개)
int bconn_get_local_ips(char out[][64], int max_count);

#ifdef __cplusplus
}
#endif
