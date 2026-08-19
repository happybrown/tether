/// ClientHandler.cpp — 클라이언트 전용 패킷 수신/커서 주입 구현
#include "ClientHandler.h"
#include "NetworkEngine.h"
#include "TetherLog.h"
#include "CursorUtil.h"
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <algorithm>
#include <chrono>
#include <cstring>

// ── tether_core.cpp 에서 가져오는 전역 ────────────────────────────────

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

// ── 커서 숨김 (offscreen warp) ────────────────────────────────────────

void client_hide_cursor_offscreen() {
    // 전체 화면 영역의 우하단 바깥으로 커서를 warp 한다. hide 와 달리 메뉴바 앱에서도
    // 확실히 동작하며, 물리 마우스 이동 시 OS 가 커서를 도로 화면 안으로 데려온다.
    ScreenBounds s = get_screen_bounds();
    CGWarpMouseCursorPosition(CGPointMake(s.right + 9999.0, s.bottom + 9999.0));
    // warp 좌표가 실제 커서 위치로 반영되도록 연동을 잠깐 유지한다.
    CGAssociateMouseAndMouseCursorPosition(true);
}

void client_show_cursor_onscreen() {
    // 연결 종료·장애 복원 시 사용. offscreen 으로 밀어둔 커서를 화면 중앙으로 되돌려
    // 사용자가 즉시 커서를 찾을 수 있게 한다.
    ScreenBounds s = get_screen_bounds();
    CGWarpMouseCursorPosition(CGPointMake(s.left + s.width() * 0.5,
                                          s.top  + s.height() * 0.5));
    CGAssociateMouseAndMouseCursorPosition(true);
}

// ── 클라이언트 정보 전송 ──────────────────────────────────────────────

void send_client_info() {
    int32_t w = (int32_t)g_client.screen.width();
    int32_t h = (int32_t)g_client.screen.height();
    g_engine->send_client_info(w, h);
}

void client_apply_display_change() {
    ScreenBounds old_screen = g_client.screen;
    g_client.screen = get_screen_bounds();

    TETHER_INFO(CAT_CURSOR,
        "[client] display change detected — recalculating screen: (%.0f x %.0f) → (%.0f x %.0f)",
        old_screen.width(), old_screen.height(),
        g_client.screen.width(), g_client.screen.height());

    // 원격 모드 중이었다면 기존 커서 좌표계가 새 배치에서 무의미하므로 로컬로 복원한다.
    // host 도 자신의 디스플레이 콜백/복귀로 재동기화하며, 이후 재진입 시 새 화면 기준으로
    // SYNC_CURSOR 가 다시 매핑된다.
    if (g_client.pos_init) {
        g_client.pos_init = false;
        if (g_client.cursor_hidden) {
            client_show_cursor_onscreen();
            g_client.cursor_hidden = false;
        }
        TETHER_INFO(CAT_CURSOR, "[client] display change — restoring local from remote mode");
    }

    // 새 화면 크기를 host 에 재전송해 host 의 커서 예측이 올바른 해상도를 쓰게 한다.
    if (g_engine) send_client_info();
}

// ── 수신 패킷 처리 ────────────────────────────────────────────────────

void client_handle_sync(const MousePacket& pkt) {
    g_client.screen = get_screen_bounds();

    TetherEdge host_edge = (TetherEdge)pkt.dx; // host가 어느 방향 경계에서 넘어왔는지
    g_client.entry_edge = (int)host_edge;    // 복귀(반대편 경계 이탈) 판정에 사용
    double ratio = static_cast<double>(pkt.dy) / 32767.0;

    // host 진입 방향에 따라 클라이언트 화면의 반대쪽 경계 안쪽에 커서를 배치
    switch (host_edge) {
    case TETHER_EDGE_RIGHT:
        g_client.cur_x = g_client.screen.left + 2.0;
        g_client.cur_y = g_client.screen.top + ratio * g_client.screen.height();
        break;
    case TETHER_EDGE_LEFT:
        g_client.cur_x = g_client.screen.right - 2.0;
        g_client.cur_y = g_client.screen.top + ratio * g_client.screen.height();
        break;
    case TETHER_EDGE_BOTTOM:
        g_client.cur_x = g_client.screen.left + ratio * g_client.screen.width();
        g_client.cur_y = g_client.screen.top + 2.0;
        break;
    case TETHER_EDGE_TOP:
        g_client.cur_x = g_client.screen.left + ratio * g_client.screen.width();
        g_client.cur_y = g_client.screen.bottom - 2.0;
        break;
    }

    g_client.pos_init = true;
    // 진입 위치로 warp 하면 커서가 화면 안에 다시 나타난다(복귀 시 밖으로 밀었던 것을 되돌림).
    CGWarpMouseCursorPosition(CGPointMake(g_client.cur_x, g_client.cur_y));
    CGAssociateMouseAndMouseCursorPosition(true);
    g_client.cursor_hidden = false;

    TETHER_INFO(CAT_CURSOR,
        "[client] cursor synced: host edge=%d, ratio=%.3f, entry=(%.0f, %.0f), screen=(%.0f, %.0f ~ %.0f, %.0f)",
        (int)host_edge, ratio, g_client.cur_x, g_client.cur_y,
        g_client.screen.left, g_client.screen.top,
        g_client.screen.right, g_client.screen.bottom);

    send_client_info();
}

