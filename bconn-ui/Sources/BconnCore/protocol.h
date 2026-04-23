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
static const uint8_t PACKET_HELLO    = 2; // client → host: 인증 요청
static const uint8_t PACKET_AUTH     = 3; // host → client: 인증 응답

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

// HMAC-SHA1(psk, hostname[32] || timestamp_s) 인증 요청
// client → host, heartbeat 겸용 (5초마다 재전송)
struct HelloPacket {
    uint8_t  packet_type;   // = PACKET_HELLO
    char     hostname[32];  // 클라이언트 호스트명 (null-terminated)
    int64_t  timestamp_s;   // Unix time (재전송 공격 방지)
    uint8_t  hmac[20];      // HMAC-SHA1(psk, hostname[32] || timestamp_s LE)
    uint8_t  _pad[3];
};

// host → client: 인증 결과
struct AuthAckPacket {
    uint8_t  packet_type;       // = PACKET_AUTH
    uint8_t  accepted;          // 1=OK, 0=거부
    char     host_hostname[32]; // host 호스트명 (표시용)
};

#pragma pack(pop)

static_assert(sizeof(MousePacket)  == 9,  "MousePacket size changed");
static_assert(sizeof(KeyPacket)    == 11, "KeyPacket size changed");
static_assert(sizeof(HelloPacket)  == 64, "HelloPacket size changed");
static_assert(sizeof(AuthAckPacket) == 34, "AuthAckPacket size changed");
static const size_t MAX_PACKET_SIZE = sizeof(HelloPacket); // 64 bytes
