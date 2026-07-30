#import <XCTest/XCTest.h>
#include "EdgeGeometry.h"

// ── 전환 경계 기하 테스트 ─────────────────────────────────────────────
//
// 멀티 모니터 배치를 rect 목록으로 직접 구성해 검증한다.
// 실제 모니터를 여러 대 연결하지 않고도 경계 계산·통과 판정을 확인할 수 있다.
//
// 좌표계: macOS 전역 좌표 (주 디스플레이 좌상단 원점, y 아래로 증가)

@interface EdgeGeometryTests : XCTestCase
@end

@implementation EdgeGeometryTests

// 실사용 구성: 맥북(내장, 1728x1117) 왼쪽 + 외장 4K(3840x2160) 오른쪽.
// 외장이 맥북 오른쪽에 세로 중앙 정렬로 붙어 있는 흔한 배치.
static std::vector<CGRect> macbook_plus_external() {
    return {
        CGRectMake(0, 0, 1728, 1117),          // 맥북 (주 디스플레이)
        CGRectMake(1728, -521, 3840, 2160),    // 외장 4K (세로 중앙 정렬)
    };
}

// ── 자동 모드 (기준 화면 미지정) ──────────────────────────────────

- (void)test_auto_right_usesOutermostEdge {
    auto d = macbook_plus_external();
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, -1);

    // 가장 바깥 오른쪽 = 외장의 오른쪽 끝
    XCTAssertEqual(g.trigger_edge, 1728 + 3840);
    XCTAssertFalse(g.interior_edge, @"최외곽 경계는 내부 경계가 아니다");
    XCTAssertEqual(g.edge_displays.size(), 1u, @"오른쪽 끝에 접한 화면은 외장 하나");
    XCTAssertEqual(g.edge_displays[0].origin.x, 1728);
}

- (void)test_auto_left_usesMacbookEdge {
    auto d = macbook_plus_external();
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_LEFT, -1);

    XCTAssertEqual(g.trigger_edge, 0);
    XCTAssertFalse(g.interior_edge);
    XCTAssertEqual(g.edge_displays.size(), 1u);
}

- (void)test_auto_desktopUnion {
    auto d = macbook_plus_external();
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, -1);

    XCTAssertEqual(g.left, 0);
    XCTAssertEqual(g.right, 5568);
    XCTAssertEqual(g.top, -521);      // 외장이 위로 더 뻗음
    XCTAssertEqual(g.bottom, 1639);   // 외장이 아래로 더 뻗음
}

// ── 기준 화면 지정: "맥북 화면 오른쪽" (내부 경계) ─────────────────

- (void)test_triggerMacbookRight_isInteriorEdge {
    auto d = macbook_plus_external();
    // index 0 = 맥북. 맥북의 오른쪽(x=1728)은 외장이 붙어 있어 데스크톱 내부.
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, 0);

    XCTAssertEqual(g.trigger_edge, 1728);
    XCTAssertTrue(g.interior_edge, @"맥북 오른쪽은 외장에 가려진 내부 경계여야 한다");
    XCTAssertEqual(g.edge_displays.size(), 1u, @"기준 화면만 대상");
    XCTAssertEqual(g.edge_displays[0].size.width, 1728);
}

- (void)test_triggerExternalRight_isNotInterior {
    auto d = macbook_plus_external();
    // index 1 = 외장. 외장의 오른쪽은 데스크톱 최외곽이라 내부 경계가 아니다.
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, 1);

    XCTAssertEqual(g.trigger_edge, 5568);
    XCTAssertFalse(g.interior_edge);
}

// ── 내부 경계 통과 판정 ───────────────────────────────────────────

- (void)test_interiorEdge_crossingDetected {
    auto d = macbook_plus_external();
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, 0);

    // 맥북 안(x=1720) → 경계 넘어 외장 쪽(x=1740). 통과로 감지되어야 한다.
    CGPoint prev = CGPointMake(1720, 500);
    CGPoint loc  = CGPointMake(1740, 500);
    XCTAssertTrue(is_edge_reached(g, TETHER_EDGE_RIGHT, loc, prev, true));
}

- (void)test_interiorEdge_noCrossing_notTriggered {
    auto d = macbook_plus_external();
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, 0);

    // 경계 근처지만 아직 넘지 않음 → 전환하지 않아야 한다.
    // (근접만으로 트리거하면 맥북에서 외장으로 넘어갈 때마다 오작동한다)
    CGPoint prev = CGPointMake(1700, 500);
    CGPoint loc  = CGPointMake(1726, 500);
    XCTAssertFalse(is_edge_reached(g, TETHER_EDGE_RIGHT, loc, prev, true));
}

