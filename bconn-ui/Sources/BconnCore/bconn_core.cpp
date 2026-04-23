#include "include/bconn_core.h"
#include "NetworkEngine.h"
#include <ApplicationServices/ApplicationServices.h>
#include <CommonCrypto/CommonHMAC.h>
#include <objc/objc-runtime.h>
#include <algorithm>
#include <atomic>
#include <vector>
#include <cstring>
#include <ctime>
#include <thread>
#include <chrono>
#include <ifaddrs.h>
#include <netdb.h>
#include <unistd.h>

// ── 공통 유틸 ────────────────────────────────────────────────────────

static void hide_cursor() {
    id cls = (id)objc_getClass("NSCursor");
    ((void(*)(id,SEL))objc_msgSend)(cls, sel_registerName("hide"));
}

static void show_cursor() {
    id cls = (id)objc_getClass("NSCursor");
    ((void(*)(id,SEL))objc_msgSend)(cls, sel_registerName("unhide"));
}

// ── 전역 상태 ─────────────────────────────────────────────────────────

static std::atomic<BconnStatus> g_status{BCONN_STATUS_IDLE};
static BconnStatusCallback g_cb  = nullptr;
static void*               g_ctx = nullptr;
static BconnConfig         g_cfg{};
static NetworkEngine*      g_engine = nullptr;
static CFMachPortRef       g_mouse_tap = nullptr;
static CFMachPortRef       g_key_tap   = nullptr;
static CFRunLoopRef        g_run_loop  = nullptr;
static std::thread         g_run_loop_thread;

// 인증 관련 전역
static char                g_psk[128]      = {};
static char                g_hostname[32]  = {};
static std::atomic<bool>   g_auth_ok{false};
static std::thread         g_heartbeat_thread;
static std::atomic<bool>   g_heartbeat_running{false};

static void set_status(BconnStatus s, const char* msg = "") {
    g_status.store(s);
    if (g_cb) g_cb(s, msg, g_ctx);
}

// ── HMAC 인증 ─────────────────────────────────────────────────────────

// HMAC-SHA1(psk, hostname[32] || timestamp_s LE) → 20 bytes
static void compute_hmac(const char* psk, const char hostname32[32],
                          int64_t ts, uint8_t out[20]) {
    uint8_t data[40];
    memcpy(data,      hostname32, 32);
    memcpy(data + 32, &ts,        8);  // little-endian (Apple Silicon/Intel 모두 LE)
    CCHmac(kCCHmacAlgSHA1, psk, strlen(psk), data, sizeof(data), out);
}

static void send_hello() {
    HelloPacket pkt{};
    pkt.packet_type  = PACKET_HELLO;
    strncpy(pkt.hostname, g_hostname, sizeof(pkt.hostname) - 1);
    pkt.timestamp_s  = (int64_t)time(nullptr);

    if (strlen(g_psk) > 0) {
        char hn32[32] = {};
        memcpy(hn32, pkt.hostname, sizeof(hn32));
        compute_hmac(g_psk, hn32, pkt.timestamp_s, pkt.hmac);
    }
    // psk 없으면 hmac은 zero → host에서 open mode로 수락

    g_engine->send_raw_any(&pkt, sizeof(pkt));
}

// ── 디스플레이 레이아웃 ──────────────────────────────────────────────

struct DisplayLayout {
    double trigger_edge; // 커서가 넘어가는 경계
    double top, bottom, height;
    double left, right;
    std::vector<CGRect> edge_displays;
};

static DisplayLayout g_layout;
static std::atomic<int32_t> g_client_width{32000};
static std::atomic<int32_t> g_client_height{18000};

