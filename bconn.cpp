#include "NetworkEngine.h"
#include <ApplicationServices/ApplicationServices.h>
#include <objc/objc-runtime.h>
#include <iostream>
#include <string>
#include <algorithm>
#include <atomic>
#include <vector>
#include <cstring>

// ── 공통 유틸 ────────────────────────────────────────────────────────

static void hide_cursor() {
    id cls = (id)objc_getClass("NSCursor");
    ((void(*)(id,SEL))objc_msgSend)(cls, sel_registerName("hide"));
}

static void show_cursor() {
    id cls = (id)objc_getClass("NSCursor");
    ((void(*)(id,SEL))objc_msgSend)(cls, sel_registerName("unhide"));
}

// ── Host 모드 ─────────────────────────────────────────────────────────

struct DisplayLayout {
    double right_edge;
    double top;
    double bottom;
    double height;
    std::vector<CGRect> displays;
};

static DisplayLayout g_layout;
static std::atomic<int32_t> g_client_width{32000};

static void refresh_layout() {
    CGDirectDisplayID ids[16];
    uint32_t count = 0;
    CGGetActiveDisplayList(16, ids, &count);

    double max_x = -1e9, min_y = 1e9, max_y = -1e9;
    for (uint32_t i = 0; i < count; ++i) {
        CGRect b = CGDisplayBounds(ids[i]);
        max_x = std::max(max_x, b.origin.x + b.size.width);
        min_y = std::min(min_y, b.origin.y);
        max_y = std::max(max_y, b.origin.y + b.size.height);
    }

    g_layout.right_edge = max_x;
    g_layout.top        = min_y;
    g_layout.bottom     = max_y;
    g_layout.height     = max_y - min_y;

    g_layout.displays.clear();
    for (uint32_t i = 0; i < count; ++i) {
        CGRect b = CGDisplayBounds(ids[i]);
        if (b.origin.x + b.size.width >= max_x - 1.0)
            g_layout.displays.push_back(b);
    }
}

static void print_layout() {
    CGDirectDisplayID ids[16];
    uint32_t count = 0;
    CGGetActiveDisplayList(16, ids, &count);

    std::cout << "=== 모니터 정보 (" << count << "개) ===\n";
    for (uint32_t i = 0; i < count; ++i) {
        CGRect b  = CGDisplayBounds(ids[i]);
        CGSize mm = CGDisplayScreenSize(ids[i]);
        bool main = (ids[i] == CGMainDisplayID());
        std::cout << "  [" << i << "] " << (main ? "(main) " : "       ")
                  << "origin=(" << (int)b.origin.x << "," << (int)b.origin.y << ")"
                  << "  size=" << (int)b.size.width << "x" << (int)b.size.height
                  << "  physical=" << (int)mm.width << "x" << (int)mm.height << "mm\n";
    }
    std::cout << "=========================\n";
}

static bool is_at_right_edge(CGPoint loc) {
    if (loc.x < g_layout.right_edge - 3.0) return false;
    for (const CGRect& b : g_layout.displays)
        if (loc.y >= b.origin.y && loc.y <= b.origin.y + b.size.height)
            return true;
    return false;
}

struct RemoteState {
    bool active      = false;
    double virtual_x = 0.0;
    double entry_y   = 0.0;
    int entry_guard  = 0;
    int return_guard = 0;
};

static RemoteState g_remote;

static void enter_remote(NetworkEngine& engine, CGPoint loc) {
    g_remote.active      = true;
    g_remote.virtual_x   = 0.0;
    g_remote.entry_y     = loc.y;
    g_remote.entry_guard = 5;

    std::cout << "[원격 모드 진입] x=" << (int)loc.x
              << " y=" << (int)loc.y
              << " client_width=" << g_client_width.load() << "\n";

    hide_cursor();
    CGAssociateMouseAndMouseCursorPosition(false);
    CGWarpMouseCursorPosition(CGPointMake(g_layout.right_edge, 99999.0));

    double y_ratio = (g_layout.height > 0)
        ? std::clamp((loc.y - g_layout.top) / g_layout.height, 0.0, 1.0)
        : 0.5;

    MousePacket pkt{};
    pkt.event_type = EVENT_SYNC_CURSOR;
    pkt.dy = static_cast<int16_t>(y_ratio * 32767.0);
    engine.send_mouse(pkt);
}

