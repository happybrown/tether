/// EventTap.cpp — 호스트 전용 CGEventTap 구현
///
/// 이 파일의 함수들은 호스트(is_host=true) 모드에서만 사용된다.
/// 클라이언트 수신 처리는 ClientHandler.cpp 에 있다.
#include "EventTap.h"
#include "EdgeGeometry.h"
#include "NetworkEngine.h"
#include "TetherLog.h"
#include "CursorUtil.h"
#include <ApplicationServices/ApplicationServices.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>
#include <string>
#include <cstring>

// ── tether_core.cpp 에서 가져오는 전역 ────────────────────────────────

extern TetherConfig              g_cfg;
extern NetworkEngine*           g_engine;
extern CFMachPortRef            g_mouse_tap;
extern CFMachPortRef            g_key_tap;
extern std::atomic<int32_t>     g_client_width;
extern std::atomic<int32_t>     g_client_height;
extern std::atomic<double>      g_client_offset;  // client 화면 정렬 오프셋 (비율)
extern RemoteState              g_remote;
extern std::atomic<CFRunLoopRef> g_run_loop;
extern std::atomic<bool>        g_taps_active;
extern std::atomic<bool>        g_predict_leave;  // host 자체 복귀 예측 on/off

void set_status(TetherStatus s, const char* msg);



// ── 디스플레이 레이아웃 ──────────────────────────────────────────────

// 경계 기하는 EdgeGeometry(순수 함수)가 계산한다. 여기서는 CoreGraphics 로 현재
// 디스플레이 목록을 읽어 넘기고 결과를 보관하는 역할만 한다.
struct DisplayLayout {
    EdgeGeometry geo;
    double height = 0.0;   // 전체 데스크톱 높이 (offscreen/복귀 계산에 쓰임)

    double trigger_edge()  const { return geo.trigger_edge; }
    bool   interior_edge() const { return geo.interior_edge; }
    double left()   const { return geo.left; }
    double right()  const { return geo.right; }
    double top()    const { return geo.top; }
    double bottom() const { return geo.bottom; }
};

static DisplayLayout g_layout;

// 사용자가 지정한 전환 기준 화면의 UUID. 빈 문자열이면 자동(가장 바깥 경계).
// RunLoop 스레드에서만 쓰이며, 변경은 apply_trigger_display 가 위임한다.
static char g_trigger_uuid[40] = {};

/// 디스플레이의 UUID 문자열을 얻는다. 실패 시 빈 문자열.
static void display_uuid(CGDirectDisplayID id, char out[40]) {
    out[0] = '\0';
    CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(id);
    if (!uuid) return;
    CFStringRef s = CFUUIDCreateString(kCFAllocatorDefault, uuid);
    if (s) {
        CFStringGetCString(s, out, 40, kCFStringEncodingUTF8);
        CFRelease(s);
    }
    CFRelease(uuid);
}

void set_trigger_display_uuid(const char* uuid) {
    if (uuid) {
        strncpy(g_trigger_uuid, uuid, sizeof(g_trigger_uuid) - 1);
        g_trigger_uuid[sizeof(g_trigger_uuid) - 1] = '\0';
    } else {
        g_trigger_uuid[0] = '\0';
    }
}

void refresh_layout(TetherEdge edge) {
    CGDirectDisplayID ids[16];
    uint32_t count = 0;
    CGGetActiveDisplayList(16, ids, &count);

    TETHER_DEBUG(CAT_CURSOR, "[호스트] 디스플레이 레이아웃 새로고침: 디스플레이 수=%u, 경계 방향=%d", count, (int)edge);

    // 디스플레이 rect 수집 + 기준 화면 인덱스 확인 (UUID 매칭)
    std::vector<CGRect> rects;
    rects.reserve(count);
    int trigger_index = -1;
    for (uint32_t i = 0; i < count; ++i) {
        CGRect b = CGDisplayBounds(ids[i]);
        rects.push_back(b);
        if (g_trigger_uuid[0] != '\0' && trigger_index < 0) {
            char u[40];
            display_uuid(ids[i], u);
            if (strcmp(u, g_trigger_uuid) == 0) trigger_index = (int)i;
        }
        TETHER_DEBUG(CAT_CURSOR, "[호스트] 디스플레이 %u: 위치=(%.0f, %.0f), 크기=(%.0f x %.0f)",
                    i, b.origin.x, b.origin.y, b.size.width, b.size.height);
    }

    if (g_trigger_uuid[0] != '\0' && trigger_index < 0) {
        TETHER_INFO(CAT_CURSOR,
            "[호스트] 기준 화면(uuid=%s)을 찾지 못함 — 자동(최외곽 경계)으로 폴백", g_trigger_uuid);
    }

    g_layout.geo = compute_edge_geometry(rects.data(), rects.size(), edge, trigger_index);
    g_layout.height = g_layout.bottom() - g_layout.top();

    // 인접 구간(neighbor_lo~hi)은 커서가 통과하는 범위다. 전환이 안 될 때 이 로그로
    // "근접 판정 구간인지 통과 판정 구간인지"를 바로 알 수 있다.
    if (g_layout.interior_edge()) {
        TETHER_INFO(CAT_CURSOR,
            "[호스트] 레이아웃 갱신: 트리거 에지=%.0f, 인접 구간=%.0f~%.0f (이 구간은 통과 판정), "
            "에지 화면 수=%zu, 전체 영역=(%.0f, %.0f ~ %.0f, %.0f)",
            g_layout.trigger_edge(), g_layout.geo.neighbor_lo, g_layout.geo.neighbor_hi,
            g_layout.geo.edge_displays.size(),
            g_layout.left(), g_layout.top(), g_layout.right(), g_layout.bottom());
    } else {
        TETHER_INFO(CAT_CURSOR,
            "[호스트] 레이아웃 갱신: 트리거 에지=%.0f, 인접 화면 없음 (근접 판정), "
            "에지 화면 수=%zu, 전체 영역=(%.0f, %.0f ~ %.0f, %.0f)",
            g_layout.trigger_edge(), g_layout.geo.edge_displays.size(),
            g_layout.left(), g_layout.top(), g_layout.right(), g_layout.bottom());
    }
}