static void refresh_layout(BconnEdge edge) {
    CGDirectDisplayID ids[16];
    uint32_t count = 0;
    CGGetActiveDisplayList(16, ids, &count);

    double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
    for (uint32_t i = 0; i < count; ++i) {
        CGRect b = CGDisplayBounds(ids[i]);
        min_x = std::min(min_x, b.origin.x);
        max_x = std::max(max_x, b.origin.x + b.size.width);
        min_y = std::min(min_y, b.origin.y);
        max_y = std::max(max_y, b.origin.y + b.size.height);
    }

    g_layout.left   = min_x;
    g_layout.right  = max_x;
    g_layout.top    = min_y;
    g_layout.bottom = max_y;
    g_layout.height = max_y - min_y;

    // 경계 방향에 따라 trigger_edge 및 edge_displays 설정
    g_layout.edge_displays.clear();
    for (uint32_t i = 0; i < count; ++i) {
        CGRect b = CGDisplayBounds(ids[i]);
        bool on_edge = false;
        switch (edge) {
        case BCONN_EDGE_RIGHT:
            g_layout.trigger_edge = max_x;
            on_edge = (b.origin.x + b.size.width >= max_x - 1.0);
            break;
        case BCONN_EDGE_LEFT:
            g_layout.trigger_edge = min_x;
            on_edge = (b.origin.x <= min_x + 1.0);
            break;
        case BCONN_EDGE_TOP:
            g_layout.trigger_edge = min_y;
            on_edge = (b.origin.y <= min_y + 1.0);
            break;
        case BCONN_EDGE_BOTTOM:
            g_layout.trigger_edge = max_y;
            on_edge = (b.origin.y + b.size.height >= max_y - 1.0);
            break;
        }
        if (on_edge) g_layout.edge_displays.push_back(b);
    }
}

static bool is_at_trigger_edge(CGPoint loc) {
    const double threshold = 3.0;
    bool hit = false;
    switch (g_cfg.client_edge) {
    case BCONN_EDGE_RIGHT:  hit = loc.x >= g_layout.trigger_edge - threshold; break;
    case BCONN_EDGE_LEFT:   hit = loc.x <= g_layout.trigger_edge + threshold; break;
    case BCONN_EDGE_TOP:    hit = loc.y <= g_layout.trigger_edge + threshold; break;
    case BCONN_EDGE_BOTTOM: hit = loc.y >= g_layout.trigger_edge - threshold; break;
    }
    if (!hit) return false;

    for (const CGRect& b : g_layout.edge_displays) {
        bool in_range = false;
        switch (g_cfg.client_edge) {
        case BCONN_EDGE_RIGHT:
        case BCONN_EDGE_LEFT:
            in_range = (loc.y >= b.origin.y && loc.y <= b.origin.y + b.size.height);
            break;
        case BCONN_EDGE_TOP:
        case BCONN_EDGE_BOTTOM:
            in_range = (loc.x >= b.origin.x && loc.x <= b.origin.x + b.size.width);
            break;
        }
        if (in_range) return true;
    }
    return false;
}

// ── 원격 모드 상태 ────────────────────────────────────────────────────

struct RemoteState {
    bool   active       = false;
    double virtual_x    = 0.0;
    double virtual_y    = 0.0;
    double entry_x      = 0.0;
    double entry_y      = 0.0;
    int    entry_guard  = 0;
    int    return_guard = 0;
};

static RemoteState g_remote;

// 진입 시 커서를 화면 밖으로 밀어두는 위치
static CGPoint offscreen_point() {
    switch (g_cfg.client_edge) {
    case BCONN_EDGE_RIGHT:  return CGPointMake(g_layout.right,  99999.0);
    case BCONN_EDGE_LEFT:   return CGPointMake(g_layout.left - 9999.0, g_layout.top);
    case BCONN_EDGE_TOP:    return CGPointMake(99999.0, g_layout.top - 9999.0);
    case BCONN_EDGE_BOTTOM: return CGPointMake(99999.0, g_layout.bottom);
    }
    return CGPointMake(g_layout.right, 99999.0);
}

