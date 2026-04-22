#include "NetworkEngine.h"
#include <ApplicationServices/ApplicationServices.h>
#include <objc/objc-runtime.h>
#include <iostream>
#include <string>
#include <algorithm>
#include <atomic>
#include <vector>

// NSCursor hide/unhide — CGDisplayHideCursor는 CLI 프로세스에서 무시됨
static void hide_cursor() {
    id cls = (id)objc_getClass("NSCursor");
    ((void(*)(id,SEL))objc_msgSend)(cls, sel_registerName("hide"));
}

static void show_cursor() {
    id cls = (id)objc_getClass("NSCursor");
    ((void(*)(id,SEL))objc_msgSend)(cls, sel_registerName("unhide"));
}

// ── 디스플레이 레이아웃 ──────────────────────────────────────────────

struct DisplayLayout {
    double right_edge;
    double top;
    double bottom;
    double height;
    std::vector<CGRect> displays; // 우측 끝 모니터만
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

    // 우측 끝 모니터 목록만 캐싱 (is_at_right_edge에서 반복 조회 불필요)
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

    std::cout << "=== 모니터 정보 (" << count << "개) ===" << std::endl;
    for (uint32_t i = 0; i < count; ++i) {
        CGRect b   = CGDisplayBounds(ids[i]);
        CGSize mm  = CGDisplayScreenSize(ids[i]);
        bool main  = (ids[i] == CGMainDisplayID());
        std::cout << "  [" << i << "] " << (main ? "(main) " : "       ")
                  << "origin=(" << (int)b.origin.x << "," << (int)b.origin.y << ")"
                  << "  size=" << (int)b.size.width << "x" << (int)b.size.height
                  << "  physical=" << (int)mm.width << "x" << (int)mm.height << "mm\n";
    }
    std::cout << "=========================\n";
}

static bool is_at_right_edge(CGPoint loc) {
    if (loc.x < g_layout.right_edge - 3.0) return false;
    for (const CGRect& b : g_layout.displays) {
        if (loc.y >= b.origin.y && loc.y <= b.origin.y + b.size.height)
            return true;
    }
    return false;
}

// ── 원격 모드 상태 ───────────────────────────────────────────────────

struct RemoteState {
    bool active        = false;
    double virtual_x   = 0.0;
    double entry_y     = 0.0;
    int entry_guard    = 0; // 진입 직후 delta 스파이크 무시
    int return_guard   = 0; // 복귀 직후 재진입 방지
};

static RemoteState g_state;
static NetworkEngine* g_engine = nullptr; // 키보드 콜백에서 접근

static void enter_remote(NetworkEngine& engine, CGPoint loc) {
    g_state.active      = true;
    g_state.virtual_x   = 0.0;
    g_state.entry_y     = loc.y;
    g_state.entry_guard = 5;

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
    engine.push_mouse(pkt);
}

static void leave_remote(NetworkEngine& engine) {
    g_state.active       = false;
    g_state.return_guard = 10;

    std::cout << "[로컬 모드 복귀] virtual_x=" << (int)g_state.virtual_x << "\n";

    MousePacket pkt{};
    pkt.event_type = EVENT_SHOW_CURSOR;
    engine.push_mouse(pkt);

    CGWarpMouseCursorPosition(CGPointMake(g_layout.right_edge - 50.0, g_state.entry_y));
    CGAssociateMouseAndMouseCursorPosition(true);
    show_cursor();
}

// ── 마우스 이벤트 콜백 ───────────────────────────────────────────────

static CGEventRef mouse_callback(CGEventTapProxy, CGEventType type, CGEventRef event, void* userInfo) {
    auto& engine = *static_cast<NetworkEngine*>(userInfo);

    if (!g_state.active) {
        CGPoint loc = CGEventGetLocation(event);

        // 디버그 (50ms 간격)
        static double last_log = 0;
        double now = CFAbsoluteTimeGetCurrent();
        if (now - last_log > 0.05) {
            std::cout << "[mouse] x=" << (int)loc.x << " y=" << (int)loc.y
                      << "  right_edge=" << (int)g_layout.right_edge << "\n";
            last_log = now;
        }

        if (g_state.return_guard > 0) { --g_state.return_guard; return event; }

        if (is_at_right_edge(loc))
            enter_remote(engine, loc);

        return event;
    }

    // 원격 모드: 매 이벤트마다 커서를 화면 밖으로 유지
    CGWarpMouseCursorPosition(CGPointMake(g_layout.right_edge, 99999.0));

    bool is_click = (type == kCGEventLeftMouseDown  || type == kCGEventLeftMouseUp ||
                     type == kCGEventRightMouseDown || type == kCGEventRightMouseUp);

    if (!is_click) {
        if (g_state.entry_guard > 0) { --g_state.entry_guard; return NULL; }

        int64_t dx = CGEventGetIntegerValueField(event, kCGMouseEventDeltaX);
        g_state.virtual_x = std::min(g_state.virtual_x + dx,
                                     (double)g_client_width.load(std::memory_order_relaxed));

        if (g_state.virtual_x <= 0.0) {
            leave_remote(engine);
            return NULL;
        }
    }

    int64_t dx = CGEventGetIntegerValueField(event, kCGMouseEventDeltaX);
    int64_t dy = CGEventGetIntegerValueField(event, kCGMouseEventDeltaY);

    MousePacket pkt{};
    pkt.dx         = static_cast<int16_t>(std::clamp(dx, -32768LL, 32767LL));
    pkt.dy         = static_cast<int16_t>(std::clamp(dy, -32768LL, 32767LL));
    pkt.event_type = type;
    engine.push_mouse(pkt);

    return NULL;
}

