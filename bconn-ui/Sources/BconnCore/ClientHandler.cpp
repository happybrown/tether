/// ClientHandler.cpp — 클라이언트 전용 패킷 수신/커서 주입 구현
#include "ClientHandler.h"
#include "NetworkEngine.h"
#include "BconnLog.h"
#include <ApplicationServices/ApplicationServices.h>
#include <objc/objc-runtime.h>
#include <algorithm>
#include <cstring>

// ── bconn_core.cpp 에서 가져오는 전역 ────────────────────────────────

extern NetworkEngine* g_engine;
extern ClientState    g_client;

// ── 커서 유틸 ─────────────────────────────────────────────────────────

static void show_cursor() {
    id cls = (id)objc_getClass("NSCursor");
    ((void(*)(id,SEL))objc_msgSend)(cls, sel_registerName("unhide"));
}

// ── 화면 경계 ─────────────────────────────────────────────────────────

ScreenBounds get_screen_bounds() {
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

// ── 클라이언트 정보 전송 ──────────────────────────────────────────────

void send_client_info() {
    int32_t w = (int32_t)g_client.screen.width();
    int32_t h = (int32_t)g_client.screen.height();

    // 32bit 값을 2개의 int16_t 필드로 분할해 전송 (dx=하위16bit, dy=상위16bit)
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

// ── 수신 패킷 처리 ────────────────────────────────────────────────────

void client_handle_sync(const MousePacket& pkt) {
    g_client.screen = get_screen_bounds();

    BconnEdge host_edge = (BconnEdge)pkt.dx; // host가 어느 방향 경계에서 넘어왔는지
    double ratio = static_cast<double>(pkt.dy) / 32767.0;

    // host 진입 방향에 따라 클라이언트 화면의 반대쪽 경계 안쪽에 커서를 배치
    switch (host_edge) {
    case BCONN_EDGE_RIGHT:
        g_client.cur_x = g_client.screen.left + 2.0;
        g_client.cur_y = g_client.screen.top + ratio * g_client.screen.height();
        break;
    case BCONN_EDGE_LEFT:
        g_client.cur_x = g_client.screen.right - 2.0;
        g_client.cur_y = g_client.screen.top + ratio * g_client.screen.height();
        break;
    case BCONN_EDGE_BOTTOM:
        g_client.cur_x = g_client.screen.left + ratio * g_client.screen.width();
        g_client.cur_y = g_client.screen.top + 2.0;
        break;
    case BCONN_EDGE_TOP:
        g_client.cur_x = g_client.screen.left + ratio * g_client.screen.width();
        g_client.cur_y = g_client.screen.bottom - 2.0;
        break;
    }

    g_client.pos_init = true;
    CGWarpMouseCursorPosition(CGPointMake(g_client.cur_x, g_client.cur_y));
    if (g_client.cursor_hidden) { show_cursor(); g_client.cursor_hidden = false; }

    BCONN_INFO(CAT_CURSOR,
        "[클라이언트] 커서 동기화 완료: 호스트 경계=%d, 비율=%.3f, 진입좌표=(%.0f, %.0f), 화면영역=(%.0f, %.0f ~ %.0f, %.0f)",
        (int)host_edge, ratio, g_client.cur_x, g_client.cur_y,
        g_client.screen.left, g_client.screen.top,
        g_client.screen.right, g_client.screen.bottom);

    send_client_info();
}

void client_handle_move(const MousePacket& pkt) {
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

    CGPoint pos = CGPointMake(g_client.cur_x, g_client.cur_y);

    // kCGEventMouseMoved는 CGEventPost만으로 커서가 이동하지 않는다.
    // CGWarpMouseCursorPosition으로 실제 커서 위치를 먼저 옮겨야 한다.
    if (etype == kCGEventMouseMoved)
        CGWarpMouseCursorPosition(pos);

    CGMouseButton btn = kCGMouseButtonLeft;
    if (etype == kCGEventRightMouseDown || etype == kCGEventRightMouseUp ||
        etype == kCGEventRightMouseDragged)
        btn = kCGMouseButtonRight;
    else if (etype == kCGEventOtherMouseDown || etype == kCGEventOtherMouseUp ||
             etype == kCGEventOtherMouseDragged)
        btn = kCGMouseButtonCenter;

    CGEventRef ev = CGEventCreateMouseEvent(NULL, etype, pos, btn);
    CGEventSetIntegerValueField(ev, kCGMouseEventDeltaX, pkt.dx);
    CGEventSetIntegerValueField(ev, kCGMouseEventDeltaY, pkt.dy);
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
}

void client_handle_key(const KeyPacket& pkt) {
    bool key_down = (pkt.event_type == EVENT_KEY_DOWN);
    // EVENT_FLAGS_CHANGE는 keyDown=false로 전달해야 modifier 키 상태가 정상 반영된다.
    CGEventRef ev = CGEventCreateKeyboardEvent(NULL, pkt.keycode,
                        pkt.event_type == EVENT_FLAGS_CHANGE ? false : key_down);
    CGEventSetFlags(ev, (CGEventFlags)pkt.flags);
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
}
