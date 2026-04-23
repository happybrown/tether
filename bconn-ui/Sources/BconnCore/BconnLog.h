#pragma once
#include <os/log.h>
#include <stdarg.h>
#include <stdio.h>

#define BCONN_LOG_SUBSYSTEM "com.bconn"

// BconnLogLevel 은 bconn_core.h 에도 정의된다.
// 두 헤더가 함께 include될 때 중복 정의를 막기 위해 동일한 guard를 사용한다.
#ifndef BCONN_LOG_LEVEL_DEFINED
#define BCONN_LOG_LEVEL_DEFINED
typedef enum { BCONN_LOG_DEBUG = 0, BCONN_LOG_INFO = 1, BCONN_LOG_ERROR = 2 } BconnLogLevel;
typedef void (*BconnLogCallback)(BconnLogLevel level, const char* category, const char* message, void* ctx);
#endif
typedef BconnLogCallback BconnLogCb;

#ifdef __cplusplus
extern "C" {
#endif
extern BconnLogCb g_log_cb;
extern void*      g_log_ctx;
#ifdef __cplusplus
}
#endif

static inline void bconn_log_emit(BconnLogLevel level, const char* cat,
                                   const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    os_log_t log = os_log_create(BCONN_LOG_SUBSYSTEM, cat);
    switch (level) {
    case BCONN_LOG_DEBUG: os_log_debug(log, "%{public}s", buf); break;
    case BCONN_LOG_INFO:  os_log_info (log, "%{public}s", buf); break;
    case BCONN_LOG_ERROR: os_log_error(log, "%{public}s", buf); break;
    }

    if (g_log_cb) g_log_cb(level, cat, buf, g_log_ctx);
}

#define BCONN_DEBUG(cat, fmt, ...) bconn_log_emit(BCONN_LOG_DEBUG, cat, fmt, ##__VA_ARGS__)
#define BCONN_INFO(cat, fmt, ...)  bconn_log_emit(BCONN_LOG_INFO,  cat, fmt, ##__VA_ARGS__)
#define BCONN_ERROR(cat, fmt, ...) bconn_log_emit(BCONN_LOG_ERROR, cat, fmt, ##__VA_ARGS__)

#define CAT_AUTH    "auth"
#define CAT_NETWORK "network"
#define CAT_CURSOR  "cursor"
#define CAT_EVENT   "event"