- (void)test_interiorEdge_reverseCrossing_notTriggered {
    auto d = macbook_plus_external();
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, 0);

    // 외장 → 맥북 (반대 방향 통과)은 전환 대상이 아니다.
    CGPoint prev = CGPointMake(1740, 500);
    CGPoint loc  = CGPointMake(1720, 500);
    XCTAssertFalse(is_edge_reached(g, TETHER_EDGE_RIGHT, loc, prev, true));
}

- (void)test_interiorEdge_outsideSpan_notTriggered {
    auto d = macbook_plus_external();
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, 0);

    // 경계선을 넘었지만 y 가 맥북 화면 세로 범위(0~1117) 밖 — 외장 위쪽 영역.
    // 맥북 경계가 아니므로 전환하지 않아야 한다.
    CGPoint prev = CGPointMake(1720, -300);
    CGPoint loc  = CGPointMake(1740, -300);
    XCTAssertFalse(is_edge_reached(g, TETHER_EDGE_RIGHT, loc, prev, true));
}

- (void)test_interiorEdge_withoutPrev_notTriggered {
    auto d = macbook_plus_external();
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, 0);

    // 직전 위치를 모르면(세션 시작 직후) 통과 판정을 할 수 없어 트리거하지 않는다.
    // 다음 이동 이벤트부터 정상 동작하므로 안전한 선택이다.
    CGPoint loc = CGPointMake(1728, 500);
    XCTAssertFalse(is_edge_reached(g, TETHER_EDGE_RIGHT, loc, CGPointZero, false));
}

// ── 최외곽 경계 근접 판정 (기존 동작 유지) ─────────────────────────

- (void)test_outerEdge_proximityTriggers {
    auto d = macbook_plus_external();
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, -1);

    // 최외곽에서는 커서가 경계에 멈추므로 3px 근접으로 트리거한다.
    CGPoint loc = CGPointMake(5566, 500);
    XCTAssertTrue(is_edge_reached(g, TETHER_EDGE_RIGHT, loc, CGPointMake(5500, 500), true));
}

- (void)test_outerEdge_farFromEdge_notTriggered {
    auto d = macbook_plus_external();
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, -1);

    CGPoint loc = CGPointMake(5000, 500);
    XCTAssertFalse(is_edge_reached(g, TETHER_EDGE_RIGHT, loc, CGPointMake(4900, 500), true));
}

- (void)test_outerEdge_outsideSpan_notTriggered {
    // 계단식 배치: 두 화면이 세로로 어긋나 있어 오른쪽 끝 span 이 제한된다.
    std::vector<CGRect> d = {
        CGRectMake(0, 0, 1728, 1117),
        CGRectMake(1728, 0, 1920, 600),   // 오른쪽 끝, 세로 0~600 만 차지
    };
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, -1);
    XCTAssertEqual(g.trigger_edge, 3648);

    // y=800 은 오른쪽 끝 화면의 세로 범위 밖 → 트리거되지 않아야 한다.
    CGPoint loc = CGPointMake(3647, 800);
    XCTAssertFalse(is_edge_reached(g, TETHER_EDGE_RIGHT, loc, CGPointMake(3600, 800), true));
}

// ── 세로 배치 (위/아래) ───────────────────────────────────────────

- (void)test_verticalStack_triggerBottomOfTopDisplay {
    // 외장이 맥북 위에 있는 배치. "맥북 위쪽"을 기준으로 하면 내부 경계.
    std::vector<CGRect> d = {
        CGRectMake(0, 0, 1728, 1117),        // 맥북
        CGRectMake(0, -1440, 2560, 1440),    // 위쪽 외장
    };
    auto top = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_TOP, 0);
    XCTAssertEqual(top.trigger_edge, 0);
    XCTAssertTrue(top.interior_edge, @"맥북 위쪽은 외장에 가려진 내부 경계");

    // 아래로 통과: y 가 감소하며 0 을 넘는다
    XCTAssertTrue(is_edge_reached(top, TETHER_EDGE_TOP,
                                 CGPointMake(500, -5), CGPointMake(500, 10), true));
    // 반대 방향은 트리거 안 됨
    XCTAssertFalse(is_edge_reached(top, TETHER_EDGE_TOP,
                                   CGPointMake(500, 10), CGPointMake(500, -5), true));
}

- (void)test_singleDisplay_neverInterior {
    // 모니터 1대: 어느 방향이든 최외곽이라 내부 경계가 될 수 없다.
    std::vector<CGRect> d = { CGRectMake(0, 0, 1728, 1117) };
    for (int e = 0; e <= 3; ++e) {
        auto g = compute_edge_geometry(d.data(), d.size(), (TetherEdge)e, 0);
        XCTAssertFalse(g.interior_edge, @"단일 모니터는 내부 경계가 없다 (edge=%d)", e);
    }
}