static void leave_remote(NetworkEngine& engine) {
    g_remote.active       = false;
    g_remote.return_guard = 10;

    std::cout << "[로컬 모드 복귀]\n";

    MousePacket pkt{};
    pkt.event_type = EVENT_SHOW_CURSOR;
    engine.send_mouse(pkt);

    CGWarpMouseCursorPosition(CGPointMake(g_layout.right_edge - 50.0, g_remote.entry_y));
    CGAssociateMouseAndMouseCursorPosition(true);
    show_cursor();
}

static CGEventRef mouse_callback(CGEventTapProxy, CGEventType type, CGEventRef event, void* userInfo) {
    auto& engine = *static_cast<NetworkEngine*>(userInfo);

    if (!g_remote.active) {
        CGPoint loc = CGEventGetLocation(event);

        static double last_log = 0;
        double now = CFAbsoluteTimeGetCurrent();
        if (now - last_log > 0.05) {
            std::cout << "[mouse] x=" << (int)loc.x << " y=" << (int)loc.y
                      << "  right_edge=" << (int)g_layout.right_edge << "\n";
            last_log = now;
        }

        if (g_remote.return_guard > 0) { --g_remote.return_guard; return event; }
        if (is_at_right_edge(loc)) enter_remote(engine, loc);
        return event;
    }

    bool is_click  = (type == kCGEventLeftMouseDown   || type == kCGEventLeftMouseUp  ||
                      type == kCGEventRightMouseDown  || type == kCGEventRightMouseUp ||
                      type == kCGEventOtherMouseDown  || type == kCGEventOtherMouseUp);
    bool is_scroll = (type == kCGEventScrollWheel);

    if (!is_scroll)
        CGWarpMouseCursorPosition(CGPointMake(g_layout.right_edge, 99999.0));

    if (!is_click && !is_scroll) {
        if (g_remote.entry_guard > 0) { --g_remote.entry_guard; return NULL; }

        int64_t dx = CGEventGetIntegerValueField(event, kCGMouseEventDeltaX);
        g_remote.virtual_x = std::min(g_remote.virtual_x + dx,
                                      (double)g_client_width.load(std::memory_order_relaxed));
        if (g_remote.virtual_x <= 0.0) { leave_remote(engine); return NULL; }
    }

    MousePacket pkt{};
    pkt.event_type = type;

    if (is_scroll) {
        int64_t sdx = CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis2);
        int64_t sdy = CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1);
        pkt.dx = static_cast<int16_t>(std::clamp(sdx, -32768LL, 32767LL));
        pkt.dy = static_cast<int16_t>(std::clamp(sdy, -32768LL, 32767LL));
    } else {
        int64_t dx = CGEventGetIntegerValueField(event, kCGMouseEventDeltaX);
        int64_t dy = CGEventGetIntegerValueField(event, kCGMouseEventDeltaY);
        pkt.dx = static_cast<int16_t>(std::clamp(dx, -32768LL, 32767LL));
        pkt.dy = static_cast<int16_t>(std::clamp(dy, -32768LL, 32767LL));
    }

    engine.send_mouse(pkt);
    return NULL;
}

static CGEventRef keyboard_callback(CGEventTapProxy, CGEventType type, CGEventRef event, void* userInfo) {
    if (!g_remote.active) return event;

    auto& engine = *static_cast<NetworkEngine*>(userInfo);

    CGKeyCode    keycode = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    CGEventFlags flags   = CGEventGetFlags(event);

    KeyPacket pkt{};
    pkt.keycode    = keycode;
    pkt.flags      = static_cast<uint32_t>(flags & 0xFFFFFFFF);
    pkt.event_type = (type == kCGEventKeyDown) ? EVENT_KEY_DOWN
                   : (type == kCGEventKeyUp)   ? EVENT_KEY_UP
                   :                             EVENT_FLAGS_CHANGE;
    engine.send_key(pkt);
    return NULL;
}

