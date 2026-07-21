#pragma once
#include <os/log.h>
#include <stdarg.h>
#include <stdio.h>

#define TETHER_LOG_SUBSYSTEM "com.tether"

// TetherLogLevel 은 tether_core.h 에도 정의된다.
// 두 헤더가 함께 include될 때 중복 정의를 막기 위해 동일한 guard를 사용한다.
#ifndef TETHER_LOG_LEVEL_DEFINED
#define TETHER_LOG_LEVEL_DEFINED
typedef enum { TETHER_LOG_DEBUG = 0, TETHER_LOG_INFO = 1, TETHER_LOG_ERROR = 2 } TetherLogLevel;
typedef void (*TetherLogCallback)(TetherLogLevel level, const char* category, const char* message, void* ctx);
#endif
typedef TetherLogCallback TetherLogCb;

#ifdef __cplusplus
extern "C" {
#endif
extern TetherLogCb g_log_cb;
extern void*      g_log_ctx;
#ifdef __cplusplus
}
#endif

// os_log_t 객체는 생성 비용이 있으므로 카테고리별로 한 번만 만들어 재사용한다.
static inline os_log_t tether_log_for_cat(const char* cat) {
    // 카테고리 상수는 4개로 고정 — 선형 탐색으로 충분
    static os_log_t logs[4] = {};
    static const char* keys[4] = { "auth", "network", "cursor", "event" };
    for (int i = 0; i < 4; ++i) {
        if (keys[i] == cat) {  // 포인터 동일성 비교 (매크로 리터럴이므로 항상 성립)
            if (!logs[i]) logs[i] = os_log_create(TETHER_LOG_SUBSYSTEM, cat);
            return logs[i];
        }
    }
    // 알 수 없는 카테고리는 매번 생성 (드문 경우)
    return os_log_create(TETHER_LOG_SUBSYSTEM, cat);
}

static inline void tether_log_emit(TetherLogLevel level, const char* cat,
                                   const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    os_log_t log = tether_log_for_cat(cat);
    switch (level) {
    case TETHER_LOG_DEBUG: os_log_debug(log, "%{public}s", buf); break;
    case TETHER_LOG_INFO:  os_log_info (log, "%{public}s", buf); break;
    case TETHER_LOG_ERROR: os_log_error(log, "%{public}s", buf); break;
    }

    if (g_log_cb) g_log_cb(level, cat, buf, g_log_ctx);
}

// DEBUG 빌드에서만 TETHER_DEBUG 활성화 — Release에서는 완전히 제거된다
#ifdef DEBUG
#define TETHER_DEBUG(cat, fmt, ...) tether_log_emit(TETHER_LOG_DEBUG, cat, fmt, ##__VA_ARGS__)
#else
#define TETHER_DEBUG(cat, fmt, ...) ((void)0)
#endif

#define TETHER_INFO(cat, fmt, ...)  tether_log_emit(TETHER_LOG_INFO,  cat, fmt, ##__VA_ARGS__)
#define TETHER_ERROR(cat, fmt, ...) tether_log_emit(TETHER_LOG_ERROR, cat, fmt, ##__VA_ARGS__)

#define CAT_AUTH    "auth"
#define CAT_NETWORK "network"
#define CAT_CURSOR  "cursor"
#define CAT_EVENT   "event"