// 복귀 시 커서를 경계 안쪽으로 되돌리는 위치
static CGPoint return_point() {
    const double offset = 50.0;
    switch (g_cfg.client_edge) {
    case BCONN_EDGE_RIGHT:  return CGPointMake(g_layout.trigger_edge - offset, g_remote.entry_y);
    case BCONN_EDGE_LEFT:   return CGPointMake(g_layout.trigger_edge + offset, g_remote.entry_y);
    case BCONN_EDGE_TOP:    return CGPointMake(g_remote.entry_x, g_layout.trigger_edge + offset);
    case BCONN_EDGE_BOTTOM: return CGPointMake(g_remote.entry_x, g_layout.trigger_edge - offset);
    }
    return CGPointMake(g_layout.trigger_edge - offset, g_remote.entry_y);
}

static void enter_remote(CGPoint loc) {
    g_remote.active      = true;
    g_remote.virtual_x   = 0.0;
    g_remote.virtual_y   = 0.0;
    g_remote.entry_x     = loc.x;
    g_remote.entry_y     = loc.y;
    g_remote.entry_guard = 5;

    hide_cursor();
    CGAssociateMouseAndMouseCursorPosition(false);
    CGWarpMouseCursorPosition(offscreen_point());

    // 진입 위치 비율 계산 (가로/세로 방향 모두 대응)
    double ratio = 0.5;
    switch (g_cfg.client_edge) {
    case BCONN_EDGE_RIGHT:
    case BCONN_EDGE_LEFT:
        ratio = (g_layout.height > 0)
            ? std::clamp((loc.y - g_layout.top) / g_layout.height, 0.0, 1.0)
            : 0.5;
        break;
    case BCONN_EDGE_TOP:
    case BCONN_EDGE_BOTTOM:
        ratio = (g_layout.right - g_layout.left > 0)
            ? std::clamp((loc.x - g_layout.left) / (g_layout.right - g_layout.left), 0.0, 1.0)
            : 0.5;
        break;
    }

    MousePacket pkt{};
    pkt.event_type = EVENT_SYNC_CURSOR;
    pkt.dx = static_cast<int16_t>(g_cfg.client_edge); // 방향 전달
    pkt.dy = static_cast<int16_t>(ratio * 32767.0);
    g_engine->send_mouse(pkt);

    set_status(BCONN_STATUS_REMOTE, "원격 모드");
}

static void leave_remote() {
    g_remote.active       = false;
    g_remote.return_guard = 10;

    MousePacket pkt{};
    pkt.event_type = EVENT_SHOW_CURSOR;
    g_engine->send_mouse(pkt);

    CGWarpMouseCursorPosition(return_point());
    CGAssociateMouseAndMouseCursorPosition(true);
    show_cursor();

    set_status(BCONN_STATUS_CONNECTED, "연결됨");
}

// ── 마우스 콜백 ───────────────────────────────────────────────────────

