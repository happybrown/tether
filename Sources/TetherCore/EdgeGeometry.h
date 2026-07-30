#pragma once
/// EdgeGeometry.h — 전환 경계 기하 계산 (순수 함수, 하드웨어 비의존)
///
/// EventTap.cpp 의 레이아웃/판정 로직 중 CoreGraphics 호출이 필요 없는 부분을 분리했다.
/// 디스플레이 rect 목록만 주면 결과가 정해지므로, 실제 모니터를 여러 대 연결하지 않고도
/// 멀티 모니터 배치(맥북+외장 등)의 경계 계산을 단위 테스트로 검증할 수 있다.
#include "include/tether_core.h"
#include <CoreGraphics/CGGeometry.h>
#include <vector>
#include <cstddef>

/// 계산된 전환 경계 정보.
struct EdgeGeometry {
    double trigger_edge = 0.0;   // 전환이 일어나는 경계 좌표 (edge 축)
    // 이 변에 이웃 화면이 맞닿아 있어 커서가 멈추지 않고 통과하는지.
    // true 면 통과 판정(prev→loc 이 경계선을 넘었는지)을 쓴다.
    bool   interior_edge = false;
    // 이웃이 맞닿은 구간 (경계 평행축 범위). 이웃이 변의 일부만 덮는 경우
    // 그 구간은 통과 판정, 나머지 구간은 근접 판정을 써야 한다.
    double neighbor_lo = 0.0, neighbor_hi = 0.0;
    // 경계 방향에 접한 디스플레이들. 경계 평행 축의 유효 구간을 정하는 데 쓴다.
    std::vector<CGRect> edge_displays;
    // 전체 데스크톱 union
    double left = 0.0, right = 0.0, top = 0.0, bottom = 0.0;
};

/// 디스플레이 rect 목록에서 데스크톱 union 을 구한다.
inline void compute_desktop_union(const CGRect* displays, size_t count,
                                  double& left, double& right,
                                  double& top, double& bottom) {
    left = top = 1e9;
    right = bottom = -1e9;
    for (size_t i = 0; i < count; ++i) {
        const CGRect& b = displays[i];
        if (b.origin.x < left)   left = b.origin.x;
        if (b.origin.y < top)    top = b.origin.y;
        if (b.origin.x + b.size.width  > right)  right = b.origin.x + b.size.width;
        if (b.origin.y + b.size.height > bottom) bottom = b.origin.y + b.size.height;
    }
}

/// 지정한 디스플레이의 edge 방향 경계 좌표.
inline double display_edge_coord(const CGRect& b, TetherEdge edge) {
    switch (edge) {
    case TETHER_EDGE_RIGHT:  return b.origin.x + b.size.width;
    case TETHER_EDGE_LEFT:   return b.origin.x;
    case TETHER_EDGE_TOP:    return b.origin.y;
    case TETHER_EDGE_BOTTOM: return b.origin.y + b.size.height;
    }
    return b.origin.x + b.size.width;
}

/// 두 구간 [a0,a1] 과 [b0,b1] 이 실제로 겹치는 길이 (접하기만 하면 0).
inline double span_overlap(double a0, double a1, double b0, double b1) {
    double lo = a0 > b0 ? a0 : b0;
    double hi = a1 < b1 ? a1 : b1;
    return hi > lo ? hi - lo : 0.0;
}