- (void)test_invalidTriggerIndex_fallsBackToAuto {
    auto d = macbook_plus_external();
    // 기준 화면이 분리된 상황(인덱스 범위 밖) → 자동 모드로 폴백해야 한다.
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, 99);
    XCTAssertEqual(g.trigger_edge, 5568, @"자동(최외곽)으로 폴백");
    XCTAssertFalse(g.interior_edge);
}

// ── 회귀: 이웃 없는 변을 내부 경계로 오판하던 버그 ────────────────
//
// 증상(v0.9.61): 맥북 오른쪽으로 이동해도 전환이 안 되고, 위쪽 모서리 근처에서만 됐다.
// 원인: interior_edge 를 "union 최외곽이 아님"으로 판정해서, 실제로는 이웃 화면이
//       없어 커서가 멈추는 변까지 내부 경계로 보고 통과 판정만 기다렸다.
//       통과는 영구히 발생하지 않으므로 전환 불가.

// 외장이 맥북 '위쪽'에 있고 폭이 더 넓은 배치.
// 맥북 오른쪽(x=1728)에는 이웃 화면이 없지만, union.right(2560)보다는 작다.
static std::vector<CGRect> macbook_with_wider_display_above() {
    return {
        CGRectMake(0, 0, 1728, 1117),        // 맥북
        CGRectMake(0, -1440, 2560, 1440),    // 위쪽 외장 (더 넓음)
    };
}

// 버그를 실제로 재현한 배치 (맥북 + 위쪽에 걸친 외장, 좌우로 어긋남).
// 맥북 오른쪽(x=1800)에 이웃이 없는데 union.right(2182)는 더 커서 오판이 발생했다.
- (void)test_regression_actualLayout_rightEdgeWorks {
    std::vector<CGRect> d = {
        CGRectMake(0, 0, 1800, 1169),        // 맥북 (주 디스플레이)
        CGRectMake(-378, -1440, 2560, 1440), // 외장: 위쪽, 왼쪽으로 튀어나옴
    };
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, 0);

    XCTAssertEqual(g.trigger_edge, 1800);
    XCTAssertFalse(g.interior_edge,
                   @"맥북 오른쪽에 맞닿은 화면이 없다 (외장은 위쪽) → 근접 판정을 써야 한다");
    // 맥북 오른쪽으로 커서를 밀면 전환돼야 한다 (버그 시에는 영구히 안 됐다)
    XCTAssertTrue(is_edge_reached(g, TETHER_EDGE_RIGHT,
                                 CGPointMake(1799, 600), CGPointMake(1750, 600), true));
    // 직전 위치를 몰라도 동작해야 한다
    XCTAssertTrue(is_edge_reached(g, TETHER_EDGE_RIGHT,
                                 CGPointMake(1799, 600), CGPointZero, false));

    // 위쪽은 외장이 실제로 맞닿아 있으므로 통과 판정 (버그 시에도 이쪽만 동작했다)
    auto top = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_TOP, 0);
    XCTAssertTrue(top.interior_edge);
    XCTAssertTrue(is_edge_reached(top, TETHER_EDGE_TOP,
                                 CGPointMake(500, -5), CGPointMake(500, 10), true));
}

- (void)test_regression_noNeighborOnRight_usesProximity {
    auto d = macbook_with_wider_display_above();
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, 0);

    XCTAssertEqual(g.trigger_edge, 1728);
    XCTAssertFalse(g.interior_edge,
                   @"맥북 오른쪽에는 이웃 화면이 없으므로 내부 경계가 아니다 "
                   @"(union.right 가 더 크다는 이유로 내부 경계로 보면 전환 불가)");

    // 커서가 맥북 오른쪽 변에 붙으면 근접 판정으로 전환돼야 한다.
    XCTAssertTrue(is_edge_reached(g, TETHER_EDGE_RIGHT,
                                 CGPointMake(1727, 500), CGPointMake(1700, 500), true));
}

- (void)test_regression_neighborAbove_topIsInterior {
    auto d = macbook_with_wider_display_above();
    // 맥북 위쪽에는 외장이 실제로 맞닿아 있다 → 내부 경계 (통과 판정)
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_TOP, 0);

    XCTAssertEqual(g.trigger_edge, 0);
    XCTAssertTrue(g.interior_edge, @"맥북 위쪽은 외장이 맞닿아 있어 내부 경계");
    XCTAssertTrue(is_edge_reached(g, TETHER_EDGE_TOP,
                                 CGPointMake(500, -5), CGPointMake(500, 10), true));
}

