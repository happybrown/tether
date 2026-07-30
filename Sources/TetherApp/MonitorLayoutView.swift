import SwiftUI
import AppKit

/// Host/Client 모니터를 실제 해상도 비례 사각형으로 그리고, 상대 Mac 사각형을
/// 마우스로 끌어 배치(edge)와 정렬 오프셋을 함께 조절하는 뷰.
///
/// 조작 규칙:
/// - 상대 Mac 사각형을 host 의 상하좌우로 끌면 그 방향이 커서 전환 경계(edge)가 된다.
/// - 경계와 평행한 축으로 끌면 정렬 오프셋(-1.0 ~ +1.0, client 화면 크기 대비)이 바뀐다.
///   오프셋 0 = host 경계 중앙 정렬. 값은 C 코어의 진입/복귀 비율 변환에 쓰인다.
struct MonitorLayoutView: View {
    @Binding var edge: ClientEdge
    @Binding var offset: Double
    /// host 화면 해상도 (pt). 사각형 비례의 기준.
    var hostResolution: CGSize
    /// client 화면 해상도 (pt). 미연결 등으로 모르면 nil → host 와 같은 비례로 그린다.
    var clientResolution: CGSize?

    private let canvasHeight: CGFloat = 190
    private let margin: CGFloat = 10

    var body: some View {
        GeometryReader { geo in
            let m = metrics(in: geo.size)
            ZStack {
                MonitorBox(title: "이 Mac", caption: resolutionText(hostResolution),
                           tint: .secondary, selected: false)
                    .frame(width: m.host.width, height: m.host.height)
                    .position(x: m.host.midX, y: m.host.midY)

                MonitorBox(title: "상대 Mac", caption: clientCaption,
                           tint: .accentColor, selected: true)
                    .frame(width: m.client.width, height: m.client.height)
                    .position(x: m.client.midX, y: m.client.midY)
                    .gesture(dragGesture(m))
            }
            .coordinateSpace(name: Self.canvasSpace)
            .animation(.spring(response: 0.3, dampingFraction: 0.8), value: edge)
        }
        .frame(height: canvasHeight)
        .help("상대 Mac 사각형을 끌어 위치와 미세 정렬을 조절하세요")
    }

    private var clientCaption: String {
        clientResolution.map(resolutionText) ?? "해상도 미상 (미연결)"
    }

    private func resolutionText(_ s: CGSize) -> String {
        "\(Int(s.width)) × \(Int(s.height))"
    }

    // ── 배치 계산 ──────────────────────────────────────────────────

    private struct Metrics {
        let host: CGRect        // host 사각형 (캔버스 좌표)
        let client: CGRect      // client 사각형 (offset 시각 반영)
        let clientBox: CGSize   // client 사각형 크기 (드래그 → offset 환산용)
    }

    /// 실제 해상도를 캔버스에 비례 축소해 배치한다.
    /// 드래그로 어느 방향이든 옮길 수 있도록 4방향 여유를 모두 잡아 스케일을 고정한다
    /// (드래그 중 edge 가 바뀌어도 사각형 크기가 출렁이지 않는다).
    private func metrics(in size: CGSize) -> Metrics {
        let hostPx = hostResolution
        let cliPx = clientResolution ?? hostResolution

        let extentW = hostPx.width + 2 * cliPx.width
        let extentH = hostPx.height + 2 * cliPx.height
        let scale = min((size.width - margin * 2) / extentW,
                        (size.height - margin * 2) / extentH)

        let hostBox = CGSize(width: hostPx.width * scale, height: hostPx.height * scale)
        let cliBox = CGSize(width: cliPx.width * scale, height: cliPx.height * scale)
        let host = CGRect(x: (size.width - hostBox.width) / 2,
                          y: (size.height - hostBox.height) / 2,
                          width: hostBox.width, height: hostBox.height)

        // 오프셋 시각화: 경계 평행 축으로 offset × client 크기만큼 이동.
        // 캔버스를 벗어나지 않게 표시만 절반까지 clamp 한다 (실제 값은 % 표시가 기준).
        let visual = max(-0.5, min(0.5, offset))
        var center = CGPoint(x: host.midX, y: host.midY)
        switch edge {
        case .right:
            center.x = host.maxX + cliBox.width / 2
            center.y += visual * cliBox.height
        case .left:
            center.x = host.minX - cliBox.width / 2
            center.y += visual * cliBox.height
        case .top:
            center.y = host.minY - cliBox.height / 2
            center.x += visual * cliBox.width
        case .bottom:
            center.y = host.maxY + cliBox.height / 2
            center.x += visual * cliBox.width
        }
        let client = CGRect(x: center.x - cliBox.width / 2, y: center.y - cliBox.height / 2,
                            width: cliBox.width, height: cliBox.height)
        return Metrics(host: host, client: client, clientBox: cliBox)
    }

    // ── 드래그 → edge + offset 변환 ────────────────────────────────

    /// 캔버스 고정 좌표계 이름. 드래그 중 client 박스 자신이 움직여도
    /// 포인터 좌표가 흔들리지 않도록 박스 로컬이 아닌 캔버스 기준으로 읽는다.
    private static let canvasSpace = "monitorLayoutCanvas"

    private func dragGesture(_ m: Metrics) -> some Gesture {
        DragGesture(minimumDistance: 2, coordinateSpace: .named(Self.canvasSpace))
            .onChanged { v in
                let p = v.location

                // 1) 방향: host 중심 기준 정규화 거리로 가장 가까운 변을 고른다.
                let nx = (p.x - m.host.midX) / m.host.width
                let ny = (p.y - m.host.midY) / m.host.height
                let newEdge: ClientEdge = abs(nx) > abs(ny)
                    ? (nx > 0 ? .right : .left)
                    : (ny > 0 ? .bottom : .top)
                if newEdge != edge { edge = newEdge }

                // 2) 오프셋: 경계 평행 축에서 host 중앙과의 거리를 client 크기 비율로 환산.
                let raw: Double
                switch newEdge {
                case .left, .right:
                    raw = (p.y - m.host.midY) / m.clientBox.height
                case .top, .bottom:
                    raw = (p.x - m.host.midX) / m.clientBox.width
                }
                var snapped = (max(-1.0, min(1.0, raw)) * 100).rounded() / 100
                if abs(snapped) < 0.03 { snapped = 0 }   // 중앙 근처는 0 으로 스냅
                if snapped != offset { offset = snapped }
            }
    }
}

// ── 개별 모니터 박스 ────────────────────────────────────────────────

private struct MonitorBox: View {
    let title: String
    let caption: String
    let tint: Color
    let selected: Bool

    var body: some View {
        VStack(spacing: 2) {
            Text(title)
                .font(.system(size: 10, weight: .semibold))
            Text(caption)
                .font(.system(size: 9))
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(selected ? tint.opacity(0.15) : Color.primary.opacity(0.04))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 6)
                .stroke(selected ? tint : Color.primary.opacity(0.18),
                        lineWidth: selected ? 2 : 1)
        )
    }
}