static CGEventRef mouse_callback(CGEventTapProxy, CGEventType type, CGEventRef event, void*) {
    if (!g_remote.active) {
        CGPoint loc = CGEventGetLocation(event);
        if (g_remote.return_guard > 0) { --g_remote.return_guard; return event; }
        if (is_at_trigger_edge(loc)) enter_remote(loc);
        return event;
    }

    bool is_scroll = (type == kCGEventScrollWheel);
    bool is_click  = (type == kCGEventLeftMouseDown   || type == kCGEventLeftMouseUp  ||
                      type == kCGEventRightMouseDown  || type == kCGEventRightMouseUp ||
                      type == kCGEventOtherMouseDown  || type == kCGEventOtherMouseUp);

    if (!is_scroll)
        CGWarpMouseCursorPosition(offscreen_point());

    if (!is_click && !is_scroll) {
        if (g_remote.entry_guard > 0) { --g_remote.entry_guard; return NULL; }

        int64_t dx = CGEventGetIntegerValueField(event, kCGMouseEventDeltaX);
        int64_t dy = CGEventGetIntegerValueField(event, kCGMouseEventDeltaY);

        // 방향에 따라 복귀 판단 축이 다름
        switch (g_cfg.client_edge) {
        case BCONN_EDGE_RIGHT:
            g_remote.virtual_x = std::min(g_remote.virtual_x + dx,
                                          (double)g_client_width.load(std::memory_order_relaxed));
            if (g_remote.virtual_x <= 0.0) { leave_remote(); return NULL; }
            break;
        case BCONN_EDGE_LEFT:
            g_remote.virtual_x = std::max(g_remote.virtual_x + dx,
                                          -(double)g_client_width.load(std::memory_order_relaxed));
            if (g_remote.virtual_x >= 0.0) { leave_remote(); return NULL; }
            break;
        case BCONN_EDGE_BOTTOM:
            g_remote.virtual_y = std::min(g_remote.virtual_y + dy,
                                          (double)g_client_height.load(std::memory_order_relaxed));
            if (g_remote.virtual_y <= 0.0) { leave_remote(); return NULL; }
            break;
        case BCONN_EDGE_TOP:
            g_remote.virtual_y = std::max(g_remote.virtual_y + dy,
                                          -(double)g_client_height.load(std::memory_order_relaxed));
            if (g_remote.virtual_y >= 0.0) { leave_remote(); return NULL; }
            break;
        }
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
    g_engine->send_mouse(pkt);
    return NULL;
}

// ── 키보드 콜백 ───────────────────────────────────────────────────────

static CGEventRef keyboard_callback(CGEventTapProxy, CGEventType type, CGEventRef event, void*) {
    if (!g_remote.active) return event;

    CGKeyCode    keycode = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    CGEventFlags flags   = CGEventGetFlags(event);

    KeyPacket pkt{};
    pkt.keycode    = keycode;
    pkt.flags      = static_cast<uint32_t>(flags & 0xFFFFFFFF);
    pkt.event_type = (type == kCGEventKeyDown) ? EVENT_KEY_DOWN
                   : (type == kCGEventKeyUp)   ? EVENT_KEY_UP
                   :                             EVENT_FLAGS_CHANGE;
    g_engine->send_key(pkt);
    return NULL;
}

// ── RunLoop 스레드 ────────────────────────────────────────────────────

static void setup_event_taps() {
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

    g_mouse_tap = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap,
                                   kCGEventTapOptionDefault,
                                   mouse_mask, mouse_callback, nullptr);

    CGEventMask key_mask = CGEventMaskBit(kCGEventKeyDown)    |
                           CGEventMaskBit(kCGEventKeyUp)      |
                           CGEventMaskBit(kCGEventFlagsChanged);
    g_key_tap = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap,
                                 kCGEventTapOptionDefault,
                                 key_mask, keyboard_callback, nullptr);

    if (!g_mouse_tap || !g_key_tap) {
        set_status(BCONN_STATUS_ERROR, "Event Tap 권한 필요 (손쉬운 사용)");
        return;
    }

    g_run_loop = CFRunLoopGetCurrent();

    auto add_tap = [](CFMachPortRef tap) {
        CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
        CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopCommonModes);
        CGEventTapEnable(tap, true);
        CFRelease(src);
    };
    add_tap(g_mouse_tap);
    add_tap(g_key_tap);
}

static void teardown_event_taps() {
    if (g_mouse_tap) { CGEventTapEnable(g_mouse_tap, false); CFRelease(g_mouse_tap); g_mouse_tap = nullptr; }
    if (g_key_tap)   { CGEventTapEnable(g_key_tap,   false); CFRelease(g_key_tap);   g_key_tap   = nullptr; }
}

// ── Client 수신 처리 ──────────────────────────────────────────────────

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

// client 전용 상태 (NetworkEngine recv 콜백에서 접근)
struct ClientState {
    ScreenBounds screen;
    double cur_x = 0, cur_y = 0;
    bool pos_init      = false;
    bool cursor_hidden = false;
};

static ClientState g_client;

static void send_client_info() {
    int32_t w = (int32_t)g_client.screen.width();
    int32_t h = (int32_t)g_client.screen.height();

    MousePacket info{};
    info.event_type = EVENT_CLIENT_INFO;
    info.dx = static_cast<int16_t>(w & 0xFFFF);
    info.dy = static_cast<int16_t>((w >> 16) & 0xFFFF);
    g_engine->send_mouse(info);

    MousePacket hinfo{};
    hinfo.event_type = EVENT_CLIENT_INFO + 1;
    hinfo.dx = static_cast<int16_t>(h & 0xFFFF);
    hinfo.dy = static_cast<int16_t>((h >> 16) & 0xFFFF);
    g_engine->send_mouse(hinfo);
}

