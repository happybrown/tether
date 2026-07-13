import SwiftUI
import BconnCore

struct MenuBarView: View {
    @ObservedObject private var manager  = BconnManager.shared
    @ObservedObject private var settings = AppSettings.shared
    @Environment(\.openWindow) private var openWindow

    /// 설정 창의 선택 탭. 특정 탭(예: 로그)으로 창을 열 때 사용한다.
    @Binding var settingsTab: SettingsTab

    @State private var copiedIP: String?

    /// 설정 창을 지정한 탭으로 연다. 탭을 먼저 설정한 뒤 창을 띄운다.
    private func openSettings(_ tab: SettingsTab = .connection) {
        settingsTab = tab
        openWindow(id: "settings")
    }

    var body: some View {
        VStack(spacing: 0) {
            statusHeader

            if !manager.hasAccessibilityPermission && settings.mode == .host {
                Divider().padding(.horizontal, 8)
                permissionBanner
            }

            if manager.status == .error && !manager.statusMessage.isEmpty {
                Divider().padding(.horizontal, 8)
                errorBanner(message: manager.statusMessage)
            }

            if !isClientReady {
                Divider().padding(.horizontal, 8)
                missingHostBanner
            }

            Divider().padding(.horizontal, 8)
            connectionInfo
            Divider().padding(.horizontal, 8)
            actionButtons
            Divider().padding(.horizontal, 8)
            menuActions
        }
        .frame(width: 280)
        .animation(.easeInOut(duration: 0.18), value: manager.status)
    }

    // ── 상태 헤더 ──────────────────────────────────────────────────────

