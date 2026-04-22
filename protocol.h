#pragma once
#include <stdint.h>
#include <stddef.h>

// 이벤트 타입 상수 (kCGEventType 범위와 겹치지 않도록 9900번대 사용)
static const uint32_t EVENT_SYNC_CURSOR  = 9999; // host→client: 진입 Y 비율 전달
static const uint32_t EVENT_SHOW_CURSOR  = 9998; // host→client: 호스트 복귀 알림
static const uint32_t EVENT_CLIENT_INFO  = 9996; // client→host: 화면 너비 전달
static const uint32_t EVENT_KEY_DOWN     = 9001; // host→client: 키 눌림
static const uint32_t EVENT_KEY_UP       = 9002; // host→client: 키 뗌
static const uint32_t EVENT_FLAGS_CHANGE = 9003; // host→client: modifier 키 변경

// 패킷 종류 구분자
static const uint8_t PACKET_MOUSE    = 0;
static const uint8_t PACKET_KEYBOARD = 1;

#pragma pack(push, 1)

struct MousePacket {
    uint8_t  packet_type = PACKET_MOUSE;
    int16_t  dx;
    int16_t  dy;
    uint32_t event_type;
};

// CGKeyCode = uint16_t, CGEventFlags = uint64_t (하위 32bit만 사용)
struct KeyPacket {
    uint8_t  packet_type = PACKET_KEYBOARD;
    uint16_t keycode;
    uint32_t flags;       // modifier 플래그 (CGEventFlags 하위 32bit)
    uint32_t event_type;  // EVENT_KEY_DOWN / EVENT_KEY_UP / EVENT_FLAGS_CHANGE
};

#pragma pack(pop)

// 두 패킷 중 큰 쪽에 맞춘 공용 버퍼 크기
static_assert(sizeof(MousePacket) == 9,  "MousePacket size changed");
static_assert(sizeof(KeyPacket)   == 11, "KeyPacket size changed");
static const size_t MAX_PACKET_SIZE = sizeof(KeyPacket);