static void client_handle_sync(const MousePacket& pkt) {
    g_client.screen = get_screen_bounds();

    BconnEdge host_edge = (BconnEdge)pkt.dx; // host가 어느 방향 경계에서 넘어왔는지
    double ratio = static_cast<double>(pkt.dy) / 32767.0;

    // host의 진입 방향에 따라 client 커서 진입 위치 결정
    switch (host_edge) {
    case BCONN_EDGE_RIGHT:  // host 우측 → client 좌측 진입
        g_client.cur_x = g_client.screen.left + 2.0;
        g_client.cur_y = g_client.screen.top + ratio * g_client.screen.height();
        break;
    case BCONN_EDGE_LEFT:   // host 좌측 → client 우측 진입
        g_client.cur_x = g_client.screen.right - 2.0;
        g_client.cur_y = g_client.screen.top + ratio * g_client.screen.height();
        break;
    case BCONN_EDGE_BOTTOM: // host 아래 → client 위쪽 진입
        g_client.cur_x = g_client.screen.left + ratio * g_client.screen.width();
        g_client.cur_y = g_client.screen.top + 2.0;
        break;
    case BCONN_EDGE_TOP:    // host 위 → client 아래쪽 진입
        g_client.cur_x = g_client.screen.left + ratio * g_client.screen.width();
        g_client.cur_y = g_client.screen.bottom - 2.0;
        break;
    }

    g_client.pos_init = true;
    CGWarpMouseCursorPosition(CGPointMake(g_client.cur_x, g_client.cur_y));
    if (g_client.cursor_hidden) { show_cursor(); g_client.cursor_hidden = false; }

    // host에 화면 크기 전달
    send_client_info();
}

static void client_handle_move(const MousePacket& pkt) {
    CGEventType etype = (CGEventType)pkt.event_type;

    if (etype == kCGEventScrollWheel) {
        CGEventRef ev = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 2, pkt.dy, pkt.dx);
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
        return;
    }

    g_client.cur_x = std::clamp(g_client.cur_x + pkt.dx,
                                 g_client.screen.left, g_client.screen.right  - 1.0);
    g_client.cur_y = std::clamp(g_client.cur_y + pkt.dy,
                                 g_client.screen.top,  g_client.screen.bottom - 1.0);

    CGMouseButton btn = kCGMouseButtonLeft;
    if (etype == kCGEventRightMouseDown || etype == kCGEventRightMouseUp ||
        etype == kCGEventRightMouseDragged)
        btn = kCGMouseButtonRight;
    else if (etype == kCGEventOtherMouseDown || etype == kCGEventOtherMouseUp ||
             etype == kCGEventOtherMouseDragged)
        btn = kCGMouseButtonCenter;

    CGPoint pos = CGPointMake(g_client.cur_x, g_client.cur_y);
    CGEventRef ev = CGEventCreateMouseEvent(NULL, etype, pos, btn);
    CGEventSetIntegerValueField(ev, kCGMouseEventDeltaX, pkt.dx);
    CGEventSetIntegerValueField(ev, kCGMouseEventDeltaY, pkt.dy);
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
}

static void client_handle_key(const KeyPacket& pkt) {
    bool key_down = (pkt.event_type == EVENT_KEY_DOWN);
    CGEventRef ev = CGEventCreateKeyboardEvent(NULL, pkt.keycode,
                        pkt.event_type == EVENT_FLAGS_CHANGE ? false : key_down);
    CGEventSetFlags(ev, (CGEventFlags)pkt.flags);
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
}

// ── 공개 API ─────────────────────────────────────────────────────────