/// 진입 반대편 경계로 이탈하려는지 검사한다.
/// 커서가 완전히 경계에 닿기 전이라도 경계 근처(EDGE_MARGIN 이내)에서 바깥 방향으로
/// 움직이면 미리 복귀를 트리거한다 (예측적 복귀). 왕복 지연이 사용자 이동과 겹쳐
/// 엣지에서의 멈춤을 없앤다.
/// 이탈 시 host로 LeavePacket(방향 + 복귀 비율 + 잔여 delta)을 보내고
/// 로컬 원격 상태를 해제한 뒤 true를 반환한다.
/// 잔여 delta(이번 이동의 delta)는 host가 복귀 직후 커서를 이어서 움직여
/// 이동 관성을 유지하는 데 쓴다.
static bool client_check_leave(int16_t dx, int16_t dy) {
    const double EDGE_MARGIN = 8.0; // 경계로부터 이 거리 이내 + 바깥 delta 면 이탈로 예측
    const ScreenBounds& s = g_client.screen;
    bool leave = false;
    double ratio = 0.5;

    // 진입 방향의 반대편 경계로 향하는지 판정. 복귀 비율은 진입 축과 동일한 축으로 계산.
    switch ((TetherEdge)g_client.entry_edge) {
    case TETHER_EDGE_RIGHT: // 왼쪽 경계로 이탈 (왼쪽=바깥, dx<0)
        leave = (g_client.cur_x <= s.left + EDGE_MARGIN && dx < 0);
        ratio = s.height() > 0 ? (g_client.cur_y - s.top) / s.height() : 0.5;
        break;
    case TETHER_EDGE_LEFT:  // 오른쪽 경계로 이탈 (오른쪽=바깥, dx>0)
        leave = (g_client.cur_x >= s.right - 1.0 - EDGE_MARGIN && dx > 0);
        ratio = s.height() > 0 ? (g_client.cur_y - s.top) / s.height() : 0.5;
        break;
    case TETHER_EDGE_BOTTOM: // 위쪽 경계로 이탈 (위=바깥, dy<0)
        leave = (g_client.cur_y <= s.top + EDGE_MARGIN && dy < 0);
        ratio = s.width() > 0 ? (g_client.cur_x - s.left) / s.width() : 0.5;
        break;
    case TETHER_EDGE_TOP:    // 아래쪽 경계로 이탈 (아래=바깥, dy>0)
        leave = (g_client.cur_y >= s.bottom - 1.0 - EDGE_MARGIN && dy > 0);
        ratio = s.width() > 0 ? (g_client.cur_x - s.left) / s.width() : 0.5;
        break;
    }
    if (!leave) return false;

    // residual 필드는 프로토콜 호환을 위해 유지하되 0 을 보낸다.
    // host 는 자체 예측으로 복귀 위치를 계산하므로 잔여 delta 를 쓰지 않는다.
    int16_t r = static_cast<int16_t>(std::clamp(ratio, 0.0, 1.0) * 32767.0);
    g_engine->send_leave(static_cast<uint8_t>(g_client.entry_edge), r, 0, 0);

    // 로컬 원격 상태 해제 + 커서를 화면 밖으로 감춘다 (커서 제어가 host 로 넘어감).
    // 이후 host 패킷은 EVENT_SHOW_CURSOR 로 재동기화된다.
    g_client.pos_init = false;
    client_hide_cursor_offscreen();
    g_client.cursor_hidden = true;

    TETHER_INFO(CAT_CURSOR, "[client] leaving opposite edge → requesting return: edge=%d, ratio=%.3f",
               g_client.entry_edge, ratio);
    return true;
}