/// 현재 커서 위치가 전환 경계에 도달했는지 검사한다 (판정 로직은 EdgeGeometry 공용).
/// out_by_crossing: 통과 판정으로 트리거됐는지 (진입점 선택에 쓴다).
static bool is_at_trigger_edge(CGPoint loc, CGPoint prev, bool prev_valid,
                               bool* out_by_crossing) {
    return is_edge_reached(g_layout.geo, g_cfg.client_edge, loc, prev, prev_valid,
                           3.0, out_by_crossing);
}

/// 진입 좌표가 속한 경계 디스플레이를 반환한다. 못 찾으면 전체 데스크톱을 대체로 반환.
/// 진입/복귀 비율을 이 모니터 기준으로 계산해 클라이언트 화면과 일관되게 매핑한다.
static CGRect edge_display_at(CGPoint loc) {
    for (const CGRect& b : g_layout.geo.edge_displays) {
        bool in_range = false;
        switch (g_cfg.client_edge) {
        case TETHER_EDGE_RIGHT:
        case TETHER_EDGE_LEFT:
            in_range = (loc.y >= b.origin.y && loc.y <= b.origin.y + b.size.height);
            break;
        case TETHER_EDGE_TOP:
        case TETHER_EDGE_BOTTOM:
            in_range = (loc.x >= b.origin.x && loc.x <= b.origin.x + b.size.width);
            break;
        }
        if (in_range) return b;
    }
    // 기준 화면이 지정된 경우 edge_displays 에 그 화면만 있으므로 첫 항목이 정답에 가깝다.
    if (!g_layout.geo.edge_displays.empty()) return g_layout.geo.edge_displays.front();
    return CGRectMake(g_layout.left(), g_layout.top(),
                      g_layout.right() - g_layout.left(), g_layout.height);
}

// ── 원격 모드 상태 ────────────────────────────────────────────────────

// RemoteState 정의는 EventTap.h 에 있다.
// g_remote 인스턴스는 tether_core.cpp 에 있고, 아래에서 extern으로 참조한다.

// 로컬(비원격) 상태의 직전 커서 위치. 내부 경계 통과 판정에 쓴다.
// mouse_callback(RunLoop 스레드)에서만 접근한다.
static CGPoint g_last_local_loc{};
static bool    g_last_local_valid = false;

/// 원격 진입 시 커서를 화면 밖으로 밀어두는 위치.
/// CGEventTap이 계속 실행되는 동안 커서가 실제로 보이지 않도록 한다.
/// 데스크톱 전체 경계 기준으로 밀어낸다. 기준 화면이 데스크톱 내부일 때
/// trigger_edge 로 밀면 옆 디스플레이 안이라 커서가 그대로 보이므로,
/// 항상 전체 영역(left/right/top/bottom) 바깥을 쓴다.
static CGPoint offscreen_point() {
    switch (g_cfg.client_edge) {
    case TETHER_EDGE_RIGHT:  return CGPointMake(g_layout.right(),          99999.0);
    case TETHER_EDGE_LEFT:   return CGPointMake(g_layout.left() - 9999.0,  g_layout.top());
    case TETHER_EDGE_TOP:    return CGPointMake(99999.0,                 g_layout.top() - 9999.0);
    case TETHER_EDGE_BOTTOM: return CGPointMake(99999.0,                 g_layout.bottom());
    }
    return CGPointMake(g_layout.right(), 99999.0);
}

