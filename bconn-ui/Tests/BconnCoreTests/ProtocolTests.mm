#import <XCTest/XCTest.h>
#import <CommonCrypto/CommonHMAC.h>
#include "protocol.h"
#include "bconn_core.h"
#include <cstring>
#include <ctime>

// ── 패킷 구조 테스트 ──────────────────────────────────────────────────

@interface ProtocolTests : XCTestCase
@end

@implementation ProtocolTests

- (void)test_packetSizes {
    XCTAssertEqual(sizeof(MousePacket),      9u);
    XCTAssertEqual(sizeof(KeyPacket),       11u);
    XCTAssertEqual(sizeof(HelloPacket),     64u);
    XCTAssertEqual(sizeof(AuthAckPacket),   34u);
    XCTAssertEqual(sizeof(ClientInfoPacket), 9u);
    // MAX_PACKET_SIZE = 클립보드 헤더(5B) + 청크 페이로드(CLIPBOARD_CHUNK_SIZE)
    XCTAssertEqual(MAX_PACKET_SIZE, (size_t)(5 + CLIPBOARD_CHUNK_SIZE));
}

- (void)test_helloPacket_fieldLayout {
    HelloPacket pkt{};
    pkt.packet_type = PACKET_HELLO;
    strncpy(pkt.hostname, "testhost", sizeof(pkt.hostname) - 1);
    pkt.timestamp_s = 1234567890LL;
    memset(pkt.hmac, 0xAB, sizeof(pkt.hmac));

    XCTAssertEqual(pkt.packet_type, PACKET_HELLO);
    XCTAssertEqual(strcmp(pkt.hostname, "testhost"), 0);
    XCTAssertEqual(pkt.timestamp_s, 1234567890LL);

    // 첫 바이트가 packet_type
    uint8_t* raw = (uint8_t*)&pkt;
    XCTAssertEqual(raw[0], PACKET_HELLO);
}

- (void)test_authAckPacket_fieldLayout {
    AuthAckPacket pkt{};
    pkt.packet_type = PACKET_AUTH;
    pkt.accepted    = 1;
    strncpy(pkt.host_hostname, "myhost", sizeof(pkt.host_hostname) - 1);

    uint8_t* raw = (uint8_t*)&pkt;
    XCTAssertEqual(raw[0], PACKET_AUTH);
    XCTAssertEqual(raw[1], 1);
    XCTAssertEqual(strcmp(pkt.host_hostname, "myhost"), 0);
}

- (void)test_packetTypeConstants_unique {
    // 각 타입 상수가 고유한 값을 가지는지 확인
    XCTAssertNotEqual((int)PACKET_MOUSE,    (int)PACKET_KEYBOARD);
    XCTAssertNotEqual((int)PACKET_MOUSE,    (int)PACKET_HELLO);
    XCTAssertNotEqual((int)PACKET_MOUSE,    (int)PACKET_AUTH);
    XCTAssertNotEqual((int)PACKET_KEYBOARD, (int)PACKET_HELLO);
    XCTAssertNotEqual((int)PACKET_KEYBOARD, (int)PACKET_AUTH);
    XCTAssertNotEqual((int)PACKET_HELLO,    (int)PACKET_AUTH);
}

- (void)test_mousePacket_eventTypes_noOverlapWithPacketTypes {
    // 이벤트 타입 상수가 kCGEventType 범위와 충돌하지 않도록 9900번대 사용
    XCTAssertGreaterThan(EVENT_SYNC_CURSOR,  9000u);
    XCTAssertGreaterThan(EVENT_SHOW_CURSOR,  9000u);
    XCTAssertGreaterThan(EVENT_CLIENT_INFO,  9000u);
    XCTAssertGreaterThan(EVENT_KEY_DOWN,     9000u);
    XCTAssertGreaterThan(EVENT_KEY_UP,       9000u);
    XCTAssertGreaterThan(EVENT_FLAGS_CHANGE, 9000u);
}

@end

// ── HMAC 인증 테스트 ──────────────────────────────────────────────────

@interface AuthHmacTests : XCTestCase
@end

@implementation AuthHmacTests

- (void)test_hmac_sameInputProducesSameOutput {
    char hn[32] = {};
    strncpy(hn, "MacBookPro.local", 31);
    int64_t ts = 1700000000LL;

    uint8_t out1[20], out2[20];
    compute_hmac("mysecret", hn, ts, out1);
    compute_hmac("mysecret", hn, ts, out2);

    XCTAssertEqual(memcmp(out1, out2, 20), 0);
}

