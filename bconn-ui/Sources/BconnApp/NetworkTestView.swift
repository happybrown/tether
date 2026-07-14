import SwiftUI

/// 설정 창의 "네트워크" 탭. 연결된 상대 Mac 과의 실제 UDP 왕복 지연(RTT)을 측정한다.
/// 마우스/키보드가 타는 바로 그 소켓 경로를 재사용하므로 체감 지연을 정확히 반영한다.
struct NetworkTestView: View {
    @ObservedObject private var manager = BconnManager.shared

    @State private var running = false
    @State private var result: NetPerfResult?
    @State private var progressNote = ""

    // 측정 파라미터 — 기본 50회 / 100ms 간격(약 5초).
    private let probeCount = 50
    private let intervalMs = 100

    private var canMeasure: Bool {
        manager.isRunning && manager.status != .error && manager.status != .authFailed
    }

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                FormSection(title: "왕복 지연 측정", icon: "speedometer") {
                    VStack(spacing: 0) {
                        FormRow(label: "측정") {
                            measureButton
                        }
                        Divider().padding(.leading, 16)
                        descriptionRow
                    }
                }

                if let result {
                    resultSection(result)
                }

                Spacer(minLength: 0)
            }
            .padding(16)
        }
    }

    // ── 측정 버튼 ─────────────────────────────────────────────────────

    private var measureButton: some View {
        Button(action: startMeasure) {
            HStack(spacing: 6) {
                if running {
                    ProgressView().controlSize(.small)
                    Text("측정 중…")
                } else {
                    Image(systemName: "play.fill")
                    Text("측정 시작")
                }
            }
            .frame(minWidth: 96)
        }
        .disabled(running || !canMeasure)
        .help(canMeasure ? "약 5초간 \(probeCount)회 왕복 측정"
                         : "연결된 상태에서만 측정할 수 있습니다")
    }

    private var descriptionRow: some View {
        HStack {
            Text(canMeasure
                 ? "\(probeCount)회 왕복 지연을 측정합니다 (약 \(probeCount * intervalMs / 1000)초)."
                 : "상대 Mac 과 연결된 뒤 측정할 수 있습니다.")
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }

    // ── 결과 ──────────────────────────────────────────────────────────

    @ViewBuilder
    private func resultSection(_ r: NetPerfResult) -> some View {
        FormSection(title: "결과", icon: "chart.bar") {
            VStack(spacing: 0) {
                if r.hasData {
                    gradeRow(r)
                    Divider().padding(.leading, 16)
                    metricRow("평균", r.avgMs, emphasize: true)
                    Divider().padding(.leading, 16)
                    metricRow("최소", r.minMs)
                    Divider().padding(.leading, 16)
                    metricRow("최대", r.maxMs)
                    Divider().padding(.leading, 16)
                    metricRow("지터(표준편차)", r.stddevMs)
                    Divider().padding(.leading, 16)
                    lossRow(r)
                } else {
                    HStack {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .foregroundStyle(.orange)
                        Text("응답이 없습니다. 연결 상태를 확인하세요.")
                            .font(.system(size: 12))
                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 12)
                }
            }
        }
    }

    private func gradeRow(_ r: NetPerfResult) -> some View {
        let g = r.grade
        return FormRow(label: "체감 등급") {
            HStack(spacing: 6) {
                Circle().fill(g.color).frame(width: 8, height: 8)
                Text(g.label)
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(g.color)
            }
        }
    }

    private func metricRow(_ label: String, _ ms: Double, emphasize: Bool = false) -> some View {
        FormRow(label: label) {
            Text(String(format: "%.1f ms", ms))
                .font(.system(size: emphasize ? 14 : 13,
                              weight: emphasize ? .semibold : .regular,
                              design: .monospaced))
                .foregroundStyle(emphasize ? .primary : .secondary)
        }
    }

    private func lossRow(_ r: NetPerfResult) -> some View {
        FormRow(label: "패킷 손실") {
            Text(String(format: "%.0f%% (%d/%d)", r.lossPct, r.received, r.sent))
                .font(.system(size: 13, design: .monospaced))
                .foregroundStyle(r.lossPct > 0 ? .orange : .secondary)
        }
    }

    // ── 측정 실행 ─────────────────────────────────────────────────────

    private func startMeasure() {
        running = true
        result = nil
        let ok = manager.runNetPerf(count: probeCount, intervalMs: intervalMs) { r in
            self.result = r
            self.running = false
        }
        if !ok { running = false }
    }
}