// 원격 입력이 올 때, 디스플레이가 꺼져 있으면 깨운다.
// 화면만 꺼진(디스플레이 슬립) 상태에서는 CGEventPost 합성 이벤트가 물리 입력과 달리
// 시스템을 깨우지 못하므로 IOPMAssertionDeclareUserActivity 로 사용자 활동을 선언한다.
//
// 성능: 정상 사용(화면 켜짐) 중에는 CGDisplayIsAsleep 검사에서 바로 빠져나가므로
// 매 이동 이벤트당 비용은 이 저렴한 검사 하나뿐이다. 실제 wake 호출은 화면이
// 꺼져 있을 때만, 그마저도 3초 throttle 로 최소화한다.
static void client_wake_display() {
    if (!CGDisplayIsAsleep(CGMainDisplayID())) return;   // 화면 켜져 있으면 아무것도 안 함

    using clock = std::chrono::steady_clock;
    static clock::time_point last{};
    auto now = clock::now();
    if (now - last < std::chrono::seconds(3)) return;    // 꺼진 상태에서만, 3초 throttle
    last = now;

    IOPMAssertionID id = 0;
    IOPMAssertionDeclareUserActivity(
        CFSTR("tether remote input"), kIOPMUserActiveLocal, &id);
    // 선언 후 반환된 assertion 은 시스템이 관리하며, 재호출 시 갱신되므로 별도 release 불필요.
}

void client_handle_move(const MousePacket& pkt) {
    client_wake_display();
    CGEventType etype = (CGEventType)pkt.event_type;

    if (etype == kCGEventScrollWheel) {
        CGEventRef ev = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 2, pkt.dy, pkt.dx);
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
        return;
    }

    bool is_click = (etype == kCGEventLeftMouseDown  || etype == kCGEventLeftMouseUp  ||
                     etype == kCGEventRightMouseDown || etype == kCGEventRightMouseUp ||
                     etype == kCGEventOtherMouseDown || etype == kCGEventOtherMouseUp);

    // 클릭은 위치 이동이 없다. 게다가 host 가 클릭의 dx 필드를 클릭 카운트(clickState)
    // 전달에 재활용하므로, 여기서 dx 를 커서에 누적하면 클릭 시 커서가 밀린다.
    // 따라서 이동/드래그일 때만 delta 를 누적한다.
    if (!is_click) {
        g_client.cur_x = std::clamp(g_client.cur_x + pkt.dx,
                                     g_client.screen.left, g_client.screen.right  - 1.0);
        g_client.cur_y = std::clamp(g_client.cur_y + pkt.dy,
                                     g_client.screen.top,  g_client.screen.bottom - 1.0);
    }

    // 진입 반대편 경계로 이탈하려 하면 host로 복귀 요청 (예측적 — delta 방향으로 판정).
    // 복귀 판정은 실제 커서 위치를 아는 클라이언트가 맡는다 (host delta 누적은 부정확).
    if (etype == kCGEventMouseMoved && client_check_leave(pkt.dx, pkt.dy))
        return;

    CGPoint pos = CGPointMake(g_client.cur_x, g_client.cur_y);

    // 위치 이동 방식:
    // - 순수 이동(mouseMoved): CGEventPost 가 절대위치 pos 를 실어 커서를 옮긴다.
    //   여기서 warp 를 생략해 이동당 Mach RPC(warp+associate)를 제거한다 — 감도/지연의
    //   핵심 개선. post 이벤트의 location 이 곧 커서 위치가 되므로 결과 위치는 동일하다.
    // - 클릭/드래그: warp 로 시스템 커서를 pos 에 확정한 뒤 이벤트를 넣는다. 클릭은
    //   "현재 커서 위치"에서 히트테스트되고 드래그는 대상 추적이 필요해, 위치가 조금이라도
    //   어긋나면 엉뚱한 곳을 클릭하거나 드래그가 끊긴다. 이 경로는 저빈도라 warp 유지가 안전.
    if (etype != kCGEventMouseMoved) {
        CGWarpMouseCursorPosition(pos);
        CGAssociateMouseAndMouseCursorPosition(true);
    }

    CGMouseButton btn = kCGMouseButtonLeft;
    if (etype == kCGEventRightMouseDown || etype == kCGEventRightMouseUp ||
        etype == kCGEventRightMouseDragged)
        btn = kCGMouseButtonRight;
    else if (etype == kCGEventOtherMouseDown || etype == kCGEventOtherMouseUp ||
             etype == kCGEventOtherMouseDragged)
        // host 가 실어 보낸 버튼 번호를 그대로 주입한다 (2=휠, 3=뒤로, 4=앞으로, …).
        // 번호가 없으면(구버전 host, button<2) 기존 동작대로 center 로 처리.
        btn = (pkt.button >= 2) ? (CGMouseButton)pkt.button : kCGMouseButtonCenter;

    CGEventRef ev = CGEventCreateMouseEvent(NULL, etype, pos, btn);
    if (is_click) {
        // host 가 dx 에 실어 보낸 클릭 카운트(1=단일,2=더블,3=트리플)를 그대로 주입한다.
        // 이 값이 있어야 더블클릭=단어선택, 트리플클릭=문단선택이 동작한다.
        int64_t clickState = std::max<int16_t>(1, pkt.dx);
        CGEventSetIntegerValueField(ev, kCGMouseEventClickState, clickState);
    } else {
        // 이동/드래그: 상대 이동량(delta)을 실어 delta 기반 앱(FPS 등)도 반응하게 한다.
        CGEventSetIntegerValueField(ev, kCGMouseEventDeltaX, pkt.dx);
        CGEventSetIntegerValueField(ev, kCGMouseEventDeltaY, pkt.dy);
    }
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);

    if (is_click) {
        TETHER_DEBUG(CAT_EVENT, "[client] click injected: type=%u pos=(%.0f,%.0f) clicks=%d",
                    (unsigned)etype, pos.x, pos.y, (int)pkt.dx);
    }
}

