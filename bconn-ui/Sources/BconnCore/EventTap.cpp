/// EventTap.cpp — 호스트 전용 CGEventTap 구현
///
/// 이 파일의 함수들은 호스트(is_host=true) 모드에서만 사용된다.
/// 클라이언트 수신 처리는 ClientHandler.cpp 에 있다.
#include "EventTap.h"
#include "NetworkEngine.h"
#include "BconnLog.h"
#include <ApplicationServices/ApplicationServices.h>
#include <objc/objc-runtime.h>
#include <algorithm>
#include <atomic>
#include <vector>
#include <cstring>

// ── bconn_core.cpp 에서 가져오는 전역 ────────────────────────────────

extern BconnConfig              g_cfg;
extern NetworkEngine*           g_engine;
extern CFMachPortRef            g_mouse_tap;
extern CFMachPortRef            g_key_tap;
extern std::atomic<int32_t>     g_client_width;
extern std::atomic<int32_t>     g_client_height;
extern RemoteState              g_remote;
extern std::atomic<CFRunLoopRef> g_run_loop;

void set_status(BconnStatus s, const char* msg);

// ── 커서 유틸 ─────────────────────────────────────────────────────────

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
    double trigger_edge;          // 커서가 클라이언트로 넘어가는 경계 좌표
    double top, bottom, height;
    double left, right;
    std::vector<CGRect> edge_displays; // 경계 방향에 접한 디스플레이 목록
};

static DisplayLayout g_layout;

void refresh_layout(BconnEdge edge) {
    CGDirectDisplayID ids[16];
    uint32_t count = 0;
    CGGetActiveDisplayList(16, ids, &count);

    BCONN_DEBUG(CAT_CURSOR, "[호스트] 디스플레이 레이아웃 새로고침: 디스플레이 수=%u, 경계 방향=%d", count, (int)edge);

    double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
    for (uint32_t i = 0; i < count; ++i) {
        CGRect b = CGDisplayBounds(ids[i]);
        min_x = std::min(min_x, b.origin.x);
        max_x = std::max(max_x, b.origin.x + b.size.width);
        min_y = std::min(min_y, b.origin.y);
        max_y = std::max(max_y, b.origin.y + b.size.height);
        BCONN_DEBUG(CAT_CURSOR, "[호스트] 디스플레이 %u: 위치=(%.0f, %.0f), 크기=(%.0f x %.0f)",
                    i, b.origin.x, b.origin.y, b.size.width, b.size.height);
    }

    g_layout.left   = min_x;
    g_layout.right  = max_x;
    g_layout.top    = min_y;
    g_layout.bottom = max_y;
    g_layout.height = max_y - min_y;

    BCONN_DEBUG(CAT_CURSOR, "[호스트] 전체 화면 영역: (%.0f, %.0f) ~ (%.0f, %.0f), 높이=%.0f",
                g_layout.left, g_layout.top, g_layout.right, g_layout.bottom, g_layout.height);

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
        if (on_edge) {
            g_layout.edge_displays.push_back(b);
            BCONN_DEBUG(CAT_CURSOR, "[호스트] 에지 디스플레이 추가: %u", i);
        }
    }

    BCONN_INFO(CAT_CURSOR, "[호스트] 레이아웃 새로고침 완료: 트리거 에지=%.0f, 에지 디스플레이 수=%zu",
               g_layout.trigger_edge, g_layout.edge_displays.size());
}

/// 현재 커서 위치가 경계(trigger edge) 3px 이내이고 경계 디스플레이 범위 안인지 검사한다.
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

// RemoteState 정의는 EventTap.h 에 있다.
// g_remote 인스턴스는 bconn_core.cpp 에 있고, 아래에서 extern으로 참조한다.

/// 원격 진입 시 커서를 화면 밖으로 밀어두는 위치.
/// CGEventTap이 계속 실행되는 동안 커서가 실제로 보이지 않도록 한다.
static CGPoint offscreen_point() {
    switch (g_cfg.client_edge) {
    case BCONN_EDGE_RIGHT:  return CGPointMake(g_layout.right,          99999.0);
    case BCONN_EDGE_LEFT:   return CGPointMake(g_layout.left - 9999.0,  g_layout.top);
    case BCONN_EDGE_TOP:    return CGPointMake(99999.0,                 g_layout.top - 9999.0);
    case BCONN_EDGE_BOTTOM: return CGPointMake(99999.0,                 g_layout.bottom);
    }
    return CGPointMake(g_layout.right, 99999.0);
}

