import SwiftUI
import BconnCore

struct MenuBarView: View {
    @ObservedObject private var manager  = BconnManager.shared
    @ObservedObject private var settings = AppSettings.shared
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // ── 상태 헤더 ──────────────────────────────────────────
            HStack(spacing: 8) {
                Circle()
                    .fill(manager.statusColor)
                    .frame(width: 8, height: 8)
                Text(manager.statusLabel)
                    .font(.headline)
                Spacer()
                Text(settings.mode.label)
                    .font(.caption)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(.secondary.opacity(0.2))
                    .clipShape(Capsule())
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)

            Divider()

            // ── 연결 정보 ──────────────────────────────────────────
            VStack(alignment: .leading, spacing: 4) {
                if settings.mode == .host {
                    InfoRow(label: "포트",        value: "\(settings.port)")
                    InfoRow(label: "Client 위치", value: settings.clientEdge.label)
                    if let ip = manager.localIPs.first {
                        InfoRow(label: "내 IP", value: ip)
                    }
                } else {
                    InfoRow(label: "Host", value: settings.hostIP.isEmpty ? "미설정" : settings.hostIP)
                    InfoRow(label: "포트",  value: "\(settings.port)")
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)

            Divider()

            // ── 시작 / 중지 ────────────────────────────────────────
            Group {
                if manager.isRunning {
                    Button(role: .destructive, action: { manager.stop() }) {
                        Label("중지", systemImage: "stop.fill")
                            .frame(maxWidth: .infinity)
                    }
                } else {
                    Button(action: { manager.start() }) {
                        Label("시작", systemImage: "play.fill")
                            .frame(maxWidth: .infinity)
                    }
                    .disabled(!canStart)
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)

            Divider()

            // ── 메뉴 액션 ──────────────────────────────────────────
            Button("설정...") {
                openWindow(id: "settings")
            }
            .keyboardShortcut(",")

            Button("종료", role: .destructive) {
                NSApplication.shared.terminate(nil)
            }
            .keyboardShortcut("q")
        }
        .frame(width: 240)
    }

    private var canStart: Bool {
        settings.mode == .host || !settings.hostIP.trimmingCharacters(in: .whitespaces).isEmpty
    }
}

private struct InfoRow: View {
    let label: String
    let value: String

    var body: some View {
        HStack {
            Text(label)
                .foregroundStyle(.secondary)
                .font(.caption)
            Spacer()
            Text(value)
                .font(.caption)
                .textSelection(.enabled)
        }
    }
}
