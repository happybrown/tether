import SwiftUI
import TetherCore

struct MenuBarView: View {
    @ObservedObject private var manager  = TetherManager.shared
    @ObservedObject private var settings = AppSettings.shared
    @Environment(\.openWindow) private var openWindow

    /// 설정 창의 선택 탭. 특정 탭(예: 로그)으로 창을 열 때 사용한다.
    @Binding var settingsTab: SettingsTab

    @State private var copiedIP: String?

    /// 설정 창을 지정한 탭으로 연다. 탭을 먼저 설정한 뒤 창을 띄운다.
    /// 전체화면(별도 Space)·멀티 모니터에서도 "지금 보던 화면"에 뜨도록 보정한다.
    private func openSettings(_ tab: SettingsTab = .connection) {
        settingsTab = tab
        // 클릭한 이 순간의 마우스 위치가 사용자가 보던 화면을 가장 정확히 가리킨다.
        // 창 생성은 비동기라 나중에 조회하면 팝오버가 닫히며 위치가 흐트러질 수 있어,
        // 대상 화면을 여기서 캡처해 넘긴다.
        let screen = SettingsWindow.screenUnderMouse()
        openWindow(id: SettingsWindow.id)
        SettingsWindow.present(on: screen)
    }

    var body: some View {
        VStack(spacing: 0) {
            statusHeader

            // 손쉬운 사용 권한은 두 모드 모두 필요하다.
            // host: CGEventTap 으로 입력을 가로챈다. client: CGEventPost 로 마우스/키를 주입한다.
            // 권한이 없으면 client 는 커서 이동은 되지만 클릭·키 입력이 조용히 무시된다.
            if !manager.hasAccessibilityPermission {
                permissionBanner
            }

            if manager.status == .error && !manager.statusMessage.isEmpty {
                errorBanner(message: manager.statusMessage)
            }

            if !isClientReady {
                missingHostBanner
            }

            connectionInfo
            actionButtons
            menuActions
        }
        .frame(width: 300)
        // 시스템 vibrancy 재질을 배경으로 깔아 뒤 화면이 비쳐 텍스트 대비가 흔들리는
        // 것을 막는다. 재질 위에서는 secondary/tertiary 색도 안정적으로 읽힌다.
        .background(.regularMaterial)
        .animation(.easeInOut(duration: 0.18), value: manager.status)
    }

    // ── 상태 헤더 ──────────────────────────────────────────────────────

    private var statusHeader: some View {
        HStack(spacing: 11) {
            ZStack {
                Circle()
                    .fill(manager.statusColor.opacity(0.18))
                    .frame(width: 32, height: 32)
                Image(systemName: manager.statusHeaderSymbol)
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(manager.statusColor)
            }
            .accessibilityLabel("상태: \(manager.statusLabel)")

            VStack(alignment: .leading, spacing: 2) {
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
                    .font(.system(size: 14))
                    .foregroundStyle(.secondary)
                    .padding(6)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help("설정 (⌘,)")
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 13)
        .background(
            manager.status == .remote
                ? manager.statusColor.opacity(0.10) : Color.clear
        )
        .overlay(alignment: .bottom) { hairline }
    }

    /// 섹션 사이를 나누는 은은한 구분선. Divider 보다 옅게 써서 카드형 리듬을 만든다.
    private var hairline: some View {
        Rectangle()
            .fill(Color.primary.opacity(0.08))
            .frame(height: 1)
    }

    // ── 접근성 권한 배너 (Host 모드에서만) ───────────────────────────────

    private var permissionBanner: some View {
        Button(action: { manager.openAccessibilitySettings() }) {
            HStack(alignment: .top, spacing: 9) {
                Image(systemName: "lock.shield.fill")
                    .foregroundStyle(.orange)
                VStack(alignment: .leading, spacing: 2) {
                    Text("손쉬운 사용 권한 필요")
                        .font(.system(size: 12, weight: .semibold))
                    Text(settings.mode == .host
                         ? "키보드·마우스를 가로채기 위해 권한이 필요합니다. 탭하여 시스템 설정 열기."
                         : "원격 클릭·키 입력을 주입하려면 권한이 필요합니다. 탭하여 시스템 설정 열기.")
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 0)
                Image(systemName: "arrow.up.right.square")
                    .font(.system(size: 11))
                    .foregroundStyle(.orange.opacity(0.8))
            }
            .bannerCard(tint: .orange)
        }
        .buttonStyle(.plain)
        .help("시스템 설정 > 개인 정보 보호 및 보안 > 손쉬운 사용")
    }

    // ── 에러 배너 ─────────────────────────────────────────────────────

    private func errorBanner(message: String) -> some View {
        HStack(alignment: .top, spacing: 9) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundStyle(.red)
            Text(message)
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(.red)
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
        }
        .bannerCard(tint: .red)
    }

