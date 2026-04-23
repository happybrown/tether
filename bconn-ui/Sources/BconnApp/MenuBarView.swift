import SwiftUI
import BconnCore

struct MenuBarView: View {
    @ObservedObject private var manager  = BconnManager.shared
    @ObservedObject private var settings = AppSettings.shared
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(spacing: 0) {
            // ── 상태 헤더 ──────────────────────────────────────────
            HStack(spacing: 10) {
                ZStack {
                    Circle()
                        .fill(manager.statusColor.opacity(0.15))
                        .frame(width: 28, height: 28)
                    Circle()
                        .fill(manager.statusColor)
                        .frame(width: 8, height: 8)
                }
                VStack(alignment: .leading, spacing: 1) {
                    Text(manager.statusLabel)
                        .font(.system(size: 13, weight: .semibold))
                    Text(settings.mode.label + " 모드")
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                }
                Spacer()
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 12)

            Divider().padding(.horizontal, 8)

            // ── 연결 정보 ──────────────────────────────────────────
            VStack(spacing: 6) {
                if settings.mode == .host {
                    InfoRow(icon: "network", label: "포트",        value: "\(settings.port)")
                    InfoRow(icon: "arrow.right.square", label: "Client 위치", value: settings.clientEdge.label)
                    if let ip = manager.localIPs.first {
                        InfoRow(icon: "desktopcomputer", label: "내 IP", value: ip)
                    }
                    if !manager.connectedHostname.isEmpty {
                        InfoRow(icon: "checkmark.shield.fill", label: "연결된 Client",
                                value: manager.connectedHostname)
                    }
                } else {
                    InfoRow(icon: "desktopcomputer", label: "Host",
                            value: settings.hostIP.isEmpty ? "미설정" : settings.hostIP)
                    InfoRow(icon: "network", label: "포트", value: "\(settings.port)")
                    if manager.authFailed {
                        HStack(spacing: 6) {
                            Image(systemName: "exclamationmark.triangle.fill")
                                .foregroundStyle(.orange)
                                .font(.system(size: 12))
                            Text("PSK 불일치 — 공유 암호를 확인하세요")
                                .font(.system(size: 11))
                                .foregroundStyle(.orange)
                        }
                        .padding(.top, 2)
                    }
                }
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)

            Divider().padding(.horizontal, 8)

            // ── 시작 / 중지 ────────────────────────────────────────
            if manager.isRunning {
                Button(action: { manager.stop() }) {
                    HStack(spacing: 6) {
                        Image(systemName: "stop.fill")
                        Text("중지")
                            .fontWeight(.medium)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 7)
                    .background(Color.red.opacity(0.12))
                    .foregroundStyle(.red)
                    .clipShape(RoundedRectangle(cornerRadius: 8))
                }
                .buttonStyle(.plain)
                .padding(.horizontal, 14)
                .padding(.vertical, 8)
            } else {
                Button(action: { manager.start() }) {
                    HStack(spacing: 6) {
                        Image(systemName: "play.fill")
                        Text("시작")
                            .fontWeight(.medium)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 7)
                    .background(canStart ? Color.accentColor.opacity(0.12) : Color.secondary.opacity(0.08))
                    .foregroundStyle(canStart ? Color.accentColor : Color.secondary)
                    .clipShape(RoundedRectangle(cornerRadius: 8))
                }
                .buttonStyle(.plain)
                .disabled(!canStart)
                .padding(.horizontal, 14)
                .padding(.vertical, 8)
            }

            Divider().padding(.horizontal, 8)

            // ── 메뉴 액션 ──────────────────────────────────────────
            VStack(spacing: 2) {
                MenuRowButton(icon: "gear", label: "설정...") {
                    openWindow(id: "settings")
                }
                .keyboardShortcut(",")

                MenuRowButton(icon: "power", label: "종료") {
                    NSApplication.shared.terminate(nil)
                }
                .keyboardShortcut("q")
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 6)
        }
        .frame(width: 260)
    }

    private var canStart: Bool {
        settings.mode == .host || !settings.hostIP.trimmingCharacters(in: .whitespaces).isEmpty
    }
}

private struct InfoRow: View {
    let icon: String
    let label: String
    let value: String

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: icon)
                .foregroundStyle(.secondary)
                .frame(width: 14)
            Text(label)
                .foregroundStyle(.secondary)
                .font(.system(size: 12))
            Spacer()
            Text(value)
                .font(.system(size: 12, design: .monospaced))
                .foregroundStyle(.primary)
                .textSelection(.enabled)
        }
    }
}

private struct MenuRowButton: View {
    let icon: String
    let label: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 8) {
                Image(systemName: icon)
                    .frame(width: 16)
                Text(label)
                Spacer()
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 6)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .clipShape(RoundedRectangle(cornerRadius: 6))
    }
}