/// 복귀 시 커서를 경계 바로 안쪽으로 되돌린다.
/// 경계와 평행한 축 좌표(return_x/return_y)는 클라이언트가 보낸 복귀 비율로 정해진다.
/// offset 은 재진입 임계값(is_at_trigger_edge 의 3px)보다 크게만 두면 된다.
/// 즉시 재진입은 return_time 기반 200ms 차단으로 별도로 막으므로 작게 유지해
/// 복귀 커서가 경계에서 멀리 튀지 않도록 한다.
static CGPoint return_point() {
    const double offset = 8.0;
    // 진입했던 기준 화면의 경계를 쓴다. 내부 경계(맥북+외장에서 맥북 오른쪽 등)에서
    // 전역 trigger_edge 를 쓰면 복귀 커서가 엉뚱한 디스플레이에 놓인다.
    const CGRect& d = g_remote.entry_display;
    double e = g_layout.trigger_edge();
    if (!CGRectIsEmpty(d)) {
        switch (g_cfg.client_edge) {
        case TETHER_EDGE_RIGHT:  e = d.origin.x + d.size.width;  break;
        case TETHER_EDGE_LEFT:   e = d.origin.x;                 break;
        case TETHER_EDGE_TOP:    e = d.origin.y;                 break;
        case TETHER_EDGE_BOTTOM: e = d.origin.y + d.size.height; break;
        }
    }
    switch (g_cfg.client_edge) {
    case TETHER_EDGE_RIGHT:  return CGPointMake(e - offset, g_remote.return_y);
    case TETHER_EDGE_LEFT:   return CGPointMake(e + offset, g_remote.return_y);
    case TETHER_EDGE_TOP:    return CGPointMake(g_remote.return_x, e + offset);
    case TETHER_EDGE_BOTTOM: return CGPointMake(g_remote.return_x, e - offset);
    }
    return CGPointMake(e - offset, g_remote.return_y);
}

/// 클라이언트가 보낸 복귀 비율(0~1, client 화면 기준)을 host 화면 좌표로 변환해
/// return_x/return_y 에 저장한다.
/// 진입했던 경계 모니터(entry_display) 기준으로 역변환해 진입/복귀 축을 일치시킨다.
/// RIGHT/LEFT 진입은 세로 비율 → return_y, TOP/BOTTOM 진입은 가로 비율 → return_x.
static void set_return_from_ratio(double ratio) {
    // 정렬 오프셋 역변환: 진입 시 host→client 로 -offset 했으므로 복귀는 +offset.
    ratio = std::clamp(ratio + g_client_offset.load(std::memory_order_relaxed), 0.0, 1.0);
    const CGRect& d = g_remote.entry_display;
    switch (g_cfg.client_edge) {
    case TETHER_EDGE_RIGHT:
    case TETHER_EDGE_LEFT:
        g_remote.return_y = d.origin.y + ratio * d.size.height;
        break;
    case TETHER_EDGE_TOP:
    case TETHER_EDGE_BOTTOM:
        g_remote.return_x = d.origin.x + ratio * d.size.width;
        break;
    }
}

/// 원격 진입 시점에 host 에서 이미 눌려 있던 modifier 를 client 에 재생한다.
///
/// keyboard_callback 은 원격이 아닐 때 이벤트를 로컬로 흘려보내므로, Ctrl 을 누른 채
/// 경계를 넘으면 client 는 그 down 을 받지 못한 채 이후 키의 flags 에만 Ctrl 이 실린다.
/// 그 결과 "누르지 않은 Ctrl 이 눌린 것처럼" 동작하므로, 진입 때 실제 상태를 맞춰준다.
static void sync_modifiers_on_enter() {
    // 진입 전 잔여 상태를 먼저 지워 양쪽 기준점을 0 으로 맞춘다.
    KeyPacket reset{};
    reset.event_type = EVENT_RESET_MODIFIERS;
    for (int i = 0; i < 3; ++i) g_engine->send_key(reset);

    CGEventFlags state = CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);

    // 실제 눌려 있는 modifier 만 down 으로 재생한다. flags 는 누적해서 실어
    // 실제 flagsChanged 이벤트 열과 같은 형태가 되게 한다.
    static const struct { CGEventFlags mask; CGKeyCode kc; } kMods[] = {
        { kCGEventFlagMaskControl,   59 },
        { kCGEventFlagMaskShift,     56 },
        { kCGEventFlagMaskAlternate, 58 },
        { kCGEventFlagMaskCommand,   55 },
    };

    CGEventFlags acc = 0;
    for (const auto& m : kMods) {
        if (!(state & m.mask)) continue;
        acc |= m.mask;
        KeyPacket kp{};
        kp.keycode    = m.kc;
        kp.flags      = static_cast<uint32_t>(acc);
        kp.event_type = EVENT_FLAGS_CHANGE;
        for (int i = 0; i < 2; ++i) g_engine->send_key(kp);
    }

    if (acc) {
        TETHER_INFO(CAT_EVENT, "[호스트] 진입 시 modifier 동기화: flags=0x%llx",
                    (unsigned long long)acc);
    }
}