bool bconn_start(const BconnConfig* config, BconnStatusCallback cb, void* ctx) {
    if (g_status.load() != BCONN_STATUS_IDLE) return false;

    g_cfg = *config;
    g_cb  = cb;
    g_ctx = ctx;

    // PSK, 호스트명 초기화
    strncpy(g_psk, g_cfg.psk, sizeof(g_psk) - 1);
    g_psk[sizeof(g_psk) - 1] = '\0';
    gethostname(g_hostname, sizeof(g_hostname));
    g_hostname[sizeof(g_hostname) - 1] = '\0';
    g_auth_ok.store(false, std::memory_order_release);

    if (g_cfg.is_host) {
        refresh_layout(g_cfg.client_edge);

        // host: CLIENT_INFO는 인증된 peer에게서만 수락
        auto on_mouse = [](const MousePacket& pkt) {
            if (pkt.event_type == EVENT_CLIENT_INFO) {
                int32_t w = ((int32_t)(uint16_t)pkt.dy << 16) | (uint16_t)pkt.dx;
                g_client_width.store(w, std::memory_order_relaxed);
            } else if (pkt.event_type == EVENT_CLIENT_INFO + 1) {
                int32_t h = ((int32_t)(uint16_t)pkt.dy << 16) | (uint16_t)pkt.dx;
                g_client_height.store(h, std::memory_order_relaxed);
            }
        };

        // host: HELLO 수신 → HMAC 검증 → AUTH_ACK 응답
        auto on_hello = [](const HelloPacket& pkt, const sockaddr_in& sender) {
            // 재전송 공격 방지: ±30초 이내만 허용
            int64_t now = (int64_t)time(nullptr);
            int64_t diff = pkt.timestamp_s - now;
            if (diff < -30 || diff > 30) {
                std::cerr << "[auth] HELLO 거부: timestamp 범위 초과 (diff=" << diff << "s)\n";
                return;
            }

            bool accepted = false;
            if (strlen(g_psk) == 0) {
                // PSK 없음 → open mode: hostname만 있으면 수락
                accepted = (pkt.hostname[0] != '\0');
            } else {
                char hn32[32] = {};
                memcpy(hn32, pkt.hostname, sizeof(hn32));
                uint8_t expected[20];
                compute_hmac(g_psk, hn32, pkt.timestamp_s, expected);
                accepted = (memcmp(expected, pkt.hmac, 20) == 0);
            }

            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sender.sin_addr, ip_str, sizeof(ip_str));

            if (accepted) {
                g_engine->set_peer(sender);
                std::cout << "[auth] 수락: " << pkt.hostname << " @ " << ip_str << "\n";
                // message에 클라이언트 호스트명 전달 (Swift UI 표시용)
                set_status(BCONN_STATUS_CONNECTED, pkt.hostname);
            } else {
                std::cerr << "[auth] 거부: " << pkt.hostname << " @ " << ip_str << " (PSK 불일치)\n";
            }

            g_engine->send_auth_result(accepted, g_hostname, sender);
        };

        g_engine = new NetworkEngine(nullptr, g_cfg.port, on_mouse, nullptr, on_hello, nullptr);
        g_engine->start();

        g_run_loop_thread = std::thread([] {
            setup_event_taps();
            if (g_status.load() != BCONN_STATUS_ERROR) {
                set_status(BCONN_STATUS_WAITING, "client 접속 대기 중");
                CFRunLoopRun();
            }
        });

    } else {
        // client 모드
        g_client.screen = get_screen_bounds();

        auto on_mouse = [](const MousePacket& pkt) {
            // 인증 완료 전에는 host 패킷 무시
            if (!g_auth_ok.load(std::memory_order_acquire)) return;

            switch (pkt.event_type) {
            case EVENT_SYNC_CURSOR:
                client_handle_sync(pkt);
                set_status(BCONN_STATUS_REMOTE, "원격 모드");
                break;
            case EVENT_SHOW_CURSOR:
                if (!g_client.cursor_hidden) { hide_cursor(); g_client.cursor_hidden = true; }
                g_client.pos_init = false;
                set_status(BCONN_STATUS_CONNECTED, "연결됨");
                break;
            default:
                if (g_client.pos_init) client_handle_move(pkt);
                break;
            }
        };

        auto on_key = [](const KeyPacket& pkt) {
            if (!g_auth_ok.load(std::memory_order_acquire)) return;
            if (g_client.pos_init) client_handle_key(pkt);
        };

        // client: AUTH_ACK 수신 처리
        auto on_auth_ack = [](const AuthAckPacket& pkt) {
            if (pkt.accepted) {
                g_auth_ok.store(true, std::memory_order_release);
                // 인증 성공 후 화면 크기 전달
                send_client_info();
                set_status(BCONN_STATUS_CONNECTED, "연결됨");
                std::cout << "[auth] 인증 성공, host=" << pkt.host_hostname << "\n";
            } else {
                g_auth_ok.store(false, std::memory_order_release);
                set_status(BCONN_STATUS_AUTH_FAILED, "PSK 불일치");
                std::cerr << "[auth] 인증 실패\n";
            }
        };

        g_engine = new NetworkEngine(g_cfg.host_ip, g_cfg.port,
                                     on_mouse, on_key, nullptr, on_auth_ack);
        g_engine->start();

        // 최초 HELLO 전송 (인증 요청)
        send_hello();
        set_status(BCONN_STATUS_WAITING, "인증 대기 중");

        // heartbeat: 5초마다 HELLO 재전송 (host 재시작 후 자동 재연결)
        g_heartbeat_running.store(true, std::memory_order_release);
        g_heartbeat_thread = std::thread([] {
            while (g_heartbeat_running.load(std::memory_order_acquire)) {
                for (int i = 0; i < 50 && g_heartbeat_running.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!g_heartbeat_running.load(std::memory_order_acquire)) break;

                // 인증 상태 리셋 후 재시도 (host 재시작 감지)
                if (!g_auth_ok.load(std::memory_order_acquire)) {
                    set_status(BCONN_STATUS_WAITING, "재연결 중...");
                }
                g_auth_ok.store(false, std::memory_order_release);
                send_hello();
            }
        });
    }

    return true;
}