    // ── Host IP 미설정 안내 (Client 모드) ──────────────────────────────

    private var missingHostBanner: some View {
        Button(action: { openSettings(.connection) }) {
            HStack(spacing: 9) {
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
                    .foregroundStyle(.orange.opacity(0.8))
            }
            .bannerCard(tint: .orange)
        }
        .buttonStyle(.plain)
    }

    // ── 연결 정보 ─────────────────────────────────────────────────────

    private var connectionInfo: some View {
        VStack(spacing: 9) {
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
        .padding(.horizontal, 13)
        .padding(.vertical, 11)
        // 정보를 카드 표면에 얹어, 배경과 분리된 명확한 읽기 영역을 만든다.
        .background(
            RoundedRectangle(cornerRadius: 10)
                .fill(Color.primary.opacity(0.05))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 10)
                .strokeBorder(Color.primary.opacity(0.07), lineWidth: 1)
        )
        .padding(.horizontal, 12)
        .padding(.vertical, 12)
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
            ActionButton(icon: "stop.fill", label: "중지", tint: .red,
                         help: "세션 종료") {
                manager.stop()
                dismissMenuBar()
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
            hairline.padding(.horizontal, -8).padding(.bottom, 4)

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

            MenuRowButton(icon: "power", label: "\(manager.appName) 종료", shortcut: "⌘Q") {
                NSApplication.shared.terminate(nil)
            }
            .keyboardShortcut("q")
        }
        .padding(.horizontal, 12)
        .padding(.bottom, 8)
    }

    // ── 헬퍼 ──────────────────────────────────────────────────────────

    private var canStart: Bool {
        // 두 모드 모두 손쉬운 사용 권한이 필요하다 (host: 캡처, client: 주입).
        guard manager.hasAccessibilityPermission else { return false }
        let hostReady = settings.mode == .host
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

// ── 공용 스타일 ──────────────────────────────────────────────────────────

private extension View {
    /// 경고·안내 배너를 색상 틴트 카드로 감싼다. 회색 배경 위에 텍스트가 바로 얹혀
    /// 대비가 흐려지는 것을 막고, 색으로 심각도를 즉시 구분하게 한다.
    func bannerCard(tint: Color) -> some View {
        self
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(
                RoundedRectangle(cornerRadius: 10)
                    .fill(tint.opacity(0.12))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 10)
                    .strokeBorder(tint.opacity(0.22), lineWidth: 1)
            )
            .padding(.horizontal, 12)
            .padding(.top, 12)
            .contentShape(Rectangle())
    }
}

// ── 서브 컴포넌트 ────────────────────────────────────────────────────────

private struct InfoRow: View {
    let icon: String
    let label: String
    let value: String

    var body: some View {
        HStack(spacing: 9) {
            Image(systemName: icon)
                .foregroundStyle(.secondary)
                .frame(width: 15)
            Text(label)
                .foregroundStyle(.secondary)
                .font(.system(size: 12))
            Spacer()
            Text(value)
                .font(.system(size: 12, weight: .medium, design: .monospaced))
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

    /// 하이라이트(hover 또는 press) 상태인지.
    private var highlighted: Bool { hovered || pressed }

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
                        // 하이라이트 시엔 흰 배경 위라 흰색을 살짝 흐리게, 기본 상태엔 secondary.
                        .foregroundStyle(highlighted ? Color.white.opacity(0.75) : Color.secondary)
                }
            }
            .font(.system(size: 13))
            .padding(.horizontal, 10)
            .padding(.vertical, 7)
            // macOS 메뉴 정석: 하이라이트 시 accent 색으로 배경을 꽉 채우고 전경을 흰색으로.
            // 눌린 순간엔 살짝 어둡게 해 클릭 피드백을 준다.
            .background(
                RoundedRectangle(cornerRadius: 7)
                    .fill(highlighted
                          ? Color.accentColor.opacity(pressed ? 0.85 : 1.0)
                          : Color.clear)
            )
            .foregroundStyle(highlighted ? Color.white : Color.primary)
            .contentShape(Rectangle())
            .animation(.easeOut(duration: 0.1), value: hovered)
            .animation(.easeOut(duration: 0.1), value: pressed)
        }
        .buttonStyle(.plain)
        .onHover { hovered = $0 }
    }
}
