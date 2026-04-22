import SwiftUI
import BconnCore

struct MenuBarView: View {
    @ObservedObject private var manager  = BconnManager.shared
    @ObservedObject private var settings = AppSettings.shared

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
                    InfoRow(label: "포트",       value: "\(settings.port)")
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

            // ── 제어 버튼 ──────────────────────────────────────────
            HStack(spacing: 8) {
                if manager.isRunning {
                    Button(action: { manager.stop() }) {
                        Label("중지", systemImage: "stop.fill")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.red)
                } else {
                    Button(action: { manager.start() }) {
                        Label("시작", systemImage: "play.fill")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.green)
                    .disabled(!canStart)
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)

            Divider()

            // ── 메뉴 액션 ──────────────────────────────────────────
            VStack(spacing: 0) {
                MenuButton(label: "설정...", shortcut: ",") {
                    AppDelegate.shared.openSettings()
                }
                MenuButton(label: "종료", shortcut: "q") {
                    NSApplication.shared.terminate(nil)
                }
            }
            .padding(.vertical, 4)
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

private struct MenuButton: View {
    let label: String
    let shortcut: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack {
                Text(label)
                Spacer()
                Text("⌘\(shortcut.uppercased())")
                    .foregroundStyle(.secondary)
                    .font(.caption)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .keyboardShortcut(KeyEquivalent(shortcut.first!), modifiers: .command)
    }
}
