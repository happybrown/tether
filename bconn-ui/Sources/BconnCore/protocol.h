#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <CommonCrypto/CommonHMAC.h>

// 이벤트 타입 상수 (kCGEventType 범위와 겹치지 않도록 9900번대 사용)
static const uint32_t EVENT_SYNC_CURSOR  = 9999; // host→client: 진입 Y 비율 전달
static const uint32_t EVENT_SHOW_CURSOR  = 9998; // host→client: 호스트 복귀 알림
static const uint32_t EVENT_LEAVE_REMOTE = 9997; // client→host: 반대편 경계 이탈 → 복귀 요청(dx=edge, dy=복귀 비율)
static const uint32_t EVENT_CLIENT_INFO  = 9996; // (deprecated) PACKET_CLIENT_INFO으로 대체됨
static const uint32_t EVENT_KEY_DOWN     = 9001; // host→client: 키 눌림
static const uint32_t EVENT_KEY_UP       = 9002; // host→client: 키 뗌
static const uint32_t EVENT_FLAGS_CHANGE = 9003; // host→client: modifier 키 변경

// 패킷 종류 구분자
static const uint8_t PACKET_MOUSE       = 0;
static const uint8_t PACKET_KEYBOARD    = 1;
static const uint8_t PACKET_HELLO       = 2; // client → host: 인증 요청
static const uint8_t PACKET_AUTH        = 3; // host → client: 인증 응답
static const uint8_t PACKET_PING        = 4; // client → host: 연결 유지 확인
static const uint8_t PACKET_PONG        = 5; // host → client: PING 응답
static const uint8_t PACKET_CLIPBOARD   = 6; // 양방향: 클립보드 텍스트 동기화
static const uint8_t PACKET_CLIENT_INFO = 7; // client → host: 화면 크기 전달

// 클립보드 패킷 최대 페이로드 크기
static const uint32_t CLIPBOARD_MAX_BYTES = 60 * 1024;
// 단일 전송 청크 크기 (MTU 1500 - IP 20 - UDP 8 - 패킷 헤더 9 = 1463, 안전 마진 적용)
static const uint32_t CLIPBOARD_CHUNK_SIZE = 1400;

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

// client → host: 화면 크기 전달 (단일 패킷으로 너비/높이 동시 전송)
struct ClientInfoPacket {
    uint8_t  packet_type;   // = PACKET_CLIENT_INFO
    int32_t  width;
    int32_t  height;
};

#pragma pack(pop)

static_assert(sizeof(MousePacket)      == 9,  "MousePacket size changed");
static_assert(sizeof(KeyPacket)        == 11, "KeyPacket size changed");
static_assert(sizeof(HelloPacket)      == 64, "HelloPacket size changed");
static_assert(sizeof(AuthAckPacket)    == 34, "AuthAckPacket size changed");
static_assert(sizeof(ClientInfoPacket) == 9,  "ClientInfoPacket size changed");

// recv_loop 버퍼: 클립보드 청크가 가장 크므로 헤더(5B) + 페이로드로 산정
static const size_t MAX_PACKET_SIZE = 5 + CLIPBOARD_CHUNK_SIZE;

// HMAC-SHA1(psk, hostname[32] || timestamp_s LE) → 20 bytes
// bconn_core.cpp 와 테스트 코드가 공유하는 단일 구현
static inline void compute_hmac(const char* psk, const char hostname32[32],
                                 int64_t ts, uint8_t out[20]) {
    uint8_t data[40];
    memcpy(data,      hostname32, 32);
    memcpy(data + 32, &ts,        8);
    CCHmac(kCCHmacAlgSHA1, psk, strlen(psk), data, sizeof(data), out);
}
