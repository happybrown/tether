#pragma once
#include <stdint.h>

// 이벤트 타입 상수 (kCGEventType 범위와 겹치지 않도록 9900번대 사용)
static const uint32_t EVENT_SYNC_CURSOR = 9999; // host→client: 진입 Y 비율 전달
static const uint32_t EVENT_SHOW_CURSOR = 9998; // host→client: 호스트 복귀 알림
static const uint32_t EVENT_CLIENT_INFO = 9996; // client→host: 화면 너비 전달

#pragma pack(push, 1)
struct MousePacket {
    int16_t  dx;
    int16_t  dy;
    uint32_t event_type;
};
#pragma pack(pop)
