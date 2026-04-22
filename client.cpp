#include "protocol.h"
#include <ApplicationServices/ApplicationServices.h>
#include <objc/objc-runtime.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <algorithm>

// NSCursor — CGDisplayHideCursor는 CLI 프로세스에서 무시됨
static void hide_cursor() {
    id cls = (id)objc_getClass("NSCursor");
    ((void(*)(id,SEL))objc_msgSend)(cls, sel_registerName("hide"));
}

static void show_cursor() {
    id cls = (id)objc_getClass("NSCursor");
    ((void(*)(id,SEL))objc_msgSend)(cls, sel_registerName("unhide"));
}

struct ScreenBounds {
    double left, top, right, bottom;

    double width()  const { return right - left; }
    double height() const { return bottom - top; }
};

static ScreenBounds get_screen_bounds() {
    CGDirectDisplayID ids[16];
    uint32_t count = 0;
    CGGetActiveDisplayList(16, ids, &count);

    ScreenBounds b{1e9, 1e9, -1e9, -1e9};
    for (uint32_t i = 0; i < count; ++i) {
        CGRect r = CGDisplayBounds(ids[i]);
        b.left   = std::min(b.left,   r.origin.x);
        b.top    = std::min(b.top,    r.origin.y);
        b.right  = std::max(b.right,  r.origin.x + r.size.width);
        b.bottom = std::max(b.bottom, r.origin.y + r.size.height);
    }
    return b;
}

// ── 패킷 처리 ────────────────────────────────────────────────────────

static void handle_sync(const MousePacket& pkt, ScreenBounds& screen,
                        double& cur_x, double& cur_y, bool& pos_init,
                        bool& cursor_hidden, int reply_sock, const sockaddr_in& host_addr)
{
    screen = get_screen_bounds();

    double y_ratio = static_cast<double>(pkt.dy) / 32767.0;
    cur_x    = screen.left + 2.0;
    cur_y    = screen.top + y_ratio * screen.height();
    pos_init = true;

    CGWarpMouseCursorPosition(CGPointMake(cur_x, cur_y));

    if (cursor_hidden) { show_cursor(); cursor_hidden = false; }

    std::cout << "[동기화] y_ratio=" << y_ratio
              << " target_y=" << (int)cur_y
              << " width=" << (int)screen.width()
              << " height=" << (int)screen.height() << "\n";

    // host에 화면 너비 전달 (int32_t를 dx/dy에 분할)
    int32_t w = (int32_t)screen.width();
    MousePacket info{};
    info.event_type = EVENT_CLIENT_INFO;
    info.dx = static_cast<int16_t>(w & 0xFFFF);
    info.dy = static_cast<int16_t>((w >> 16) & 0xFFFF);
    sendto(reply_sock, &info, sizeof(info), 0, (sockaddr*)&host_addr, sizeof(host_addr));
}

static void handle_show(bool& pos_init, bool& cursor_hidden) {
    if (!cursor_hidden) { hide_cursor(); cursor_hidden = true; }
    pos_init = false;
    std::cout << "[호스트 복귀 — 클라이언트 커서 숨김]\n";
}

static void handle_move(const MousePacket& pkt, const ScreenBounds& screen,
                        double& cur_x, double& cur_y)
{
    cur_x = std::clamp(cur_x + pkt.dx, screen.left,  screen.right  - 1.0);
    cur_y = std::clamp(cur_y + pkt.dy, screen.top,   screen.bottom - 1.0);

    CGMouseButton btn = kCGMouseButtonLeft;
    if (pkt.event_type == kCGEventRightMouseDown ||
        pkt.event_type == kCGEventRightMouseUp   ||
        pkt.event_type == kCGEventRightMouseDragged)
        btn = kCGMouseButtonRight;

    CGPoint pos = CGPointMake(cur_x, cur_y);
    CGEventRef ev = CGEventCreateMouseEvent(NULL, (CGEventType)pkt.event_type, pos, btn);
    CGEventSetIntegerValueField(ev, kCGMouseEventDeltaX, pkt.dx);
    CGEventSetIntegerValueField(ev, kCGMouseEventDeltaY, pkt.dy);
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);

    // 디버그 (50ms 간격)
    static double last_log = 0;
    double now = CFAbsoluteTimeGetCurrent();
    if (now - last_log > 0.05) {
        std::cout << "[pos] x=" << (int)cur_x << " y=" << (int)cur_y
                  << "  dx=" << pkt.dx << " dy=" << pkt.dy << "\n";
        last_log = now;
    }
}

// ── main ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    int listen_port = (argc >= 2) ? std::atoi(argv[1]) : 9000;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { std::cerr << "소켓 생성 실패\n"; return 1; }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(listen_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "바인딩 실패\n";
        close(sock);
        return 1;
    }

    int reply_sock = socket(AF_INET, SOCK_DGRAM, 0);

    ScreenBounds screen = get_screen_bounds();
    std::cout << "Client 구동 완료. 포트 " << listen_port << " 대기 중...\n"
              << "[클라이언트 화면]"
              << " left=" << (int)screen.left << " top=" << (int)screen.top
              << " right=" << (int)screen.right << " bottom=" << (int)screen.bottom
              << " width=" << (int)screen.width() << " height=" << (int)screen.height() << "\n";

    double cur_x = 0.0, cur_y = 0.0;
    bool pos_init     = false;
    bool cursor_hidden = false;

    sockaddr_in host_addr{};
    bool host_addr_known = false;

    MousePacket packet;

    while (true) {
        sockaddr_in sender{};
        socklen_t sender_len = sizeof(sender);
        ssize_t n = recvfrom(sock, &packet, sizeof(packet), 0,
                             (sockaddr*)&sender, &sender_len);
        if (n != (ssize_t)sizeof(MousePacket)) continue;

        if (!host_addr_known) {
            host_addr           = sender;
            host_addr.sin_port  = htons(listen_port + 100);
            host_addr_known     = true;
            std::cout << "[host 주소 확인됨, reply port=" << listen_port + 100 << "]\n";
        }

        switch (packet.event_type) {
        case EVENT_SYNC_CURSOR:
            handle_sync(packet, screen, cur_x, cur_y, pos_init,
                        cursor_hidden, reply_sock, host_addr);
            break;
        case EVENT_SHOW_CURSOR:
            handle_show(pos_init, cursor_hidden);
            break;
        default:
            if (pos_init)
                handle_move(packet, screen, cur_x, cur_y);
            break;
        }
    }

    close(sock);
    close(reply_sock);
    return 0;
}