// ── 키보드 이벤트 콜백 ───────────────────────────────────────────────

static CGEventRef keyboard_callback(CGEventTapProxy, CGEventType type, CGEventRef event, void* userInfo) {
    if (!g_state.active) return event;

    auto& engine = *static_cast<NetworkEngine*>(userInfo);

    CGKeyCode keycode = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    CGEventFlags flags = CGEventGetFlags(event);

    KeyPacket pkt{};
    pkt.keycode = keycode;
    pkt.flags   = static_cast<uint32_t>(flags & 0xFFFFFFFF);

    if (type == kCGEventKeyDown)
        pkt.event_type = EVENT_KEY_DOWN;
    else if (type == kCGEventKeyUp)
        pkt.event_type = EVENT_KEY_UP;
    else // kCGEventFlagsChanged
        pkt.event_type = EVENT_FLAGS_CHANGE;

    engine.push_key(pkt);

    // 원격 모드 중 키 입력은 로컬에 전달하지 않음
    return NULL;
}

// ── main ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "사용법: ./host_app <client_ip> <port>\n";
        return 1;
    }

    print_layout();
    refresh_layout();
    std::cout << "[경계] right=" << (int)g_layout.right_edge
              << " top=" << (int)g_layout.top
              << " bottom=" << (int)g_layout.bottom
              << " height=" << (int)g_layout.height << "\n";

    auto on_recv = [](const MousePacket& pkt) {
        if (pkt.event_type == EVENT_CLIENT_INFO) {
            int32_t w = ((int32_t)(uint16_t)pkt.dy << 16) | (uint16_t)pkt.dx;
            g_client_width.store(w, std::memory_order_relaxed);
            std::cout << "[← client] 화면 너비: " << w << "px\n";
        }
    };

    int port = std::stoi(argv[2]);
    NetworkEngine engine(argv[1], port, on_recv);
    g_engine = &engine;
    engine.start();

    // 마우스 이벤트 탭
    CGEventMask mouse_mask = CGEventMaskBit(kCGEventMouseMoved)        |
                             CGEventMaskBit(kCGEventLeftMouseDragged)  |
                             CGEventMaskBit(kCGEventRightMouseDragged) |
                             CGEventMaskBit(kCGEventLeftMouseDown)     |
                             CGEventMaskBit(kCGEventLeftMouseUp)       |
                             CGEventMaskBit(kCGEventRightMouseDown)    |
                             CGEventMaskBit(kCGEventRightMouseUp);

    CFMachPortRef mouse_tap = CGEventTapCreate(
        kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
        mouse_mask, mouse_callback, &engine);

    if (!mouse_tap) {
        std::cerr << "오류: Event Tap 권한 확인 필요 (손쉬운 사용 권한)\n";
        return 1;
    }

    // 키보드 이벤트 탭
    CGEventMask key_mask = CGEventMaskBit(kCGEventKeyDown)      |
                           CGEventMaskBit(kCGEventKeyUp)        |
                           CGEventMaskBit(kCGEventFlagsChanged);

    CFMachPortRef key_tap = CGEventTapCreate(
        kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
        key_mask, keyboard_callback, &engine);

    if (!key_tap) {
        std::cerr << "오류: 키보드 Event Tap 생성 실패 (손쉬운 사용 권한)\n";
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

    std::cout << "Host 대기 중... (우측 끝으로 마우스를 이동하면 클라이언트로 전환)\n";
    CFRunLoopRun();

    CGAssociateMouseAndMouseCursorPosition(true);
    show_cursor();
    engine.stop();

    CFRelease(key_src);
    CFRelease(key_tap);
    CFRelease(mouse_src);
    CFRelease(mouse_tap);
    return 0;
}
