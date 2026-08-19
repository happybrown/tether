import SwiftUI

/// 설정 창의 "네트워크" 탭. 연결된 상대 Mac 과의 실제 UDP 왕복 지연(RTT)을 측정한다.
/// 마우스/키보드가 타는 바로 그 소켓 경로를 재사용하므로 체감 지연을 정확히 반영한다.
struct NetworkTestView: View {
    @ObservedObject private var manager = TetherManager.shared
    @ObservedObject private var history = NetPerfHistory.shared
    // 언어 변경 시 즉시 다시 그려지도록 관찰한다.
    @ObservedObject private var l10n    = L10n.shared

    @State private var running = false
    @State private var result: NetPerfResult?

    // 측정 파라미터 — 기본 50회 / 100ms 간격(약 5초).
    private let probeCount = 50
    private let intervalMs = 100

    private var canMeasure: Bool {
        manager.isRunning && manager.status != .error && manager.status != .authFailed
    }

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                FormSection(title: L("netperf.section"), icon: "speedometer") {
                    VStack(spacing: 0) {
                        FormRow(label: L("netperf.measure")) {
                            measureButton
                        }
                        Divider().padding(.leading, 16)
                        descriptionRow
                    }
                }

                if let result {
                    resultSection(result)
                }

                if history.entries.count >= 2 {
                    chartSection
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
                    Text(L("netperf.measuring"))
                } else {
                    Image(systemName: "play.fill")
                    Text(L("netperf.start"))
                }
            }
            .frame(minWidth: 96)
        }
        .disabled(running || !canMeasure)
        .help(canMeasure ? L("netperf.help_format", probeCount, probeCount * intervalMs / 1000)
                         : L("netperf.help_disconnected"))
    }

    private var descriptionRow: some View {
        HStack {
            Text(canMeasure
                 ? L("netperf.desc_format", probeCount, probeCount * intervalMs / 1000)
                 : L("netperf.desc_disconnected"))
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
        FormSection(title: L("netperf.result"), icon: "chart.bar") {
            VStack(spacing: 0) {
                if r.hasData {
                    gradeRow(r)
                    Divider().padding(.leading, 16)
                    metricRow(L("netperf.avg"), r.avgMs, emphasize: true)
                    Divider().padding(.leading, 16)
                    metricRow(L("netperf.min"), r.minMs)
                    Divider().padding(.leading, 16)
                    metricRow(L("netperf.max"), r.maxMs)
                    Divider().padding(.leading, 16)
                    metricRow(L("netperf.jitter"), r.stddevMs)
                    Divider().padding(.leading, 16)
                    lossRow(r)
                } else {
                    HStack {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .foregroundStyle(.orange)
                        Text(L("netperf.no_response"))
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
        return FormRow(label: L("netperf.grade")) {
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
        FormRow(label: L("netperf.loss")) {
            Text(String(format: "%.0f%% (%d/%d)", r.lossPct, r.received, r.sent))
                .font(.system(size: 13, design: .monospaced))
                .foregroundStyle(r.lossPct > 0 ? .orange : .secondary)
        }
    }

    // ── 추이 차트 ─────────────────────────────────────────────────────

    private var chartSection: some View {
        FormSection(title: L("netperf.history_format", NetPerfHistory.maxEntries),
                    icon: "chart.xyaxis.line") {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 12) {
                    legendItem(color: .blue, filled: true, label: L("netperf.avg_rtt"))
                    legendItem(color: .blue.opacity(0.3), filled: false, label: L("netperf.min_max"))
                    Spacer()
                    Button(L("netperf.clear_history")) { history.clear() }
                        .font(.system(size: 11))
                        .buttonStyle(.plain)
                        .foregroundStyle(.secondary)
                }
                NetPerfChartView(entries: history.entries)
            }
            .padding(16)
        }
    }

    private func legendItem(color: Color, filled: Bool, label: String) -> some View {
        HStack(spacing: 5) {
            RoundedRectangle(cornerRadius: 2)
                .fill(filled ? color : color.opacity(0.5))
                .frame(width: 14, height: filled ? 3 : 8)
            Text(label)
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
        }
    }

    // ── 측정 실행 ─────────────────────────────────────────────────────

    private func startMeasure() {
        running = true
        result = nil
        let ok = manager.runNetPerf(count: probeCount, intervalMs: intervalMs) { r in
            self.result = r
            self.running = false
            self.history.add(r)   // 유효한 측정만 내부에서 걸러 누적
        }
        if !ok { running = false }
    }
}