// modifier keycode → 대응하는 CGEventFlags 마스크. modifier 가 아니면 0.
// 좌/우 키가 같은 마스크를 공유한다 (예: 좌 Ctrl 59, 우 Ctrl 62).
static CGEventFlags mask_for_modifier(CGKeyCode kc) {
    switch (kc) {
    case 59: case 62: return kCGEventFlagMaskControl;
    case 56: case 60: return kCGEventFlagMaskShift;
    case 58: case 61: return kCGEventFlagMaskAlternate;
    case 55: case 54: return kCGEventFlagMaskCommand;
    case 57:          return kCGEventFlagMaskAlphaShift; // CapsLock
    case 63:          return kCGEventFlagMaskSecondaryFn;
    default:          return 0;
    }
}

// 원격 복귀 시 client 에 고착됐을 수 있는 modifier 를 전부 해제한다.
// 각 modifier 키에 keyUp 을 주입하고, flags 를 0 으로 밀어 잔여 상태를 지운다.
static void client_reset_modifiers() {
    // 좌/우 Command, Shift, Option, Control + CapsLock + Fn 의 keycode.
    static const CGKeyCode kMods[] = { 55, 54, 56, 60, 58, 61, 59, 62, 57, 63 };
    for (CGKeyCode kc : kMods) {
        CGEventRef up = CGEventCreateKeyboardEvent(NULL, kc, false);
        CGEventSetFlags(up, 0);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(up);
    }
    TETHER_INFO(CAT_EVENT, "[client] all modifiers released (return)");
}

void client_handle_key(const KeyPacket& pkt) {
    if (pkt.event_type == EVENT_RESET_MODIFIERS) {
        client_reset_modifiers();
        return;
    }
    client_wake_display();

    // flagsChanged 는 그 자체로 down/up 구분이 없다. 해당 modifier 의 flag 가
    // 아직 켜져 있으면 눌린 것, 꺼졌으면 뗀 것으로 판정한다.
    // (무조건 up 으로 주입하면 HID 상태와 flags 가 어긋나 Ctrl 등이 눌린 것처럼 고착된다.)
    bool key_down;
    if (pkt.event_type == EVENT_FLAGS_CHANGE) {
        CGEventFlags m = mask_for_modifier(pkt.keycode);
        key_down = m != 0 && ((CGEventFlags)pkt.flags & m) != 0;
    } else {
        key_down = (pkt.event_type == EVENT_KEY_DOWN);
    }

    CGEventRef ev = CGEventCreateKeyboardEvent(NULL, pkt.keycode, key_down);
    CGEventSetFlags(ev, (CGEventFlags)pkt.flags);
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
}