static int run_host(int port) {
    print_layout();
    refresh_layout();
    std::cout << "[경계] right=" << (int)g_layout.right_edge
              << " top=" << (int)g_layout.top
              << " bottom=" << (int)g_layout.bottom
              << " height=" << (int)g_layout.height << "\n";

    auto on_mouse_recv = [](const MousePacket& pkt) {
        if (pkt.event_type == EVENT_CLIENT_INFO) {
            int32_t w = ((int32_t)(uint16_t)pkt.dy << 16) | (uint16_t)pkt.dx;
            g_client_width.store(w, std::memory_order_relaxed);
            std::cout << "[← client] 화면 너비: " << w << "px\n";
        }
    };

    // host는 server 모드 (ip=nullptr)
    NetworkEngine engine(nullptr, port, on_mouse_recv);
    engine.start();

    std::cout << "Host 대기 중... (포트 " << port << ", client 접속 대기)\n";

    CGEventMask mouse_mask = CGEventMaskBit(kCGEventMouseMoved)         |
                             CGEventMaskBit(kCGEventLeftMouseDragged)   |
                             CGEventMaskBit(kCGEventRightMouseDragged)  |
                             CGEventMaskBit(kCGEventOtherMouseDragged)  |
                             CGEventMaskBit(kCGEventLeftMouseDown)      |
                             CGEventMaskBit(kCGEventLeftMouseUp)        |
                             CGEventMaskBit(kCGEventRightMouseDown)     |
                             CGEventMaskBit(kCGEventRightMouseUp)       |
                             CGEventMaskBit(kCGEventOtherMouseDown)     |
                             CGEventMaskBit(kCGEventOtherMouseUp)       |
                             CGEventMaskBit(kCGEventScrollWheel);

    CFMachPortRef mouse_tap = CGEventTapCreate(
        kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
        mouse_mask, mouse_callback, &engine);
    if (!mouse_tap) {
        std::cerr << "오류: Event Tap 권한 확인 필요 (손쉬운 사용 권한)\n";
        return 1;
    }

    CGEventMask key_mask = CGEventMaskBit(kCGEventKeyDown)    |
                           CGEventMaskBit(kCGEventKeyUp)      |
                           CGEventMaskBit(kCGEventFlagsChanged);
    CFMachPortRef key_tap = CGEventTapCreate(
        kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
        key_mask, keyboard_callback, &engine);
    if (!key_tap) {
        std::cerr << "오류: 키보드 Event Tap 생성 실패\n";
        CFRelease(mouse_tap);
        return 1;
    }

    CFRunLoopRef loop = CFRunLoopGetCurrent();

    CFRunLoopSourceRef mouse_src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, mouse_tap, 0);
    CFRunLoopAddSource(loop, mouse_src, kCFRunLoopCommonModes);
    CGEventTapEnable(mouse_tap, true);

    CFRunLoopSourceRef key_src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, key_tap, 0);
    CFRunLoopAddSource(loop, key_src, kCFRunLoopCommonModes);
    CGEventTapEnable(key_tap, true);

    std::cout << "우측 끝으로 마우스를 이동하면 클라이언트로 전환\n";
    CFRunLoopRun();

    CGAssociateMouseAndMouseCursorPosition(true);
    show_cursor();
    engine.stop();
    CFRelease(key_src);   CFRelease(key_tap);
    CFRelease(mouse_src); CFRelease(mouse_tap);
    return 0;
}

// ── Client 모드 ───────────────────────────────────────────────────────

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

static void handle_sync(const MousePacket& pkt, ScreenBounds& screen,
                        double& cur_x, double& cur_y, bool& pos_init,
                        bool& cursor_hidden, NetworkEngine& engine)
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

    // host에 화면 너비 전달
    int32_t w = (int32_t)screen.width();
    MousePacket info{};
    info.event_type = EVENT_CLIENT_INFO;
    info.dx = static_cast<int16_t>(w & 0xFFFF);
    info.dy = static_cast<int16_t>((w >> 16) & 0xFFFF);
    engine.send_mouse(info);
}

static void handle_show(bool& pos_init, bool& cursor_hidden) {
    if (!cursor_hidden) { hide_cursor(); cursor_hidden = true; }
    pos_init = false;
    std::cout << "[호스트 복귀 — 클라이언트 커서 숨김]\n";
}

