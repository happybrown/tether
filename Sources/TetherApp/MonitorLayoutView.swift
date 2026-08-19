import SwiftUI
import AppKit

/// Host 의 모든 디스플레이를 실제 배치·해상도 비례로 그리고, 상대 Mac 사각형을
/// 원하는 화면의 원하는 변에 끌어다 붙여 전환 지점을 지정하는 뷰.
///
/// 드래그 한 번으로 세 가지가 함께 정해진다:
/// - 기준 화면(triggerUUID): 상대 Mac 을 가장 가까이 붙인 host 디스플레이
/// - 방향(edge): 그 화면의 어느 변에 붙였는지 → 커서 전환 경계
/// - 정렬 오프셋(offset): 경계와 평행한 축의 어긋남 보정 (-1.0 ~ +1.0)
///
/// 예: 맥북+외장 구성에서 상대 Mac 을 맥북 화면 오른쪽 변에 붙이면,
///     외장이 오른쪽에 있어도 "맥북 화면 오른쪽"에서 상대 Mac 으로 전환된다.
struct MonitorLayoutView: View {
    @Binding var edge: ClientEdge
    @Binding var offset: Double
    /// 기준 화면 UUID. 빈 문자열이면 자동(데스크톱 최외곽 경계).
    @Binding var triggerUUID: String
    /// host 디스플레이 목록 (전역 좌표). 비어 있으면 주 화면 하나로 대체한다.
    var displays: [DisplayInfo]
    /// client 화면 해상도. 미연결 등으로 모르면 nil → host 주 화면 비율로 그린다.
    var clientResolution: CGSize?

    private let canvasHeight: CGFloat = 210
    private let margin: CGFloat = 12

    var body: some View {
        GeometryReader { geo in
            let m = metrics(in: geo.size)
            ZStack(alignment: .topLeading) {
                ForEach(m.hosts) { h in
                    MonitorBox(title: h.display.label,
                               caption: h.display.resolutionText,
                               tint: .secondary,
                               selected: false,
                               highlighted: h.display.uuid == effectiveTriggerUUID)
                        .frame(width: h.rect.width, height: h.rect.height)
                        .position(x: h.rect.midX, y: h.rect.midY)
                }

                MonitorBox(title: L("layout.other_mac"),
                           caption: clientCaption,
                           tint: .accentColor,
                           selected: true,
                           highlighted: false)
                    .frame(width: m.client.width, height: m.client.height)
                    .position(x: m.client.midX, y: m.client.midY)
                    .gesture(dragGesture(m))
            }
            .coordinateSpace(name: Self.canvasSpace)
            .animation(.spring(response: 0.3, dampingFraction: 0.8), value: edge)
            .animation(.spring(response: 0.3, dampingFraction: 0.8), value: triggerUUID)
        }
        .frame(height: canvasHeight)
        .help(L("layout.drag_help"))
    }

    /// 실제로 적용되는 기준 화면. 자동(빈 값)일 때는 경계 방향의 최외곽 화면이 된다 —
    /// C 코어의 자동 모드와 같은 기준으로 계산해 UI 강조와 동작을 일치시킨다.
    private var effectiveTriggerUUID: String {
        if !triggerUUID.isEmpty { return triggerUUID }
        return outermostDisplay(for: edge)?.uuid ?? ""
    }

    private func outermostDisplay(for e: ClientEdge) -> DisplayInfo? {
        switch e {
        case .right:  return effectiveDisplays.max { $0.frame.maxX < $1.frame.maxX }
        case .left:   return effectiveDisplays.min { $0.frame.minX < $1.frame.minX }
        case .top:    return effectiveDisplays.min { $0.frame.minY < $1.frame.minY }
        case .bottom: return effectiveDisplays.max { $0.frame.maxY < $1.frame.maxY }
        }
    }

    /// 디스플레이 목록이 비었을 때(권한/타이밍 문제)도 UI 가 깨지지 않게 대체본을 쓴다.
    private var effectiveDisplays: [DisplayInfo] {
        displays.isEmpty
            ? [DisplayInfo(uuid: "", frame: CGRect(origin: .zero,
                                                   size: NSScreen.main?.frame.size
                                                       ?? CGSize(width: 1600, height: 1000)),
                           isMain: true, isBuiltin: false)]
            : displays
    }

