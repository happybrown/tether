/// ClientHandler.cpp — 클라이언트 전용 패킷 수신/커서 주입 구현
#include "ClientHandler.h"
#include "NetworkEngine.h"
#include "BconnLog.h"
#include "CursorUtil.h"
#include <ApplicationServices/ApplicationServices.h>
#include <algorithm>
#include <cstring>

// ── bconn_core.cpp 에서 가져오는 전역 ────────────────────────────────

extern NetworkEngine* g_engine;
extern ClientState    g_client;



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
    g_engine->send_client_info(w, h);
}

// ── 수신 패킷 처리 ────────────────────────────────────────────────────

void client_handle_sync(const MousePacket& pkt) {
    g_client.screen = get_screen_bounds();

    BconnEdge host_edge = (BconnEdge)pkt.dx; // host가 어느 방향 경계에서 넘어왔는지
    g_client.entry_edge = (int)host_edge;    // 복귀(반대편 경계 이탈) 판정에 사용
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
    // CGWarpMouseCursorPosition 호출 후 macOS가 커서를 숨기는 것을 방지.
    // Warp 직후 마우스 이동 이벤트를 posting해 커서 표시를 강제한다.
    CGAssociateMouseAndMouseCursorPosition(true);
    if (g_client.cursor_hidden) { bconn_show_cursor(); g_client.cursor_hidden = false; }

    BCONN_INFO(CAT_CURSOR,
        "[클라이언트] 커서 동기화 완료: 호스트 경계=%d, 비율=%.3f, 진입좌표=(%.0f, %.0f), 화면영역=(%.0f, %.0f ~ %.0f, %.0f)",
        (int)host_edge, ratio, g_client.cur_x, g_client.cur_y,
        g_client.screen.left, g_client.screen.top,
        g_client.screen.right, g_client.screen.bottom);

    send_client_info();
}

/// 진입 반대편 경계에 커서가 도달했는지 검사한다.
/// 도달 시 host로 EVENT_LEAVE_REMOTE(방향 + 복귀 비율)를 보내고
/// 로컬 원격 상태를 해제한 뒤 true를 반환한다.
static bool client_check_leave() {
    const double th = 1.0; // clamp 로 경계에 붙으므로 작은 임계값
    const ScreenBounds& s = g_client.screen;
    bool leave = false;
    double ratio = 0.5;

    // 진입 방향의 반대편 경계에 닿았는지 판정. 복귀 비율은 진입 축과 동일한 축으로 계산.
    switch ((BconnEdge)g_client.entry_edge) {
    case BCONN_EDGE_RIGHT: // 왼쪽 경계로 이탈
        leave = (g_client.cur_x <= s.left + th);
        ratio = s.height() > 0 ? (g_client.cur_y - s.top) / s.height() : 0.5;
        break;
    case BCONN_EDGE_LEFT:  // 오른쪽 경계로 이탈
        leave = (g_client.cur_x >= s.right - 1.0 - th);
        ratio = s.height() > 0 ? (g_client.cur_y - s.top) / s.height() : 0.5;
        break;
    case BCONN_EDGE_BOTTOM: // 위쪽 경계로 이탈
        leave = (g_client.cur_y <= s.top + th);
        ratio = s.width() > 0 ? (g_client.cur_x - s.left) / s.width() : 0.5;
        break;
    case BCONN_EDGE_TOP:    // 아래쪽 경계로 이탈
        leave = (g_client.cur_y >= s.bottom - 1.0 - th);
        ratio = s.width() > 0 ? (g_client.cur_x - s.left) / s.width() : 0.5;
        break;
    }
    if (!leave) return false;

    MousePacket pkt{};
    pkt.event_type = EVENT_LEAVE_REMOTE;
    pkt.dx = static_cast<int16_t>(g_client.entry_edge);
    pkt.dy = static_cast<int16_t>(std::clamp(ratio, 0.0, 1.0) * 32767.0);
    g_engine->send_mouse(pkt);

    // 로컬 원격 상태 해제 — 이후 host 패킷은 EVENT_SHOW_CURSOR 로 재동기화된다.
    g_client.pos_init = false;
    if (g_client.cursor_hidden) { bconn_show_cursor(); g_client.cursor_hidden = false; }

    BCONN_INFO(CAT_CURSOR, "[클라이언트] 반대편 경계 이탈 → 복귀 요청: edge=%d, 비율=%.3f",
               g_client.entry_edge, ratio);
    return true;
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

    // 진입 반대편 경계에 도달하면 host로 복귀 요청.
    // 복귀 판정은 실제 커서 위치를 아는 클라이언트가 맡는다 (host delta 누적은 부정확).
    if (etype == kCGEventMouseMoved && client_check_leave())
        return;

    CGPoint pos = CGPointMake(g_client.cur_x, g_client.cur_y);

    // kCGEventMouseMoved는 CGEventPost만으로 커서가 이동하지 않는다.
    // CGWarpMouseCursorPosition으로 실제 커서 위치를 먼저 옮겨야 한다.
    // Warp 후 커서 숨김 방지를 위해 CGAssociateMouseAndMouseCursorPosition 재확인.
    if (etype == kCGEventMouseMoved) {
        CGWarpMouseCursorPosition(pos);
        CGAssociateMouseAndMouseCursorPosition(true);
    }

    CGMouseButton btn = kCGMouseButtonLeft;
    if (etype == kCGEventRightMouseDown || etype == kCGEventRightMouseUp ||
        etype == kCGEventRightMouseDragged)
        btn = kCGMouseButtonRight;
    else if (etype == kCGEventOtherMouseDown || etype == kCGEventOtherMouseUp ||
             etype == kCGEventOtherMouseDragged)
        btn = kCGMouseButtonCenter;

    bool is_click = (etype == kCGEventLeftMouseDown  || etype == kCGEventLeftMouseUp  ||
                     etype == kCGEventRightMouseDown || etype == kCGEventRightMouseUp ||
                     etype == kCGEventOtherMouseDown || etype == kCGEventOtherMouseUp);

    CGEventRef ev = CGEventCreateMouseEvent(NULL, etype, pos, btn);
    CGEventSetIntegerValueField(ev, kCGMouseEventDeltaX, pkt.dx);
    CGEventSetIntegerValueField(ev, kCGMouseEventDeltaY, pkt.dy);
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);

    if (is_click) {
        BCONN_DEBUG(CAT_EVENT, "[클라이언트] 클릭 주입: type=%u pos=(%.0f,%.0f)", (unsigned)etype, pos.x, pos.y);
    }
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