/// 복귀 시 커서를 경계 안쪽 50px 지점으로 되돌린다.
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
    g_remote.virtual_x   = 0.0;
    g_remote.virtual_y   = 0.0;
    g_remote.entry_x     = loc.x;
    g_remote.entry_y     = loc.y;
    g_remote.entry_guard = 5;
    g_remote.active.store(true, std::memory_order_release);

    hide_cursor();
    CGAssociateMouseAndMouseCursorPosition(false);
    CGWarpMouseCursorPosition(offscreen_point());

    // 진입 위치 비율: 클라이언트가 화면 어느 지점에 커서를 올릴지 결정
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
    pkt.dx = static_cast<int16_t>(g_cfg.client_edge);
    pkt.dy = static_cast<int16_t>(ratio * 32767.0);
    g_engine->send_mouse(pkt);

    BCONN_INFO(CAT_CURSOR,
        "[호스트] 원격 모드 진입: edge=%d, 비율=%.3f, 진입좌표=(%.0f, %.0f)",
        (int)g_cfg.client_edge, ratio, loc.x, loc.y);

    set_status(BCONN_STATUS_REMOTE, "원격 모드");
}

static void leave_remote() {
    BCONN_INFO(CAT_CURSOR, "[호스트] 원격 모드 종료: 복귀 위치=(%.0f, %.0f)",
               return_point().x, return_point().y);

    g_remote.return_guard = 10;
    g_remote.active.store(false, std::memory_order_release);

    MousePacket pkt{};
    pkt.event_type = EVENT_SHOW_CURSOR;
    g_engine->send_mouse(pkt);

    CGWarpMouseCursorPosition(return_point());
    CGAssociateMouseAndMouseCursorPosition(true);
    show_cursor();

    set_status(BCONN_STATUS_CONNECTED, "연결됨");
}

// ── CGEventTap 콜백 ───────────────────────────────────────────────────

// macOS 가 콜백 지연/CPU 부하로 tap 을 자동 비활성화할 수 있다. 재활성화하지 않으면 영구 먹통.
static CGEventRef mouse_callback(CGEventTapProxy, CGEventType type, CGEventRef event, void*) {
    if (type == kCGEventTapDisabledByTimeout ||
        type == kCGEventTapDisabledByUserInput) {
        BCONN_ERROR(CAT_EVENT, "[호스트] 마우스 tap 비활성화 감지 (%u) — 재활성화", (unsigned)type);
        CGEventTapEnable(g_mouse_tap, true);
        return event;
    }
    if (!g_remote.active.load(std::memory_order_acquire)) {
        CGPoint loc = CGEventGetLocation(event);
        if (g_remote.return_guard > 0) { --g_remote.return_guard; return event; }
        if (is_at_trigger_edge(loc)) enter_remote(loc);
        return event;
    }

    bool is_scroll = (type == kCGEventScrollWheel);
    bool is_click  = (type == kCGEventLeftMouseDown   || type == kCGEventLeftMouseUp  ||
                      type == kCGEventRightMouseDown  || type == kCGEventRightMouseUp ||
                      type == kCGEventOtherMouseDown  || type == kCGEventOtherMouseUp);

    // 스크롤 이외의 이벤트는 커서를 항상 offscreen으로 유지
    if (!is_scroll)
        CGWarpMouseCursorPosition(offscreen_point());

    if (is_click) {
        BCONN_DEBUG(CAT_EVENT, "[호스트] 클릭 전송: type=%u entry_guard=%d", (unsigned)type, g_remote.entry_guard);
    }

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
    } else if (!is_click) {
        // 이동/드래그만 delta를 읽는다.
        // 클릭 이벤트의 kCGMouseEventDeltaX/Y는 무의미한 값이 들어올 수 있어
        // 클라이언트의 커서 누적 위치를 오염시킨다.
        int64_t dx = CGEventGetIntegerValueField(event, kCGMouseEventDeltaX);
        int64_t dy = CGEventGetIntegerValueField(event, kCGMouseEventDeltaY);
        pkt.dx = static_cast<int16_t>(std::clamp(dx, -32768LL, 32767LL));
        pkt.dy = static_cast<int16_t>(std::clamp(dy, -32768LL, 32767LL));
    }
    // is_click 시 dx/dy는 0 (zero-initialized) — 클라이언트는 현재 커서 위치에 클릭 주입
    g_engine->send_mouse(pkt);
    return NULL;
}