    private var statusHeader: some View {
        HStack(spacing: 11) {
            ZStack {
                Circle()
                    .fill(manager.statusColor.opacity(0.15))
                    .frame(width: 30, height: 30)
                Image(systemName: manager.statusHeaderSymbol)
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(manager.statusColor)
            }
            .accessibilityLabel("상태: \(manager.statusLabel)")

            VStack(alignment: .leading, spacing: 1) {
                Text(manager.statusLabel)
                    .font(.system(size: 13, weight: .semibold))
                    .contentTransition(.opacity)
                HStack(spacing: 5) {
                    Text(settings.mode.label + " 모드")
                    Text("v" + manager.appVersion)
                        .foregroundStyle(.tertiary)
                }
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
            }
            Spacer()

            Button { openSettings() } label: {
                Image(systemName: "gearshape")
                    .font(.system(size: 13))
                    .foregroundStyle(.secondary)
            }
            .buttonStyle(.plain)
            .help("설정 (⌘,)")
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
        .background(
            manager.status == .remote
                ? manager.statusColor.opacity(0.08) : Color.clear
        )
    }

    // ── 접근성 권한 배너 (Host 모드에서만) ───────────────────────────────

    private var permissionBanner: some View {
        Button(action: { manager.openAccessibilitySettings() }) {
            HStack(alignment: .top, spacing: 8) {
                Image(systemName: "lock.shield.fill")
                    .foregroundStyle(.orange)
                VStack(alignment: .leading, spacing: 2) {
                    Text("손쉬운 사용 권한 필요")
                        .font(.system(size: 12, weight: .semibold))
                    Text("Host 모드는 키보드·마우스를 가로채기 위해 권한이 필요합니다. 탭하여 시스템 설정 열기.")
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 0)
                Image(systemName: "arrow.up.right.square")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help("시스템 설정 > 개인 정보 보호 및 보안 > 손쉬운 사용")
    }

    // ── 에러 배너 ─────────────────────────────────────────────────────

    private func errorBanner(message: String) -> some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundStyle(.red)
            Text(message)
                .font(.system(size: 11))
                .foregroundStyle(.red)
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
    }

    // ── Host IP 미설정 안내 (Client 모드) ──────────────────────────────

    private var missingHostBanner: some View {
        Button(action: { openSettings(.connection) }) {
            HStack(spacing: 8) {
                Image(systemName: "exclamationmark.circle.fill")
                    .foregroundStyle(.orange)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Host IP 미설정")
                        .font(.system(size: 12, weight: .semibold))
                    Text("설정에서 연결할 Host 의 IP 를 입력하세요.")
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                }
                Spacer(minLength: 0)
                Image(systemName: "gear")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    // ── 연결 정보 ─────────────────────────────────────────────────────

    private var connectionInfo: some View {
        VStack(spacing: 6) {
            if settings.mode == .host {
                InfoRow(icon: "network", label: "포트", value: "\(settings.port)")
                InfoRow(icon: "arrow.right.square", label: "Client 위치",
                        value: settings.clientEdge.label)
                if let ip = manager.localIPs.first {
                    copyableIPRow(label: "내 IP", ip: ip,
                                  hint: "Client 쪽 설정에 이 주소를 입력하세요")
                }
                if !manager.connectedHostname.isEmpty {
                    InfoRow(icon: "checkmark.shield.fill", label: "연결된 Client",
                            value: manager.connectedHostname)
                }
            } else {
                InfoRow(icon: "desktopcomputer", label: "Host",
                        value: settings.hostIP.isEmpty ? "미설정" : settings.hostIP)
                InfoRow(icon: "network", label: "포트", value: "\(settings.port)")
                if let ip = manager.localIPs.first {
                    copyableIPRow(label: "내 IP", ip: ip,
                                  hint: "Host 에서 이 주소로 접속합니다")
                }
                if manager.authFailed {
                    pskMismatchRow
                }
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
    }

    private func copyableIPRow(label: String, ip: String, hint: String) -> some View {
        HStack(spacing: 8) {
            Image(systemName: "desktopcomputer")
                .foregroundStyle(.secondary)
                .frame(width: 14)
            Text(label)
                .foregroundStyle(.secondary)
                .font(.system(size: 12))
            Spacer()
            Button(action: { copy(ip) }) {
                HStack(spacing: 4) {
                    Text(ip)
                        .font(.system(size: 12, design: .monospaced))
                        .foregroundStyle(.primary)
                    Image(systemName: copiedIP == ip ? "checkmark" : "doc.on.doc")
                        .font(.system(size: 10))
                        .foregroundStyle(copiedIP == ip ? .green : .secondary)
                }
            }
            .buttonStyle(.plain)
            .help(hint)
        }
    }

    private var pskMismatchRow: some View {
        Button(action: { openSettings(.security) }) {
            HStack(spacing: 6) {
                Image(systemName: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
                    .font(.system(size: 12))
                Text("PSK 불일치 — 공유 암호 수정")
                    .font(.system(size: 11))
                    .foregroundStyle(.orange)
                Spacer()
                Image(systemName: "chevron.right")
                    .font(.system(size: 10))
                    .foregroundStyle(.secondary)
            }
            .padding(.top, 2)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    // ── 액션 버튼 ─────────────────────────────────────────────────────

    @ViewBuilder
    private var actionButtons: some View {
        if manager.isRunning {
            VStack(spacing: 8) {
                if manager.status == .remote {
                    ActionButton(
                        icon: "arrow.uturn.backward",
                        label: "강제 로컬 복귀",
                        tint: .orange,
                        help: "다른 Mac 으로 넘어간 입력을 되찾습니다 (Ctrl+Opt+Cmd+Esc)"
                    ) { bconn_force_local() }
                }
                ActionButton(icon: "stop.fill", label: "중지", tint: .red,
                             help: "세션 종료") {
                    manager.stop()
                    dismissMenuBar()
                }
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 8)
        } else {
            ActionButton(
                icon: "play.fill",
                label: "시작",
                tint: canStart ? .accentColor : .secondary,
                help: canStart ? "세션 시작" : "설정이 완료되어야 시작할 수 있습니다"
            ) {
                manager.start()
                dismissMenuBar()
            }
            .opacity(canStart ? 1.0 : 0.6)
            .disabled(!canStart)
            .padding(.horizontal, 14)
            .padding(.vertical, 8)
        }
    }

    // ── 메뉴 액션 ─────────────────────────────────────────────────────

    private var menuActions: some View {
        VStack(spacing: 2) {
            MenuRowButton(icon: "gear", label: "설정...", shortcut: "⌘,") {
                openSettings()
            }
            .keyboardShortcut(",")

            MenuRowButton(icon: "doc.text.magnifyingglass",
                          label: "로그...",
                          shortcut: "⌘L") {
                openSettings(.logs)
            }
            .keyboardShortcut("l")

            MenuRowButton(icon: "power", label: "종료", shortcut: "⌘Q") {
                NSApplication.shared.terminate(nil)
            }
            .keyboardShortcut("q")
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
    }

    // ── 헬퍼 ──────────────────────────────────────────────────────────

    private var canStart: Bool {
        let hostReady = settings.mode == .host && manager.hasAccessibilityPermission
        let clientReady = settings.mode == .client && isClientReady
        return hostReady || clientReady
    }

    private var isClientReady: Bool {
        settings.mode != .client ||
            !settings.hostIP.trimmingCharacters(in: .whitespaces).isEmpty
    }

    private func copy(_ value: String) {
        let pb = NSPasteboard.general
        pb.clearContents()
        pb.setString(value, forType: .string)
        copiedIP = value
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) {
            if copiedIP == value { copiedIP = nil }
        }
    }

    /// 메뉴바 팝오버를 닫는다. 시작/중지 버튼을 눌렀을 때 macOS 관례처럼 자동 닫힘.
    private func dismissMenuBar() {
        NSApp.windows.first { $0.className.contains("MenuBarExtra") }?.resignKey()
        NSApp.deactivate()
    }
}

// ── 서브 컴포넌트 ────────────────────────────────────────────────────────

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
                .lineLimit(1)
                .truncationMode(.middle)
        }
    }
}

private struct ActionButton: View {
    let icon: String
    let label: String
    let tint: Color
    let help: String
    let action: () -> Void

    @State private var pressed = false

    var body: some View {
        Button {
            pressed = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) { pressed = false }
            action()
        } label: {
            HStack(spacing: 6) {
                Image(systemName: icon)
                Text(label).fontWeight(.medium)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 7)
            .background(tint.opacity(pressed ? 0.28 : 0.12))
            .foregroundStyle(tint)
            .clipShape(RoundedRectangle(cornerRadius: 8))
            .animation(.easeOut(duration: 0.12), value: pressed)
        }
        .buttonStyle(.plain)
        .help(help)
    }
}

private struct MenuRowButton: View {
    let icon: String
    let label: String
    let shortcut: String?
    let action: () -> Void

    @State private var hovered = false
    @State private var pressed = false

    init(icon: String, label: String, shortcut: String? = nil, action: @escaping () -> Void) {
        self.icon = icon
        self.label = label
        self.shortcut = shortcut
        self.action = action
    }

    var body: some View {
        Button {
            pressed = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) { pressed = false }
            action()
        } label: {
            HStack(spacing: 8) {
                Image(systemName: icon)
                    .frame(width: 16)
                Text(label)
                Spacer()
                if let shortcut {
                    Text(shortcut)
                        .font(.system(size: 11))
                        .foregroundStyle(Color.secondary)
                }
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 6)
            .background(
                RoundedRectangle(cornerRadius: 6)
                    .fill(pressed ? Color.accentColor.opacity(0.25) :
                          hovered ? Color.accentColor.opacity(0.12) : Color.clear)
            )
            .foregroundStyle(hovered || pressed ? Color.primary : Color.primary.opacity(0.85))
            .contentShape(Rectangle())
            .animation(.easeOut(duration: 0.1), value: hovered)
            .animation(.easeOut(duration: 0.1), value: pressed)
        }
        .buttonStyle(.plain)
        .onHover { hovered = $0 }
    }
}