// 이웃이 변의 '일부'만 덮는 배치: 맥북 오른쪽 위쪽 절반에만 작은 외장이 붙어 있다.
// 덮인 구간은 커서가 통과하고, 덮이지 않은 구간은 커서가 멈춘다 → 판정이 달라야 한다.
- (void)test_partialNeighbor_mixedDetection {
    std::vector<CGRect> d = {
        CGRectMake(0, 0, 1728, 1117),      // 맥북
        CGRectMake(1728, 0, 1280, 500),    // 오른쪽 위 절반에만 붙은 외장
    };
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, 0);

    XCTAssertEqual(g.trigger_edge, 1728);
    XCTAssertTrue(g.interior_edge, @"일부라도 맞닿았으면 내부 경계 구간이 존재");
    XCTAssertEqual(g.neighbor_lo, 0);
    XCTAssertEqual(g.neighbor_hi, 500);

    // 덮인 구간(y=250): 통과해야 트리거
    XCTAssertTrue(is_edge_reached(g, TETHER_EDGE_RIGHT,
                                 CGPointMake(1740, 250), CGPointMake(1720, 250), true));
    XCTAssertFalse(is_edge_reached(g, TETHER_EDGE_RIGHT,
                                  CGPointMake(1726, 250), CGPointMake(1700, 250), true),
                   @"덮인 구간은 근접만으로 트리거하면 안 된다");

    // 덮이지 않은 구간(y=800): 커서가 멈추므로 근접으로 트리거
    XCTAssertTrue(is_edge_reached(g, TETHER_EDGE_RIGHT,
                                 CGPointMake(1727, 800), CGPointMake(1700, 800), true),
                  @"이웃이 없는 구간은 근접 판정으로 전환돼야 한다");
}

- (void)test_partialNeighbor_uncoveredSpan_worksWithoutPrev {
    // 이웃 없는 구간은 직전 위치를 몰라도 근접 판정으로 동작해야 한다.
    std::vector<CGRect> d = {
        CGRectMake(0, 0, 1728, 1117),
        CGRectMake(1728, 0, 1280, 500),
    };
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, 0);
    XCTAssertTrue(is_edge_reached(g, TETHER_EDGE_RIGHT,
                                 CGPointMake(1727, 800), CGPointZero, false));
}

- (void)test_crossingFlag_reportedCorrectly {
    auto d = macbook_plus_external();
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, 0);

    // 통과로 트리거된 경우 진입점을 prev 로 써야 하므로 플래그가 켜져야 한다.
    bool by_crossing = false;
    XCTAssertTrue(is_edge_reached(g, TETHER_EDGE_RIGHT,
                                 CGPointMake(1740, 500), CGPointMake(1720, 500), true,
                                 3.0, &by_crossing));
    XCTAssertTrue(by_crossing);

    // 근접으로 트리거된 경우(최외곽)는 loc 을 그대로 쓰므로 플래그가 꺼져 있어야 한다.
    auto outer = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, -1);
    by_crossing = true;
    XCTAssertTrue(is_edge_reached(outer, TETHER_EDGE_RIGHT,
                                 CGPointMake(5566, 500), CGPointMake(5500, 500), true,
                                 3.0, &by_crossing));
    XCTAssertFalse(by_crossing);
}

- (void)test_diagonalNeighbor_notAdjacent {
    // 대각선으로만 접한 배치(모서리만 닿음)는 커서가 넘어가지 않으므로 이웃이 아니다.
    std::vector<CGRect> d = {
        CGRectMake(0, 0, 1728, 1117),
        CGRectMake(1728, 1117, 1280, 800),   // 오른쪽 아래로 대각
    };
    auto g = compute_edge_geometry(d.data(), d.size(), TETHER_EDGE_RIGHT, 0);
    XCTAssertFalse(g.interior_edge, @"모서리만 닿은 경우는 인접이 아니다");
    XCTAssertTrue(is_edge_reached(g, TETHER_EDGE_RIGHT,
                                 CGPointMake(1727, 500), CGPointMake(1700, 500), true));
}

- (void)test_emptyDisplayList_safe {
    // 디스플레이 목록을 못 읽은 이상 상황에서도 크래시 없이 안전한 값.
    auto g = compute_edge_geometry(nullptr, 0, TETHER_EDGE_RIGHT, -1);
    XCTAssertTrue(g.edge_displays.empty());
    XCTAssertFalse(is_edge_reached(g, TETHER_EDGE_RIGHT,
                                   CGPointMake(100, 100), CGPointMake(0, 0), true));
}

@end