static void handle_move(const MousePacket& pkt, const ScreenBounds& screen,
                        double& cur_x, double& cur_y)
{
    CGEventType etype = (CGEventType)pkt.event_type;

    if (etype == kCGEventScrollWheel) {
        CGEventRef ev = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 2,
                                                      pkt.dy, pkt.dx);
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
        return;
    }

    cur_x = std::clamp(cur_x + pkt.dx, screen.left,  screen.right  - 1.0);
    cur_y = std::clamp(cur_y + pkt.dy, screen.top,   screen.bottom - 1.0);

    CGMouseButton btn = kCGMouseButtonLeft;
    if (etype == kCGEventRightMouseDown || etype == kCGEventRightMouseUp ||
        etype == kCGEventRightMouseDragged)
        btn = kCGMouseButtonRight;
    else if (etype == kCGEventOtherMouseDown || etype == kCGEventOtherMouseUp ||
             etype == kCGEventOtherMouseDragged)
        btn = kCGMouseButtonCenter;

    CGPoint pos = CGPointMake(cur_x, cur_y);
    CGEventRef ev = CGEventCreateMouseEvent(NULL, etype, pos, btn);
    CGEventSetIntegerValueField(ev, kCGMouseEventDeltaX, pkt.dx);
    CGEventSetIntegerValueField(ev, kCGMouseEventDeltaY, pkt.dy);
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);

    static double last_log = 0;
    double now = CFAbsoluteTimeGetCurrent();
    if (now - last_log > 0.05) {
        std::cout << "[pos] x=" << (int)cur_x << " y=" << (int)cur_y
                  << "  dx=" << pkt.dx << " dy=" << pkt.dy << "\n";
        last_log = now;
    }
}

static void handle_key(const KeyPacket& pkt) {
    if (pkt.event_type == EVENT_FLAGS_CHANGE) {
        CGEventRef ev = CGEventCreateKeyboardEvent(NULL, pkt.keycode, false);
        CGEventSetFlags(ev, (CGEventFlags)pkt.flags);
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
        return;
    }

    bool key_down = (pkt.event_type == EVENT_KEY_DOWN);
    CGEventRef ev = CGEventCreateKeyboardEvent(NULL, pkt.keycode, key_down);
    CGEventSetFlags(ev, (CGEventFlags)pkt.flags);
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
}

static int run_client(const char* host_ip, int port) {
    ScreenBounds screen = get_screen_bounds();
    std::cout << "Client 구동. host=" << host_ip << ":" << port << "\n"
              << "[클라이언트 화면]"
              << " left=" << (int)screen.left << " top=" << (int)screen.top
              << " right=" << (int)screen.right << " bottom=" << (int)screen.bottom
              << " width=" << (int)screen.width() << " height=" << (int)screen.height() << "\n";

    double cur_x = 0.0, cur_y = 0.0;
    bool pos_init      = false;
    bool cursor_hidden = false;

    // client는 host ip/port로 connect
    NetworkEngine engine(host_ip, port,
        [&](const MousePacket& pkt) {
            switch (pkt.event_type) {
            case EVENT_SYNC_CURSOR:
                handle_sync(pkt, screen, cur_x, cur_y, pos_init, cursor_hidden, engine);
                break;
            case EVENT_SHOW_CURSOR:
                handle_show(pos_init, cursor_hidden);
                break;
            default:
                if (pos_init) handle_move(pkt, screen, cur_x, cur_y);
                break;
            }
        },
        [&](const KeyPacket& pkt) {
            if (pos_init) handle_key(pkt);
        }
    );
    engine.start();

    // recv 루프는 NetworkEngine 내부에서 돌므로 여기선 대기만
    std::cout << "host 연결 완료. 대기 중...\n";
    pause();

    engine.stop();
    return 0;
}

// ── 진입점 ────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cerr << "사용법:\n"
              << "  " << prog << " host [port]              (기본 포트: 24900)\n"
              << "  " << prog << " client <host_ip> [port]  (기본 포트: 24900)\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    std::string mode = argv[1];

    if (mode == "host") {
        int port = (argc >= 3) ? std::stoi(argv[2]) : 24900;
        return run_host(port);
    }

    if (mode == "client") {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        int port = (argc >= 4) ? std::stoi(argv[3]) : 24900;
        return run_client(argv[2], port);
    }

    print_usage(argv[0]);
    return 1;
}
