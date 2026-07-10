import SwiftUI

/// Host 모니터를 중심에 두고, Client 모니터를 상하좌우 어디에 배치할지
/// 시각적으로 선택하는 뷰. 선택한 방향이 곧 커서 전환 경계(edge)가 된다.
///
/// 배치 좌표 규칙:
/// - Client가 host의 "오른쪽(right)"에 있으면 → 커서가 오른쪽 경계에서 전환된다.
/// - 나머지 방향도 동일하게 대응된다.
///
/// 3×3 그리드로 배치해 각 방향 슬롯이 독립된 히트 영역을 갖도록 한다.
/// (offset 기반은 레이아웃 프레임이 중앙에 겹쳐 클릭이 한 곳으로만 가는 문제가 있다.)
struct MonitorLayoutView: View {
    @Binding var edge: ClientEdge

    private let hostSize = CGSize(width: 104, height: 66)
    private let peerSize = CGSize(width: 88,  height: 56)
    private let spacing: CGFloat = 8

    var body: some View {
        VStack(spacing: spacing) {
            row(center: .top)
            HStack(spacing: spacing) {
                slot(.left)
                hostMonitor
                slot(.right)
            }
            row(center: .bottom)
        }
        .animation(.spring(response: 0.3, dampingFraction: 0.8), value: edge)
    }

    // 위/아래 슬롯은 host 열(가운데)에만 두고 좌우는 빈 공간으로 정렬을 맞춘다.
    private func row(center slot: ClientEdge) -> some View {
        HStack(spacing: spacing) {
            Color.clear.frame(width: peerSize.width, height: peerSize.height)
            slotBox(slot)
            Color.clear.frame(width: peerSize.width, height: peerSize.height)
        }
    }

    private func slot(_ slot: ClientEdge) -> some View {
        slotBox(slot)
    }

    private var hostMonitor: some View {
        MonitorBox(
            title: "이 Mac",
            subtitle: "Host",
            size: hostSize,
            tint: .secondary,
            selected: false
        )
    }

    // ── 선택 가능한 Client 슬롯 ────────────────────────────────────

    private func slotBox(_ slot: ClientEdge) -> some View {
        let isSelected = edge == slot
        return MonitorBox(
            title: "상대 Mac",
            subtitle: slot.label,
            size: peerSize,
            tint: .accentColor,
            selected: isSelected
        )
        .opacity(isSelected ? 1 : 0.4)
        .contentShape(Rectangle())
        .onTapGesture { edge = slot }
        .help("상대 Mac을 \(slot.label)에 배치")
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
                .font(.system(size: 10, weight: .semibold))
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
                .stroke(selected ? tint : Color.primary.opacity(0.18),
                        lineWidth: selected ? 2 : 1)
        )
    }
}
