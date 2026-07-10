import SwiftUI

/// Host 모니터를 중심에 두고, Client 모니터를 상하좌우 어디에 배치할지
/// 시각적으로 선택하는 뷰. 선택한 방향이 곧 커서 전환 경계(edge)가 된다.
///
/// 배치 좌표 규칙:
/// - Client가 host의 "오른쪽(right)"에 있으면 → 커서가 오른쪽 경계에서 전환된다.
/// - 나머지 방향도 동일하게 대응된다.
struct MonitorLayoutView: View {
    @Binding var edge: ClientEdge

    // Host 모니터 박스 크기 (Client 박스도 동일 비율로 표시)
    private let hostSize  = CGSize(width: 108, height: 68)
    private let peerSize  = CGSize(width: 92,  height: 58)
    private let gap: CGFloat = 10

    var body: some View {
        ZStack {
            hostMonitor
            ForEach(ClientEdge.allCases) { slot in
                clientSlot(slot)
            }
        }
        .frame(width: canvasSize.width, height: canvasSize.height)
        .animation(.spring(response: 0.3, dampingFraction: 0.8), value: edge)
    }

    // ── 캔버스 크기 ────────────────────────────────────────────────
    // Host를 중앙에 두고 양옆/위아래로 Client 박스가 들어갈 공간을 확보한다.

    private var canvasSize: CGSize {
        CGSize(
            width:  hostSize.width  + 2 * (peerSize.width  + gap),
            height: hostSize.height + 2 * (peerSize.height + gap)
        )
    }

    // ── Host 모니터 (중앙 고정) ────────────────────────────────────

    private var hostMonitor: some View {
        MonitorBox(
            title: "이 Mac",
            subtitle: "Host",
            size: hostSize,
            tint: .secondary,
            selected: false
        )
    }

    // ── Client 배치 슬롯 (선택 가능) ───────────────────────────────

    private func clientSlot(_ slot: ClientEdge) -> some View {
        let isSelected = edge == slot
        return MonitorBox(
            title: "상대 Mac",
            subtitle: slot.label,
            size: peerSize,
            tint: .accentColor,
            selected: isSelected
        )
        .opacity(isSelected ? 1 : 0.28)
        .scaleEffect(isSelected ? 1 : 0.9)
        .offset(offset(for: slot))
        .contentShape(Rectangle())
        .onTapGesture { edge = slot }
        .help("상대 Mac을 \(slot.label)에 배치")
    }

    /// 각 방향 슬롯을 host 박스 기준으로 배치할 오프셋.
    private func offset(for slot: ClientEdge) -> CGSize {
        let dx = (hostSize.width  + peerSize.width)  / 2 + gap
        let dy = (hostSize.height + peerSize.height) / 2 + gap
        switch slot {
        case .right:  return CGSize(width:  dx, height: 0)
        case .left:   return CGSize(width: -dx, height: 0)
        case .top:    return CGSize(width: 0, height: -dy)
        case .bottom: return CGSize(width: 0, height:  dy)
        }
    }
}

// ── 개별 모니터 박스 ────────────────────────────────────────────────

private struct MonitorBox: View {
    let title: String
    let subtitle: String
    let size: CGSize
    let tint: Color
    let selected: Bool

    var body: some View {
        VStack(spacing: 2) {
            Text(title)
                .font(.system(size: 11, weight: .semibold))
            Text(subtitle)
                .font(.system(size: 9))
                .foregroundStyle(.secondary)
        }
        .frame(width: size.width, height: size.height)
        .background(
            RoundedRectangle(cornerRadius: 8)
                .fill(selected ? tint.opacity(0.15) : Color.primary.opacity(0.04))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .stroke(selected ? tint : Color.primary.opacity(0.15),
                        lineWidth: selected ? 2 : 1)
        )
    }
}