/// 디스플레이 b 의 edge 쪽 변에 다른 디스플레이가 맞닿아 있는지 검사한다.
///
/// 이것이 "커서가 이 변에서 멈추는가"를 결정한다. macOS 는 인접한 화면이 있는
/// 구간에서만 커서를 넘겨보내고, 이웃이 없는 구간에서는 커서를 변에 붙여 멈춘다.
/// union 범위로 판단하면(예: 맥북 위에만 큰 외장이 있어 union 이 옆으로 넓은 경우)
/// 이웃이 없는 변을 내부 경계로 오판해 전환이 영구히 안 되므로, 실제 인접만 본다.
///
/// out_overlap: 맞닿은 구간의 경계 평행축 범위 (겹치는 이웃들의 합집합 근사).
inline bool has_adjacent_display(const CGRect* displays, size_t count,
                                 size_t self_index, TetherEdge edge,
                                 double* out_lo = nullptr, double* out_hi = nullptr) {
    const double eps = 1.0;
    const CGRect& b = displays[self_index];
    const double e = display_edge_coord(b, edge);
    bool found = false;
    double lo = 0, hi = 0;

    for (size_t i = 0; i < count; ++i) {
        if (i == self_index) continue;
        const CGRect& o = displays[i];
        bool touching = false;
        double o_lo = 0, o_hi = 0;

        switch (edge) {
        case TETHER_EDGE_RIGHT:
            // 이웃의 왼쪽 변이 내 오른쪽 변에 붙어 있고, 세로 구간이 겹쳐야 한다.
            touching = (o.origin.x > e - eps && o.origin.x < e + eps) &&
                       span_overlap(b.origin.y, b.origin.y + b.size.height,
                                    o.origin.y, o.origin.y + o.size.height) > eps;
            o_lo = o.origin.y; o_hi = o.origin.y + o.size.height;
            break;
        case TETHER_EDGE_LEFT:
            touching = (o.origin.x + o.size.width > e - eps &&
                        o.origin.x + o.size.width < e + eps) &&
                       span_overlap(b.origin.y, b.origin.y + b.size.height,
                                    o.origin.y, o.origin.y + o.size.height) > eps;
            o_lo = o.origin.y; o_hi = o.origin.y + o.size.height;
            break;
        case TETHER_EDGE_TOP:
            touching = (o.origin.y + o.size.height > e - eps &&
                        o.origin.y + o.size.height < e + eps) &&
                       span_overlap(b.origin.x, b.origin.x + b.size.width,
                                    o.origin.x, o.origin.x + o.size.width) > eps;
            o_lo = o.origin.x; o_hi = o.origin.x + o.size.width;
            break;
        case TETHER_EDGE_BOTTOM:
            touching = (o.origin.y > e - eps && o.origin.y < e + eps) &&
                       span_overlap(b.origin.x, b.origin.x + b.size.width,
                                    o.origin.x, o.origin.x + o.size.width) > eps;
            o_lo = o.origin.x; o_hi = o.origin.x + o.size.width;
            break;
        }
        if (!touching) continue;
        if (!found) { lo = o_lo; hi = o_hi; found = true; }
        else { if (o_lo < lo) lo = o_lo; if (o_hi > hi) hi = o_hi; }
    }

    if (found) {
        if (out_lo) *out_lo = lo;
        if (out_hi) *out_hi = hi;
    }
    return found;
}

/// 전환 경계를 계산한다.
///   trigger_index >= 0 : 그 디스플레이의 edge 방향 경계를 쓴다 (내부 경계일 수 있음).
///   trigger_index <  0 : 자동 — 데스크톱 최외곽 경계, 그 선에 접한 모든 디스플레이가 대상.
inline EdgeGeometry compute_edge_geometry(const CGRect* displays, size_t count,
                                          TetherEdge edge, int trigger_index) {
    EdgeGeometry g;
    if (count == 0) return g;
    compute_desktop_union(displays, count, g.left, g.right, g.top, g.bottom);

    const double eps = 1.0;
    if (trigger_index >= 0 && (size_t)trigger_index < count) {
        const CGRect& b = displays[trigger_index];
        g.trigger_edge = display_edge_coord(b, edge);
        g.edge_displays.push_back(b);
        // 이 변에 실제로 맞닿은 이웃 화면이 있을 때만 내부 경계다.
        // (이웃이 없으면 커서가 이 변에서 멈추므로 근접 판정을 써야 한다)
        double n_lo = 0, n_hi = 0;
        if (has_adjacent_display(displays, count, (size_t)trigger_index, edge, &n_lo, &n_hi)) {
            g.interior_edge = true;
            g.neighbor_lo = n_lo;
            g.neighbor_hi = n_hi;
        }
        return g;
    }

    // 자동 모드: 최외곽 경계 + 그 선에 접한 디스플레이 전부
    switch (edge) {
    case TETHER_EDGE_RIGHT:  g.trigger_edge = g.right;  break;
    case TETHER_EDGE_LEFT:   g.trigger_edge = g.left;   break;
    case TETHER_EDGE_TOP:    g.trigger_edge = g.top;    break;
    case TETHER_EDGE_BOTTOM: g.trigger_edge = g.bottom; break;
    }
    for (size_t i = 0; i < count; ++i) {
        const CGRect& b = displays[i];
        bool on_edge = false;
        switch (edge) {
        case TETHER_EDGE_RIGHT:  on_edge = (b.origin.x + b.size.width  >= g.right  - eps); break;
        case TETHER_EDGE_LEFT:   on_edge = (b.origin.x                 <= g.left   + eps); break;
        case TETHER_EDGE_TOP:    on_edge = (b.origin.y                 <= g.top    + eps); break;
        case TETHER_EDGE_BOTTOM: on_edge = (b.origin.y + b.size.height >= g.bottom - eps); break;
        }
        if (on_edge) g.edge_displays.push_back(b);
    }
    return g;
}

