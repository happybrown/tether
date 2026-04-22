#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <algorithm>
#include <ApplicationServices/ApplicationServices.h>

#define EVENT_SYNC_CURSOR 9999
#define EVENT_SHOW_CURSOR 9998
#define EVENT_CLIENT_INFO 9996

#pragma pack(push, 1)
struct MousePacket {
    uint64_t timestamp;
    int16_t dx;
    int16_t dy;
    uint32_t event_type;
};
#pragma pack(pop)

struct ScreenBounds {
    double left, top, right, bottom;
};

ScreenBounds get_all_screen_bounds() {
    CGDirectDisplayID displays[16];
    uint32_t count = 0;
    CGGetActiveDisplayList(16, displays, &count);

    ScreenBounds b{999999, 999999, -999999, -999999};
    for (uint32_t i = 0; i < count; ++i) {
        CGRect r = CGDisplayBounds(displays[i]);
        if (r.origin.x < b.left)                   b.left   = r.origin.x;
        if (r.origin.y < b.top)                    b.top    = r.origin.y;
        if (r.origin.x + r.size.width  > b.right)  b.right  = r.origin.x + r.size.width;
        if (r.origin.y + r.size.height > b.bottom) b.bottom = r.origin.y + r.size.height;
    }
    return b;
}

void set_cursor_visibility(bool visible) {
    CGDirectDisplayID displays[16];
    uint32_t count = 0;
    CGGetActiveDisplayList(16, displays, &count);
    for (uint32_t i = 0; i < count; ++i) {
        if (visible) CGDisplayShowCursor(displays[i]);
        else         CGDisplayHideCursor(displays[i]);
    }
}

int main(int argc, char* argv[]) {
    int listen_port = 9000;
    if (argc >= 2) listen_port = std::atoi(argv[1]);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { std::cerr << "소켓 생성 실패" << std::endl; return 1; }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(listen_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "바인딩 실패" << std::endl;
        close(sock);
        return 1;
    }

    int reply_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int reply_port = listen_port + 100;

    ScreenBounds screen = get_all_screen_bounds();
    double screen_width  = screen.right  - screen.left;
    double screen_height = screen.bottom - screen.top;

    std::cout << "Client 구동 완료. 포트 " << listen_port << " 대기 중..." << std::endl;
    std::cout << "[클라이언트 화면]"
              << " left="   << (int)screen.left
              << " top="    << (int)screen.top
              << " right="  << (int)screen.right
              << " bottom=" << (int)screen.bottom
              << " width="  << (int)screen_width
              << " height=" << (int)screen_height << std::endl;

    MousePacket packet;
    CGEventTapLocation tap_location = kCGHIDEventTap;

    double cur_x = 0.0, cur_y = 0.0;
    bool pos_initialized = false;
    bool cursor_hidden = false;

    sockaddr_in host_addr{};
    bool host_addr_known = false;

    while (true) {
        sockaddr_in sender{};
        socklen_t sender_len = sizeof(sender);
        ssize_t received = recvfrom(sock, &packet, sizeof(packet), 0,
                                    (sockaddr*)&sender, &sender_len);
        if (received != (ssize_t)sizeof(MousePacket)) continue;

        if (!host_addr_known) {
            host_addr = sender;
            host_addr.sin_port = htons(reply_port);
            host_addr_known = true;
            std::cout << "[host 주소 확인됨, reply port=" << reply_port << "]" << std::endl;
        }

        // ── 동기화 패킷 ──────────────────────────────────────────
        if (packet.event_type == EVENT_SYNC_CURSOR) {
            // 화면 경계 갱신
            screen        = get_all_screen_bounds();
            screen_width  = screen.right  - screen.left;
            screen_height = screen.bottom - screen.top;

            // Y 비율을 전체 화면 높이 기준으로 계산
            double y_ratio  = static_cast<double>(packet.dy) / 32767.0;
            double target_y = screen.top + y_ratio * screen_height;

            cur_x = screen.left + 2.0;
            cur_y = target_y;
            pos_initialized = true;

            CGWarpMouseCursorPosition(CGPointMake(cur_x, cur_y));
            // 원격 제어 시작: 커서 표시 (이미 숨겨져 있으면 먼저 복원 후 다시 숨기지 않음)
            if (cursor_hidden) {
                set_cursor_visibility(true);
                cursor_hidden = false;
            }

            std::cout << "[동기화] y_ratio=" << y_ratio
                      << " target_y=" << (int)target_y
                      << " width=" << (int)screen_width
                      << " height=" << (int)screen_height << std::endl;

            // host에 화면 너비를 알림 — int32_t로 전송해 부호/범위 문제 회피
            if (host_addr_known) {
                MousePacket info{};
                info.event_type = EVENT_CLIENT_INFO;
                // screen_width를 dx(int16_t)에 넣으면 32767 초과 시 깨지므로
                // dy에 상위 바이트, dx에 하위 바이트로 나눠 담음
                int32_t w = (int32_t)screen_width;
                info.dx = static_cast<int16_t>(w & 0xFFFF);
                info.dy = static_cast<int16_t>((w >> 16) & 0xFFFF);
                sendto(reply_sock, &info, sizeof(info), 0,
                       (sockaddr*)&host_addr, sizeof(host_addr));
            }
            continue;
        }

        // ── 호스트 복귀 패킷: 클라이언트 커서 숨김 ──────────────
        if (packet.event_type == EVENT_SHOW_CURSOR) {
            if (!cursor_hidden) {
                set_cursor_visibility(false);
                cursor_hidden = true;
            }
            pos_initialized = false;
            std::cout << "[호스트 복귀 — 클라이언트 커서 숨김]" << std::endl;
            continue;
        }

        if (!pos_initialized) continue;

        // ── 일반 이동/클릭 처리 ──────────────────────────────────
        double new_x = cur_x + packet.dx;
        double new_y = cur_y + packet.dy;

        // 화면 경계 클램핑
        if (new_x < screen.left)       new_x = screen.left;
        if (new_x > screen.right  - 1) new_x = screen.right  - 1;
        if (new_y < screen.top)        new_y = screen.top;
        if (new_y > screen.bottom - 1) new_y = screen.bottom - 1;

        cur_x = new_x;
        cur_y = new_y;

        CGPoint new_pos = CGPointMake(cur_x, cur_y);

        CGMouseButton buttonType = kCGMouseButtonLeft;
        if (packet.event_type == kCGEventRightMouseDown ||
            packet.event_type == kCGEventRightMouseUp   ||
            packet.event_type == kCGEventRightMouseDragged) {
            buttonType = kCGMouseButtonRight;
        }

        CGEventRef move_event = CGEventCreateMouseEvent(
            NULL, (CGEventType)packet.event_type, new_pos, buttonType);
        CGEventSetIntegerValueField(move_event, kCGMouseEventDeltaX, packet.dx);
        CGEventSetIntegerValueField(move_event, kCGMouseEventDeltaY, packet.dy);
        CGEventPost(tap_location, move_event);
        CFRelease(move_event);

        // 이동 이벤트 디버그 (50ms 간격)
        static double last_log_time = 0;
        double now = CFAbsoluteTimeGetCurrent();
        if (now - last_log_time > 0.05) {
            std::cout << "[pos] x=" << (int)cur_x << " y=" << (int)cur_y
                      << "  dx=" << packet.dx << " dy=" << packet.dy << std::endl;
            last_log_time = now;
        }
    }

    close(sock);
    close(reply_sock);
    return 0;
}