// kill switch: Ctrl+Opt+Cmd+Esc — 원격 모드가 어떤 이유로도 풀리지 않을 때 사용자 escape hatch.
// keycode 53 = kVK_Escape. 세 modifier 가 모두 눌려있을 때만 동작한다.
static bool is_kill_switch(CGEventType type, CGEventRef event) {
    if (type != kCGEventKeyDown) return false;
    CGKeyCode kc = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    if (kc != 53) return false;
    const CGEventFlags need = kCGEventFlagMaskControl
                            | kCGEventFlagMaskAlternate
                            | kCGEventFlagMaskCommand;
    return (CGEventGetFlags(event) & need) == need;
}

static CGEventRef keyboard_callback(CGEventTapProxy, CGEventType type, CGEventRef event, void*) {
    if (type == kCGEventTapDisabledByTimeout ||
        type == kCGEventTapDisabledByUserInput) {
        BCONN_ERROR(CAT_EVENT, "[호스트] 키보드 tap 비활성화 감지 (%u) — 재활성화", (unsigned)type);
        CGEventTapEnable(g_key_tap, true);
        return event;
    }
    if (is_kill_switch(type, event)) {
        BCONN_INFO(CAT_CURSOR, "[호스트] kill switch 감지 — 강제 로컬 복귀");
        if (g_remote.active.load(std::memory_order_acquire)) leave_remote();
        return NULL;
    }
    if (!g_remote.active.load(std::memory_order_acquire)) return event;

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

// ── RunLoop 기반 이벤트 탭 설정/해제 ─────────────────────────────────

void setup_event_taps() {
    BCONN_DEBUG(CAT_EVENT, "[호스트] 이벤트 탭 설정 시작");

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

    CGEventMask key_mask = CGEventMaskBit(kCGEventKeyDown)  |
                           CGEventMaskBit(kCGEventKeyUp)    |
                           CGEventMaskBit(kCGEventFlagsChanged);
    g_key_tap = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap,
                                 kCGEventTapOptionDefault,
                                 key_mask, keyboard_callback, nullptr);

    if (!g_mouse_tap || !g_key_tap) {
        BCONN_ERROR(CAT_EVENT, "[호스트] 이벤트 탭 생성 실패: 마우스 탭=%p, 키 탭=%p",
                    g_mouse_tap, g_key_tap);
        set_status(BCONN_STATUS_ERROR, "Event Tap 권한 필요 (손쉬운 사용)");
        return;
    }

    auto add_tap = [](CFMachPortRef tap) {
        CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
        CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopCommonModes);
        CGEventTapEnable(tap, true);
        CFRelease(src);
    };
    add_tap(g_mouse_tap);
    add_tap(g_key_tap);

    BCONN_INFO(CAT_EVENT, "[호스트] 이벤트 탭 설정 완료");
}

void teardown_event_taps() {
    if (g_mouse_tap) {
        CGEventTapEnable(g_mouse_tap, false);
        CFRelease(g_mouse_tap);
        g_mouse_tap = nullptr;
    }
    if (g_key_tap) {
        CGEventTapEnable(g_key_tap, false);
        CFRelease(g_key_tap);
        g_key_tap = nullptr;
    }
}

void force_leave_remote(const char* reason) {
    if (!g_remote.active.load(std::memory_order_acquire)) return;

    CFRunLoopRef rl = g_run_loop.load(std::memory_order_acquire);
    if (!rl || rl == (CFRunLoopRef)-1) {
        // RunLoop 이 없거나 setup 실패 — 최소한의 복구만 시도
        BCONN_ERROR(CAT_CURSOR, "[호스트] 원격 모드 강제 종료 폴백 (%s): RunLoop 없음", reason ? reason : "");
        CGAssociateMouseAndMouseCursorPosition(true);
        return;
    }

    BCONN_INFO(CAT_CURSOR, "[호스트] 원격 모드 강제 종료 요청: %s", reason ? reason : "");
    CFRunLoopPerformBlock(rl, kCFRunLoopCommonModes, ^{
        if (g_remote.active.load(std::memory_order_acquire)) leave_remote();
    });
    CFRunLoopWakeUp(rl);
}