static void enter_remote(CGPoint loc) {
    // peer가 연결되어 있지 않으면 원격 모드 진입을 차단한다.
    // 연결 끊김 상태에서 경계 진입 시 먹통 방지.
    if (!g_engine || !g_engine->is_peer_known()) {
        TETHER_DEBUG(CAT_CURSOR, "[호스트] 원격 모드 진입 차단: peer 미연결");
        return;
    }

    g_remote.entry_x     = loc.x;
    g_remote.entry_y     = loc.y;
    // 복귀 좌표 기본값을 진입 위치로 둔다. 클라이언트 복귀 패킷이 오면 그 비율로 갱신된다.
    g_remote.return_x    = loc.x;
    g_remote.return_y    = loc.y;
    // 진입한 경계 모니터를 기억한다 — 진입/복귀 비율의 기준 좌표계.
    g_remote.entry_display = edge_display_at(loc);
    g_remote.entry_guard = 10;
    g_remote.active.store(true, std::memory_order_release);

    tether_hide_cursor();
    CGAssociateMouseAndMouseCursorPosition(false);
    CGWarpMouseCursorPosition(offscreen_point());

    // 진입 위치 비율: 진입한 경계 모니터 기준으로 계산해 클라이언트 화면과 대응시킨다.
    const CGRect& d = g_remote.entry_display;
    double ratio = 0.5;
    switch (g_cfg.client_edge) {
    case TETHER_EDGE_RIGHT:
    case TETHER_EDGE_LEFT:
        ratio = (d.size.height > 0)
            ? std::clamp((loc.y - d.origin.y) / d.size.height, 0.0, 1.0)
            : 0.5;
        break;
    case TETHER_EDGE_TOP:
    case TETHER_EDGE_BOTTOM:
        ratio = (d.size.width > 0)
            ? std::clamp((loc.x - d.origin.x) / d.size.width, 0.0, 1.0)
            : 0.5;
        break;
    }

    // 정렬 오프셋 적용: client 화면이 host 보다 아래/오른쪽에 있으면(양수) host 의 같은
    // 위치가 client 화면에서는 그만큼 위/왼쪽에 대응한다. client 로 보내는 비율과
    // 가상 커서 초기화는 이 client 기준 비율을 쓴다 (복귀 시 역변환은 set_return_from_ratio).
    double client_ratio = std::clamp(
        ratio - g_client_offset.load(std::memory_order_relaxed), 0.0, 1.0);

    // 클라이언트 가상 커서 위치를 client_handle_sync 와 동일하게 초기화한다.
    // client 좌표계는 (0,0)~(width,height) 로 가정 (client 는 화면 원점 기준으로 clamp).
    double cw = (double)g_client_width.load(std::memory_order_relaxed);
    double ch = (double)g_client_height.load(std::memory_order_relaxed);
    switch (g_cfg.client_edge) {
    case TETHER_EDGE_RIGHT:  g_remote.client_x = 2.0;      g_remote.client_y = client_ratio * ch; break;
    case TETHER_EDGE_LEFT:   g_remote.client_x = cw - 2.0; g_remote.client_y = client_ratio * ch; break;
    case TETHER_EDGE_BOTTOM: g_remote.client_x = client_ratio * cw; g_remote.client_y = 2.0;      break;
    case TETHER_EDGE_TOP:    g_remote.client_x = client_ratio * cw; g_remote.client_y = ch - 2.0; break;
    }

    MousePacket pkt{};
    pkt.event_type = EVENT_SYNC_CURSOR;
    pkt.dx = static_cast<int16_t>(g_cfg.client_edge);
    pkt.dy = static_cast<int16_t>(client_ratio * 32767.0);
    // 상태 전환 신호는 유실 시 양측 커서 상태가 어긋나므로 3회 연속 전송해 방어한다.
    // (지속 유실·엣지케이스는 PONG 의 active 비트 자가교정이 백업)
    for (int i = 0; i < 3; ++i) g_engine->send_mouse(pkt);

    // SYNC_CURSOR 뒤에 보낸다 — client 의 키 처리 가드(pos_init)가 먼저 열려야 한다.
    sync_modifiers_on_enter();

    TETHER_INFO(CAT_CURSOR,
        "[호스트] 원격 모드 진입: edge=%d, 비율=%.3f, 진입좌표=(%.0f, %.0f)",
        (int)g_cfg.client_edge, ratio, loc.x, loc.y);

    set_status(TETHER_STATUS_REMOTE, "원격 모드");
}