/// 경계와 평행한 축에서 loc 이 경계 디스플레이 구간 안인지.
inline bool within_edge_span(const EdgeGeometry& g, CGPoint loc, TetherEdge edge) {
    for (const CGRect& b : g.edge_displays) {
        bool in_range = false;
        switch (edge) {
        case TETHER_EDGE_RIGHT:
        case TETHER_EDGE_LEFT:
            in_range = (loc.y >= b.origin.y && loc.y <= b.origin.y + b.size.height);
            break;
        case TETHER_EDGE_TOP:
        case TETHER_EDGE_BOTTOM:
            in_range = (loc.x >= b.origin.x && loc.x <= b.origin.x + b.size.width);
            break;
        }
        if (in_range) return true;
    }
    return false;
}

/// 전환 경계 도달 판정.
///   최외곽 경계: 커서가 그 선에서 멈추므로 threshold 근접으로 판정.
///   내부 경계  : 커서가 통과하므로 prev→loc 이 경계선을 넘었는지로 판정하고,
///                진입 비율은 경계선상의 prev 위치로 계산해야 정확하다.
/// out_by_crossing: 통과 판정으로 트리거됐는지. 이 경우 진입점은 경계선상의
/// prev 위치를 써야 한다 (loc 은 이미 경계를 지나쳐 이웃 화면 안이다).
inline bool is_edge_reached(const EdgeGeometry& g, TetherEdge edge,
                            CGPoint loc, CGPoint prev, bool prev_valid,
                            double threshold = 3.0,
                            bool* out_by_crossing = nullptr) {
    if (out_by_crossing) *out_by_crossing = false;
    const double e = g.trigger_edge;

    // 경계 평행축에서 현재 위치가 "이웃 화면이 맞닿은 구간" 안인지.
    // 이 구간에서는 커서가 통과하므로 통과 판정, 밖에서는 멈추므로 근접 판정을 쓴다.
    // (예: 맥북 오른쪽 위쪽 절반에만 외장이 붙은 배치 → 아래쪽 절반은 커서가 멈춘다)
    bool in_neighbor_span = false;
    if (g.interior_edge) {
        double v = (edge == TETHER_EDGE_RIGHT || edge == TETHER_EDGE_LEFT)
                 ? (prev_valid ? prev.y : loc.y)
                 : (prev_valid ? prev.x : loc.x);
        in_neighbor_span = (v >= g.neighbor_lo && v <= g.neighbor_hi);
    }

    if (in_neighbor_span) {
        // 통과 판정. 직전 위치를 모르면(세션 시작 직후) 보류한다 — 근접으로 폴백하면
        // 커서가 그 선을 정상적으로 지나갈 때마다 오작동한다.
        if (!prev_valid) return false;
        bool crossed = false;
        switch (edge) {
        case TETHER_EDGE_RIGHT:  crossed = (prev.x <  e && loc.x >= e); break;
        case TETHER_EDGE_LEFT:   crossed = (prev.x >  e && loc.x <= e); break;
        case TETHER_EDGE_TOP:    crossed = (prev.y >  e && loc.y <= e); break;
        case TETHER_EDGE_BOTTOM: crossed = (prev.y <  e && loc.y >= e); break;
        }
        if (!crossed) return false;
        if (!within_edge_span(g, prev, edge)) return false;
        if (out_by_crossing) *out_by_crossing = true;
        return true;
    }

    // 근접 판정 (최외곽 경계 또는 이웃이 없는 구간 — 커서가 이 선에서 멈춘다)
    bool hit = false;
    switch (edge) {
    case TETHER_EDGE_RIGHT:  hit = loc.x >= e - threshold; break;
    case TETHER_EDGE_LEFT:   hit = loc.x <= e + threshold; break;
    case TETHER_EDGE_TOP:    hit = loc.y <= e + threshold; break;
    case TETHER_EDGE_BOTTOM: hit = loc.y >= e - threshold; break;
    }
    if (!hit) return false;
    return within_edge_span(g, loc, edge);
}