    private var clientCaption: String {
        clientResolution.map { "\(Int($0.width)) × \(Int($0.height))" }
            ?? L("layout.resolution_unknown")
    }

    // ── 배치 계산 ──────────────────────────────────────────────────

    private struct HostRect: Identifiable {
        let display: DisplayInfo
        let rect: CGRect
        var id: String { display.uuid }
    }

    private struct Metrics {
        let hosts: [HostRect]
        let client: CGRect
        let clientBox: CGSize
        let scale: CGFloat
        let origin: CGPoint      // 전역좌표 → 캔버스 변환 오프셋
    }

    /// 전역 디스플레이 좌표를 캔버스에 비례 축소해 배치한다.
    /// client 사각형이 어느 변에 붙어도 캔버스를 넘지 않도록 사방에 client 크기만큼 여유를 준다.
    private func metrics(in size: CGSize) -> Metrics {
        let ds = effectiveDisplays
        let union = ds.dropFirst().reduce(ds[0].frame) { $0.union($1.frame) }
        let cliPx = clientResolution ?? ds.first(where: \.isMain)?.frame.size ?? union.size

        let extentW = union.width + 2 * cliPx.width
        let extentH = union.height + 2 * cliPx.height
        let scale = min((size.width - margin * 2) / extentW,
                        (size.height - margin * 2) / extentH)

        // union 전체(+ 사방 여유)를 캔버스 중앙에 놓는다.
        let drawnW = extentW * scale
        let drawnH = extentH * scale
        let originX = (size.width - drawnW) / 2 + cliPx.width * scale - union.minX * scale
        let originY = (size.height - drawnH) / 2 + cliPx.height * scale - union.minY * scale
        let origin = CGPoint(x: originX, y: originY)

        let hosts = ds.map { d in
            HostRect(display: d, rect: CGRect(x: origin.x + d.frame.minX * scale,
                                              y: origin.y + d.frame.minY * scale,
                                              width: d.frame.width * scale,
                                              height: d.frame.height * scale))
        }

        let cliBox = CGSize(width: cliPx.width * scale, height: cliPx.height * scale)
        let anchor = hosts.first { $0.display.uuid == effectiveTriggerUUID }?.rect
            ?? hosts[0].rect

        // 기준 화면의 지정된 변에 붙이고, 경계 평행 축으로 offset 만큼 이동시킨다.
        var center = CGPoint(x: anchor.midX, y: anchor.midY)
        let visual = max(-0.5, min(0.5, offset))
        switch edge {
        case .right:
            center.x = anchor.maxX + cliBox.width / 2
            center.y += visual * cliBox.height
        case .left:
            center.x = anchor.minX - cliBox.width / 2
            center.y += visual * cliBox.height
        case .top:
            center.y = anchor.minY - cliBox.height / 2
            center.x += visual * cliBox.width
        case .bottom:
            center.y = anchor.maxY + cliBox.height / 2
            center.x += visual * cliBox.width
        }
        let client = CGRect(x: center.x - cliBox.width / 2, y: center.y - cliBox.height / 2,
                            width: cliBox.width, height: cliBox.height)
        return Metrics(hosts: hosts, client: client, clientBox: cliBox,
                       scale: scale, origin: origin)
    }

    // ── 드래그 → 기준 화면 + 방향 + 오프셋 ─────────────────────────

    /// 캔버스 고정 좌표계. 드래그 중 client 박스가 움직여도 포인터 좌표가 흔들리지 않게 한다.
    private static let canvasSpace = "monitorLayoutCanvas"

    private func dragGesture(_ m: Metrics) -> some Gesture {
        // 클로저 본문은 applyDrag 로 분리한다. 본문을 인라인으로 두면 타입 추론이
        // 무거워져 일부 Swift 버전에서 "type of expression is ambiguous" 로 실패한다.
        DragGesture(minimumDistance: 2, coordinateSpace: .named(Self.canvasSpace))
            .onChanged { (v: DragGesture.Value) in
                applyDrag(at: v.location, m: m)
            }
    }