static void leave_remote() {
    CGPoint rp = return_point();
    TETHER_INFO(CAT_CURSOR, "[호스트] 원격 모드 종료: 복귀 위치=(%.0f, %.0f)", rp.x, rp.y);

    // 커서 복귀 + 연동 복구를 active=false 보다 먼저 수행한다.
    // CGWarp/CGAssociate 는 비동기 Mach 메시지라 실제 반영까지 수십ms 지연이 있다.
    // active=false 이후 들어오는 클릭이 offscreen 위치에 dispatch되는 것을 막기 위해
    // 시스템에 먼저 전달해 둔다.
    CGWarpMouseCursorPosition(rp);
    CGAssociateMouseAndMouseCursorPosition(true);
    tether_show_cursor();

    // 통과 판정 기준점을 복귀 위치로 리셋한다. 원격 중 남아 있던 옛 위치를 그대로 쓰면
    // 복귀 직후 첫 이동이 경계를 "통과"한 것으로 오판될 수 있다 (내부 경계 모드).
    g_last_local_loc = rp;
    g_last_local_valid = true;

    // 복귀 시각 기록 — mouse_callback 에서 200ms 동안 재진입을 차단한다.
    g_remote.return_time = std::chrono::steady_clock::now();
    g_remote.active.store(false, std::memory_order_release);

    MousePacket pkt{};
    pkt.event_type = EVENT_SHOW_CURSOR;
    // 복귀 신호 유실 시 client 가 원격 상태로 남아 양측에 커서가 동시 표시된다.
    // 3회 연속 전송으로 즉각 유실을 방어한다 (PONG active 비트 자가교정이 백업).
    for (int i = 0; i < 3; ++i) g_engine->send_mouse(pkt);

    // 복귀 시 client 에 남아있을 수 있는 modifier(Ctrl/Cmd/Shift/Opt)를 전부 해제한다.
    // 원격 중 modifier down 을 보낸 뒤 그 up 이 로컬 복귀 후 발생하면(또는 UDP 유실 시)
    // client 에 키가 고착되므로, 복귀 때마다 명시적으로 리셋해 자동 복구시킨다.
    KeyPacket kp{};
    kp.event_type = EVENT_RESET_MODIFIERS;
    for (int i = 0; i < 3; ++i) g_engine->send_key(kp);

    set_status(TETHER_STATUS_CONNECTED, "연결됨");
}

/// host 가 client 가상 커서 위치를 delta 로 누적/clamp 하고, 반대편 경계로 이탈하려는지
/// 예측한다. 이탈 시 그 위치의 복귀 비율을 계산해 즉시 leave_remote 한다 (왕복 지연 없음).
/// client 의 client_check_leave 와 동일한 기준으로 판정해 양측이 같은 지점에서 복귀한다.
static bool host_predict_leave(int16_t dx, int16_t dy) {
    const double EDGE_MARGIN = 8.0;
    double cw = (double)g_client_width.load(std::memory_order_relaxed);
    double ch = (double)g_client_height.load(std::memory_order_relaxed);

    // 직전 실제(clamp된) 위치. 이탈은 "이미 경계 근방에 도달해 있는" 상태에서만 인정한다.
    double px = g_remote.client_x;
    double py = g_remote.client_y;

    double nx = px + dx;
    double ny = py + dy;

    // 이탈 판정은 clamp 전 nx/ny 가 아니라 "직전 위치가 경계 margin 안"인지로 한다.
    // 중앙에서 큰 delta 한 방(마구 흔들기)에 nx 가 경계를 훌쩍 넘어도, 커서는 실제로는
    // 아직 경계에 붙지 않았으므로 복귀시키지 않는다. client 는 clamp 된 위치로 판정하므로
    // 이렇게 해야 양측 기준이 일치하고 오탐(의도치 않은 복귀)이 사라진다.
    bool leave = false;
    double ratio = 0.5;   // 경계 평행 축 비율 (복귀 위치)
    switch (g_cfg.client_edge) {
    case TETHER_EDGE_RIGHT:  // 왼쪽 경계로 이탈
        leave = (px <= EDGE_MARGIN && dx < 0);
        ratio = ch > 0 ? std::clamp(ny, 0.0, ch) / ch : 0.5;
        break;
    case TETHER_EDGE_LEFT:   // 오른쪽 경계로 이탈
        leave = (px >= cw - 1.0 - EDGE_MARGIN && dx > 0);
        ratio = ch > 0 ? std::clamp(ny, 0.0, ch) / ch : 0.5;
        break;
    case TETHER_EDGE_BOTTOM: // 위쪽 경계로 이탈
        leave = (py <= EDGE_MARGIN && dy < 0);
        ratio = cw > 0 ? std::clamp(nx, 0.0, cw) / cw : 0.5;
        break;
    case TETHER_EDGE_TOP:    // 아래쪽 경계로 이탈
        leave = (py >= ch - 1.0 - EDGE_MARGIN && dy > 0);
        ratio = cw > 0 ? std::clamp(nx, 0.0, cw) / cw : 0.5;
        break;
    }
    // 예측이 꺼져 있으면 복귀 판정은 하지 않되, 가상 위치 누적은 유지한다.
    // (LeavePacket 안전망이 정상 복귀시키고, 실행 중 다시 켰을 때 위치가 정확해진다.)
    if (!leave || !g_predict_leave.load(std::memory_order_relaxed)) {
        g_remote.client_x = std::clamp(nx, 0.0, cw - 1.0);
        g_remote.client_y = std::clamp(ny, 0.0, ch - 1.0);
        return false;
    }

    set_return_from_ratio(std::clamp(ratio, 0.0, 1.0));
    TETHER_INFO(CAT_CURSOR, "[호스트] 자체 복귀 예측: 비율=%.3f", ratio);
    leave_remote();
    return true;
}

// ── CGEventTap 콜백 ───────────────────────────────────────────────────