void bconn_stop() {
    // 콜백을 먼저 해제해 join 중 발생하는 set_status 호출이 Swift로 전달되지 않도록 한다.
    // (join 대기 중 메인 스레드로 디스패치되면 데드락 위험)
    g_cb  = nullptr;
    g_ctx = nullptr;

    // heartbeat 스레드 종료
    g_heartbeat_running.store(false, std::memory_order_release);
    if (g_heartbeat_thread.joinable()) g_heartbeat_thread.join();

    if (g_run_loop) {
        CFRunLoopStop(g_run_loop);
        g_run_loop = nullptr;
    }
    if (g_run_loop_thread.joinable()) g_run_loop_thread.join();

    teardown_event_taps();

    if (g_engine) {
        g_engine->stop();
        delete g_engine;
        g_engine = nullptr;
    }

    CGAssociateMouseAndMouseCursorPosition(true);
    show_cursor();
    g_remote  = {};
    g_client  = {};
    g_auth_ok.store(false, std::memory_order_release);
    set_status(BCONN_STATUS_IDLE, "중지됨");
}

BconnStatus bconn_get_status() {
    return g_status.load();
}

int bconn_get_local_ips(char* out, int out_size) {
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0) return 0;

    int count = 0;
    int pos   = 0;
    for (struct ifaddrs* ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strncmp(ifa->ifa_name, "lo", 2) == 0) continue;

        char ip[64] = {};
        getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                    ip, sizeof(ip), nullptr, 0, NI_NUMERICHOST);

        int len = (int)strlen(ip);
        if (pos + len + 2 > out_size) break; // 버퍼 초과 방지
        memcpy(out + pos, ip, len);
        out[pos + len] = '\n';
        pos += len + 1;
        ++count;
    }
    if (pos < out_size) out[pos] = '\0';
    freeifaddrs(ifap);
    return count;
}