    /// 드래그 위치를 기준 화면·방향·오프셋으로 반영한다.
    private func applyDrag(at p: CGPoint, m: Metrics) {
        // 1) 기준 화면 + 방향: 포인터에서 각 화면의 네 변까지 거리를 재고
        //    가장 가까운 (화면, 변) 조합을 고른다. 화면 크기가 달라도 공정하게
        //    비교되도록 픽셀 거리를 그대로 쓴다.
        var bestUUID: String = ""
        var bestEdge: ClientEdge = .right
        var bestDist: CGFloat = .greatestFiniteMagnitude
        var found = false
        for h in m.hosts {
            let rect: CGRect = h.rect
            for e in ClientEdge.allCases {
                let d: CGFloat = distance(from: p, toEdge: e, of: rect)
                if d < bestDist {
                    bestDist = d
                    bestUUID = h.display.uuid
                    bestEdge = e
                    found = true
                }
            }
        }
        guard found,
              let anchor = m.hosts.first(where: { $0.display.uuid == bestUUID })?.rect
        else { return }

        if triggerUUID != bestUUID { triggerUUID = bestUUID }
        if edge != bestEdge { edge = bestEdge }

        // 2) 오프셋: 경계 평행 축에서 기준 화면 중앙과의 거리를 client 크기 비율로.
        let raw: Double
        switch bestEdge {
        case .left, .right:
            raw = Double(p.y - anchor.midY) / Double(m.clientBox.height)
        case .top, .bottom:
            raw = Double(p.x - anchor.midX) / Double(m.clientBox.width)
        }
        // 각 단계의 타입을 명시한다. 리터럴과 abs/max/min 오버로드가 한 식에 겹치면
        // 일부 Swift 버전이 타입을 못 풀고 "ambiguous" 로 실패한다.
        let lower: Double = -1.0
        let upper: Double = 1.0
        let clamped: Double = min(upper, max(lower, raw))
        var snapped: Double = (clamped * 100.0).rounded() / 100.0
        let deadzone: Double = 0.03
        let magnitude: Double = snapped < 0.0 ? -snapped : snapped
        if magnitude < deadzone { snapped = 0.0 }   // 중앙 근처는 0 으로 스냅
        if snapped != offset { offset = snapped }
    }

    /// 점 p 에서 사각형 r 의 특정 변까지의 거리. 변을 선분으로 보고 최단거리를 구한다.
    private func distance(from p: CGPoint, toEdge e: ClientEdge, of r: CGRect) -> CGFloat {
        switch e {
        case .right:  return hypot(p.x - r.maxX, clampGap(p.y, r.minY, r.maxY))
        case .left:   return hypot(p.x - r.minX, clampGap(p.y, r.minY, r.maxY))
        case .top:    return hypot(clampGap(p.x, r.minX, r.maxX), p.y - r.minY)
        case .bottom: return hypot(clampGap(p.x, r.minX, r.maxX), p.y - r.maxY)
        }
    }

    /// v 가 [lo, hi] 밖이면 벗어난 거리, 안이면 0.
    private func clampGap(_ v: CGFloat, _ lo: CGFloat, _ hi: CGFloat) -> CGFloat {
        if v < lo { return lo - v }
        if v > hi { return v - hi }
        return 0
    }
}

// ── 개별 모니터 박스 ────────────────────────────────────────────────

private struct MonitorBox: View {
    let title: String
    let caption: String
    let tint: Color
    let selected: Bool
    /// 기준 화면 강조 — 어느 host 화면이 전환 기준인지 한눈에 보이게 한다.
    let highlighted: Bool

    // 중첩 삼항으로 색을 고르면 타입 추론이 무거워져 일부 Swift 버전이 실패한다.
    // 상태별 값을 미리 확정해 body 를 단순하게 유지한다.
    private var fillColor: Color {
        if selected { return tint.opacity(0.15) }
        if highlighted { return Color.accentColor.opacity(0.08) }
        return Color.primary.opacity(0.04)
    }

    private var strokeColor: Color {
        if selected { return tint }
        if highlighted { return Color.accentColor.opacity(0.55) }
        return Color.primary.opacity(0.18)
    }

    private var strokeWidth: CGFloat {
        if selected { return 2 }
        if highlighted { return 1.5 }
        return 1
    }

    var body: some View {
        VStack(spacing: 2) {
            Text(title)
                .font(.system(size: 10, weight: .semibold))
                .lineLimit(1)
                .minimumScaleFactor(0.7)
            Text(caption)
                .font(.system(size: 9))
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
        }
        .padding(2)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(
            RoundedRectangle(cornerRadius: 6).fill(fillColor)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 6)
                .stroke(strokeColor, lineWidth: strokeWidth)
        )
    }
}