// macOS 가 콜백 지연/CPU 부하로 tap 을 자동 비활성화할 수 있다. 재활성화하지 않으면 영구 먹통.
static CGEventRef mouse_callback(CGEventTapProxy, CGEventType type, CGEventRef event, void*) {
    if (type == kCGEventTapDisabledByTimeout ||
        type == kCGEventTapDisabledByUserInput) {
        TETHER_ERROR(CAT_EVENT, "[호스트] 마우스 tap 비활성화 감지 (%u) — 재활성화", (unsigned)type);
        CGEventTapEnable(g_mouse_tap, true);
        return event;
    }
    if (!g_remote.active.load(std::memory_order_acquire)) {
        CGPoint loc = CGEventGetLocation(event);
        // 내부 경계 통과 판정용 직전 위치. 원격이 아닐 때 매 이벤트마다 갱신한다.
        CGPoint prev = g_last_local_loc;
        bool prev_valid = g_last_local_valid;
        g_last_local_loc = loc;
        g_last_local_valid = true;

        // 복귀 직후 짧게 재진입을 차단한다. 예측적 복귀로 커서가 경계에서 떨어진
        // 위치(return_point offset)에 놓이므로 warp 반영에 필요한 최소 시간만 둔다.
        // 값이 크면 복귀 직후 경계 근처 이동이 억눌려 끊김으로 느껴진다.
        auto elapsed = std::chrono::steady_clock::now() - g_remote.return_time;
        if (elapsed < std::chrono::milliseconds(60)) return event;

        // 드래그 중에는 원격 진입 차단 — 의도치 않은 전환 방지
        bool is_drag = (type == kCGEventLeftMouseDragged  ||
                        type == kCGEventRightMouseDragged ||
                        type == kCGEventOtherMouseDragged);
        if (is_drag) return event;

        bool by_crossing = false;
        if (is_at_trigger_edge(loc, prev, prev_valid, &by_crossing)) {
            // 통과로 트리거된 경우 진입점은 경계선상의 prev 다 (loc 은 이미 지나침).
            enter_remote(by_crossing ? prev : loc);
        }
        return event;
    }

    bool is_scroll = (type == kCGEventScrollWheel);
    bool is_click  = (type == kCGEventLeftMouseDown   || type == kCGEventLeftMouseUp  ||
                      type == kCGEventRightMouseDown  || type == kCGEventRightMouseUp ||
                      type == kCGEventOtherMouseDown  || type == kCGEventOtherMouseUp);

    // 커서 offscreen 유지 보정 (스크롤 제외).
    // CGAssociate(false) 로 연동을 끊어도 시스템 커서의 화면상 위치 자체는 남아 있어,
    // macOS 가 커서를 화면 안으로 되돌리면 host 커서가 물리 마우스를 따라 보이게 된다.
    //
    // 예전엔 매 이벤트마다 무조건 offscreen 으로 warp 했으나, CGWarpMouseCursorPosition 은
    // WindowServer 로 가는 동기 Mach RPC 라 active tap 콜백을 매번 블로킹해 감도/지연의
    // 주범이었다. 실제로 커서가 화면 안으로 튕겨 돌아오는 이벤트는 드물고 대부분은 이미
    // offscreen 상태이므로, 이벤트에 실려온 현재 좌표(loc, 추가 RPC 없음)가 데스크톱 경계
    // 안에 들어와 있을 때만 다시 밀어낸다. 절대다수의 이동에서 warp RPC 가 제거된다.
    if (!is_scroll) {
        CGPoint loc = CGEventGetLocation(event);
        bool cursor_visible = loc.x >= g_layout.left() && loc.x <= g_layout.right() &&
                              loc.y >= g_layout.top()  && loc.y <= g_layout.bottom();
        if (cursor_visible)
            CGWarpMouseCursorPosition(offscreen_point());
    }

    if (is_click) {
        TETHER_DEBUG(CAT_EVENT, "[호스트] 클릭 전송: type=%u entry_guard=%d", (unsigned)type, g_remote.entry_guard);
    }

    // 진입 직후 몇 이벤트는 host 잔여 delta 이므로 client 로 보내지 않는다.
    // 복귀 판정은 실제 커서 위치를 아는 client 가 LeavePacket 으로 알려준다.
    if (!is_click && !is_scroll) {
        if (g_remote.entry_guard > 0) { --g_remote.entry_guard; return NULL; }
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
    } else {
        // 클릭은 위치 이동이 없어 dx/dy 를 안 쓰므로, dx 를 클릭 카운트(clickState)
        // 전달에 재활용한다. 이 값이 있어야 client 에서 더블/트리플 클릭이 단어·문단
        // 선택으로 인식된다(설정 안 하면 매 클릭이 count=1 로 처리됨). dy 는 0 유지.
        int64_t cs = CGEventGetIntegerValueField(event, kCGMouseEventClickState);
        pkt.dx = static_cast<int16_t>(std::clamp(cs, 1LL, 32767LL));
    }
    g_engine->send_mouse(pkt);

    // host 자체 복귀 판정: delta 를 client 가상 위치에 누적(client 와 동일하게 clamp)하고
    // 반대편 경계 도달 시 왕복 지연 없이 즉시 복귀한다. client 의 LeavePacket 은 이중 안전망.
    if (!is_click && !is_scroll && host_predict_leave(pkt.dx, pkt.dy)) return NULL;
    return NULL;
}