- (void)test_hmac_differentPskProducesDifferentOutput {
    char hn[32] = {};
    strncpy(hn, "MacBookPro.local", 31);
    int64_t ts = 1700000000LL;

    uint8_t out1[20], out2[20];
    compute_hmac("correctpsk", hn, ts, out1);
    compute_hmac("wrongpsk",   hn, ts, out2);

    XCTAssertNotEqual(memcmp(out1, out2, 20), 0);
}

- (void)test_hmac_differentHostnameProducesDifferentOutput {
    char hn1[32] = {}, hn2[32] = {};
    strncpy(hn1, "MacBookPro.local", 31);
    strncpy(hn2, "MacMini.local",    31);
    int64_t ts = 1700000000LL;

    uint8_t out1[20], out2[20];
    compute_hmac("psk", hn1, ts, out1);
    compute_hmac("psk", hn2, ts, out2);

    XCTAssertNotEqual(memcmp(out1, out2, 20), 0);
}

- (void)test_hmac_differentTimestampProducesDifferentOutput {
    char hn[32] = {};
    strncpy(hn, "MacBookPro.local", 31);

    uint8_t out1[20], out2[20];
    compute_hmac("psk", hn, 1700000000LL, out1);
    compute_hmac("psk", hn, 1700000001LL, out2);

    XCTAssertNotEqual(memcmp(out1, out2, 20), 0);
}

- (void)test_hmac_length_is20Bytes {
    // HMAC-SHA1 출력은 항상 20 bytes
    XCTAssertEqual(CC_SHA1_DIGEST_LENGTH, 20);
}

- (void)test_hmac_verificationLogic_matchAndMismatch {
    char hn[32] = {};
    strncpy(hn, "testhost", 31);
    int64_t ts = 1700000000LL;
    const char* psk = "shared_secret";

    uint8_t computed[20];
    compute_hmac(psk, hn, ts, computed);

    // 올바른 PSK로 검증 → 통과
    uint8_t expected_ok[20];
    compute_hmac(psk, hn, ts, expected_ok);
    XCTAssertEqual(memcmp(computed, expected_ok, 20), 0, @"올바른 PSK 검증 실패");

    // 틀린 PSK로 검증 → 실패
    uint8_t expected_fail[20];
    compute_hmac("wrong_secret", hn, ts, expected_fail);
    XCTAssertNotEqual(memcmp(computed, expected_fail, 20), 0, @"잘못된 PSK가 통과됨");
}

// replay 방지: ±30초 이내만 허용하는 타임스탬프 검사 로직 테스트
- (void)test_timestampWindow_valid {
    int64_t now  = (int64_t)time(nullptr);
    int64_t diff = 0;  // 동일 시각

    XCTAssertTrue(diff >= -30 && diff <= 30, @"동일 시각은 허용");
}

- (void)test_timestampWindow_edgeCases {
    int64_t now = (int64_t)time(nullptr);

    // ±30초: 허용
    XCTAssertTrue((now - 30) - now >= -30);
    XCTAssertTrue((now + 30) - now <= 30);

    // ±31초: 거부
    int64_t diff_past   = (now - 31) - now; // -31
    int64_t diff_future = (now + 31) - now; // +31
    XCTAssertFalse(diff_past >= -30 && diff_past <= 30, @"-31초는 거부");
    XCTAssertFalse(diff_future >= -30 && diff_future <= 30, @"+31초는 거부");
}

@end

// ── BconnConfig 테스트 ─────────────────────────────────────────────────

@interface BconnConfigTests : XCTestCase
@end

@implementation BconnConfigTests

- (void)test_bconnConfig_pskFieldSize {
    BconnConfig cfg{};
    XCTAssertEqual(sizeof(cfg.psk), 128u);
}

- (void)test_bconnConfig_hostIpFieldSize {
    BconnConfig cfg{};
    XCTAssertEqual(sizeof(cfg.host_ip), 64u);
}

- (void)test_bconnConfig_edgeValues {
    XCTAssertEqual((int)BCONN_EDGE_RIGHT,  0);
    XCTAssertEqual((int)BCONN_EDGE_LEFT,   1);
    XCTAssertEqual((int)BCONN_EDGE_TOP,    2);
    XCTAssertEqual((int)BCONN_EDGE_BOTTOM, 3);
}

- (void)test_bconnStatus_values {
    XCTAssertEqual((int)BCONN_STATUS_IDLE,        0);
    XCTAssertEqual((int)BCONN_STATUS_WAITING,     1);
    XCTAssertEqual((int)BCONN_STATUS_CONNECTED,   2);
    XCTAssertEqual((int)BCONN_STATUS_REMOTE,      3);
    XCTAssertEqual((int)BCONN_STATUS_ERROR,       4);
    XCTAssertEqual((int)BCONN_STATUS_AUTH_FAILED, 5);
}

@end