// escape 단축키 감지
// Cmd+Esc          → 원격 모드 강제 복귀 (로컬 복귀)
// Cmd+Shift+Esc    → 세션 전체 강제 중지 (C++ 콜백 → Swift tether_stop)
// keycode 53 = kVK_Escape
static bool is_escape_shortcut(CGEventType type, CGEventRef event, bool* out_stop_session) {
    if (type != kCGEventKeyDown) return false;
    CGKeyCode kc = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    if (kc != 53) return false;
    CGEventFlags flags = CGEventGetFlags(event) & kCGEventFlagMaskNonCoalesced;
    (void)flags; // non-coalesced 마스크는 아래에서 별도 처리
    CGEventFlags f = CGEventGetFlags(event);
    bool cmd   = (f & kCGEventFlagMaskCommand) != 0;
    bool shift = (f & kCGEventFlagMaskShift)   != 0;
    bool ctrl  = (f & kCGEventFlagMaskControl) != 0;
    bool alt   = (f & kCGEventFlagMaskAlternate) != 0;
    if (!cmd) return false;
    // Cmd+Esc (다른 modifier 없음) → 원격 복귀
    if (!shift && !ctrl && !alt) { *out_stop_session = false; return true; }
    // Cmd+Shift+Esc (다른 modifier 없음) → 세션 중지
    if (shift && !ctrl && !alt)  { *out_stop_session = true;  return true; }
    return false;
}

// 세션 강제 중지 콜백 — Swift 에서 등록, CGEventTap 콜백에서 호출
static std::function<void()> g_force_stop_cb;

void set_force_stop_cb(std::function<void()> cb) {
    g_force_stop_cb = std::move(cb);
}

static CGEventRef keyboard_callback(CGEventTapProxy, CGEventType type, CGEventRef event, void*) {
    if (type == kCGEventTapDisabledByTimeout ||
        type == kCGEventTapDisabledByUserInput) {
        TETHER_ERROR(CAT_EVENT, "[호스트] 키보드 tap 비활성화 감지 (%u) — 재활성화", (unsigned)type);
        CGEventTapEnable(g_key_tap, true);
        return event;
    }
    bool stop_session = false;
    if (is_escape_shortcut(type, event, &stop_session)) {
        if (stop_session) {
            TETHER_INFO(CAT_CURSOR, "[호스트] Cmd+Shift+Esc — 세션 강제 중지");
            if (g_remote.active.load(std::memory_order_acquire)) leave_remote();
            if (g_force_stop_cb) g_force_stop_cb();
        } else {
            TETHER_INFO(CAT_CURSOR, "[호스트] Cmd+Esc — 강제 로컬 복귀");
            if (g_remote.active.load(std::memory_order_acquire)) leave_remote();
        }
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

// ── 디스플레이 재설정 감지 ────────────────────────────────────────────

static void display_reconfiguration_callback(CGDirectDisplayID display,
                                              CGDisplayChangeSummaryFlags flags,
                                              void* /*userInfo*/) {
    // kCGDisplayAddFlag / kCGDisplayRemoveFlag: 모니터 연결/해제
    // kCGDisplayMovedFlag / kCGDisplaySetMainFlag: 배치 변경
    if (flags & (kCGDisplayAddFlag | kCGDisplayRemoveFlag |
                 kCGDisplayMovedFlag | kCGDisplaySetMainFlag)) {
        TETHER_INFO(CAT_CURSOR, "[호스트] 디스플레이 재설정 감지 (display=%u, flags=0x%x) — 레이아웃 갱신",
                   display, flags);
        // 원격 모드 중이면 먼저 복귀 — 경계가 바뀌면 기존 가상 좌표가 무의미
        if (g_remote.active.load(std::memory_order_acquire)) {
            force_leave_remote("디스플레이 재설정");
        }
        refresh_layout(g_cfg.client_edge);
    }
}

// ── RunLoop 기반 이벤트 탭 설정/해제 ─────────────────────────────────

void setup_event_taps() {
    TETHER_DEBUG(CAT_EVENT, "[호스트] 이벤트 탭 설정 시작");

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
        TETHER_ERROR(CAT_EVENT, "[호스트] 이벤트 탭 생성 실패: 마우스 탭=%p, 키 탭=%p",
                    g_mouse_tap, g_key_tap);
        set_status(TETHER_STATUS_ERROR, "Event Tap 권한 필요 (손쉬운 사용)");
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

    // 디스플레이 핫플러그 감지 등록
    CGDisplayRegisterReconfigurationCallback(display_reconfiguration_callback, nullptr);

    g_taps_active.store(true, std::memory_order_release);
    TETHER_INFO(CAT_EVENT, "[호스트] 이벤트 탭 설정 완료");
}

void teardown_event_taps() {
    g_taps_active.store(false, std::memory_order_release);
    CGDisplayRemoveReconfigurationCallback(display_reconfiguration_callback, nullptr);
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
        // RunLoop 이 없거나 setup 실패 — 최대한 복구 시도
        TETHER_ERROR(CAT_CURSOR, "[호스트] 원격 모드 강제 종료 폴백 (%s): RunLoop 없음", reason ? reason : "");
        CGAssociateMouseAndMouseCursorPosition(true);
        tether_show_cursor();
        g_remote.return_time = std::chrono::steady_clock::now();
        g_remote.active.store(false, std::memory_order_release);

        // 커서를 화면 중앙으로 이동
        CGDirectDisplayID main_id = CGMainDisplayID();
        CGRect b = CGDisplayBounds(main_id);
        CGWarpMouseCursorPosition(CGPointMake(b.origin.x + b.size.width * 0.5,
                                              b.origin.y + b.size.height * 0.5));
        return;
    }

    TETHER_INFO(CAT_CURSOR, "[호스트] 원격 모드 강제 종료 요청: %s", reason ? reason : "");
    CFRunLoopPerformBlock(rl, kCFRunLoopCommonModes, ^{
        if (g_remote.active.load(std::memory_order_acquire)) leave_remote();
    });
    CFRunLoopWakeUp(rl);
}

// 클라이언트 LeavePacket 수신 시의 복귀 경로. host 자체 예측(host_predict_leave)이
// 주 경로이며 대개 먼저 복귀하므로, 이 경로는 host 예측이 놓친 경우의 안전망이다.
// (이미 active=false 면 아래 가드에서 무시됨)
void request_leave_remote(double ratio) {
    if (!g_remote.active.load(std::memory_order_acquire)) return;

    CFRunLoopRef rl = g_run_loop.load(std::memory_order_acquire);
    if (!rl || rl == (CFRunLoopRef)-1) { force_leave_remote("복귀 요청(RunLoop 없음)"); return; }

    // 복귀 좌표 계산과 leave_remote 를 RunLoop 스레드에서 수행해 g_layout/커서 접근을 일원화한다.
    CFRunLoopPerformBlock(rl, kCFRunLoopCommonModes, ^{
        if (!g_remote.active.load(std::memory_order_acquire)) return;
        set_return_from_ratio(ratio);
        leave_remote();
    });
    CFRunLoopWakeUp(rl);
}

// 연결 중 client 위치 변경. 레이아웃/커서 접근을 RunLoop 스레드로 일원화해
// mouse_callback 및 디스플레이 재설정 콜백과 직렬화한다.
// 전환 기준 화면 변경. edge 변경과 동일하게 RunLoop 스레드로 위임해
// 레이아웃/커서 접근을 직렬화한다.
void apply_trigger_display(const char* uuid) {
    CFRunLoopRef rl = g_run_loop.load(std::memory_order_acquire);
    if (!rl || rl == (CFRunLoopRef)-1) {
        // 시작 전이면 값만 보관 — tether_start 의 refresh_layout 이 반영한다.
        set_trigger_display_uuid(uuid);
        return;
    }

    // 블록이 실행될 때까지 문자열이 살아 있어야 하므로 복사해 캡처한다.
    std::string copy = uuid ? uuid : "";
    CFRunLoopPerformBlock(rl, kCFRunLoopCommonModes, ^{
        // 경계가 바뀌면 기존 원격 가상 좌표가 무의미하므로 먼저 복귀한다.
        if (g_remote.active.load(std::memory_order_acquire)) leave_remote();
        set_trigger_display_uuid(copy.c_str());
        refresh_layout(g_cfg.client_edge);
    });
    CFRunLoopWakeUp(rl);
}

void apply_client_edge(TetherEdge edge) {
    CFRunLoopRef rl = g_run_loop.load(std::memory_order_acquire);
    if (!rl || rl == (CFRunLoopRef)-1) {
        // RunLoop 이 아직 없으면(시작 전) 값만 갱신 — start 시 refresh_layout 이 반영한다.
        g_cfg.client_edge = edge;
        return;
    }

    CFRunLoopPerformBlock(rl, kCFRunLoopCommonModes, ^{
        // 경계가 바뀌면 기존 가상 좌표가 무의미하므로 원격 모드 중이면 먼저 복귀한다.
        // 이 블록은 RunLoop 스레드에서 실행되므로 leave_remote 를 직접 호출한다.
        // (force_leave_remote 는 leave_remote 를 다시 큐잉해 refresh_layout 뒤로 밀리므로
        //  복귀 좌표가 새 레이아웃으로 계산되는 것을 피한다.)
        if (g_remote.active.load(std::memory_order_acquire)) {
            leave_remote();
        }
        g_cfg.client_edge = edge;
        refresh_layout(edge);
    });
    CFRunLoopWakeUp(rl);
}
