import SwiftUI

struct SettingsView: View {
    @ObservedObject private var settings = AppSettings.shared
    @ObservedObject private var manager  = BconnManager.shared

    @State private var portString: String = ""
    @State private var portError: String?

    var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(spacing: 20) {
                    modeSection
                    if settings.mode == .host && !manager.hasAccessibilityPermission {
                        permissionSection
                    }
                    connectionSection
                    networkSection
                    securitySection
                    if settings.mode == .host {
                        shortcutSection
                    }
                    generalSection
                }
                .padding(24)
            }

            // ── 하단 상태바 + 버튼 ────────────────────────────────
            Divider()
            HStack(spacing: 8) {
                Image(systemName: manager.statusHeaderSymbol)
                    .font(.system(size: 12))
                    .foregroundStyle(manager.statusColor)
                Text(manager.statusLabel)
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(.secondary)
                Spacer()
                if manager.isRunning {
                    Button(role: .destructive) { manager.stop() } label: {
                        Label("중지", systemImage: "stop.fill")
                    }
                    .keyboardShortcut(.escape, modifiers: [])
                } else {
                    Button { manager.start() } label: {
                        Label("시작", systemImage: "play.fill")
                    }
                    .keyboardShortcut(.return, modifiers: [])
                    .buttonStyle(.borderedProminent)
                    .disabled(!canStart)
                }
            }
            .padding(.horizontal, 24)
            .padding(.vertical, 12)
        }
        .frame(width: 520)
        .onAppear {
            portString = String(settings.port)
            manager.refreshLocalIPs()
            manager.refreshAccessibilityPermission()
        }
        .onChange(of: settings.port) { newValue in
            if portString != String(newValue) { portString = String(newValue) }
        }
    }

    // ── 모드 ──────────────────────────────────────────────────────

    private var modeSection: some View {
        FormSection(title: "모드", icon: "person.2") {
            VStack(spacing: 0) {
                FormRow(label: "역할") {
                    Picker("", selection: $settings.mode) {
                        ForEach(AppMode.allCases) { Text($0.label).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    .frame(width: 200)
                    .labelsHidden()
                    .disabled(manager.isRunning)
                }
                if manager.isRunning {
                    Divider().padding(.leading, 16)
                    HStack(spacing: 6) {
                        Image(systemName: "lock.fill").font(.system(size: 10))
                        Text("실행 중에는 모드를 변경할 수 없습니다")
                            .font(.system(size: 11))
                    }
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 16)
                    .padding(.vertical, 8)
                }
            }
        }
    }

    // ── 권한 배너 ─────────────────────────────────────────────────

    private var permissionSection: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: "lock.shield.fill")
                .foregroundStyle(.orange)
                .font(.system(size: 20))
                .padding(.top, 1)
            VStack(alignment: .leading, spacing: 3) {
                Text("손쉬운 사용 권한 필요")
                    .font(.system(size: 13, weight: .semibold))
                Text("Host 모드는 키보드·마우스 이벤트를 가로채기 위해 시스템 권한이 필요합니다.")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
            Button("시스템 설정 열기") { manager.openAccessibilitySettings() }
                .controlSize(.small)
        }
        .padding(14)
        .background(Color.orange.opacity(0.08))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.orange.opacity(0.25), lineWidth: 1))
        .clipShape(RoundedRectangle(cornerRadius: 10))
    }

    // ── 연결 설정 ─────────────────────────────────────────────────

    private var connectionSection: some View {
        FormSection(title: settings.mode == .host ? "Host 설정" : "Client 설정",
                    icon: settings.mode == .host ? "desktopcomputer" : "laptopcomputer") {
            VStack(spacing: 0) {
                if settings.mode == .host {
                    FormRow(label: "내 IP 주소") {
                        VStack(alignment: .trailing, spacing: 2) {
                            if manager.localIPs.isEmpty {
                                Text("없음").foregroundStyle(.secondary)
                                    .font(.system(.body, design: .monospaced))
                            } else {
                                ForEach(manager.localIPs, id: \.self) { ip in
                                    Text(ip).font(.system(.body, design: .monospaced))
                                        .textSelection(.enabled)
                                }
                            }
                        }
                    }
                    Divider().padding(.leading, 16)
                    VStack(alignment: .leading, spacing: 10) {
                        HStack {
                            Text("모니터 배치")
                                .font(.system(size: 13))
                            Spacer()
                            Text("상대 Mac 위치를 클릭하세요")
                                .font(.system(size: 10))
                                .foregroundStyle(.secondary)
                        }
                        MonitorLayoutView(edge: $settings.clientEdge)
                            .frame(maxWidth: .infinity)
                            .onChange(of: settings.clientEdge) { _ in restartIfRunning() }
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 12)
                } else {
                    FormRow(label: "Host IP") {
                        TextField("예: 192.168.0.10", text: $settings.hostIP)
                            .textFieldStyle(.roundedBorder)
                            .frame(width: 200)
                            .onSubmit { restartIfRunning() }
                    }
                }
            }
        }
    }

    // ── 네트워크 ──────────────────────────────────────────────────

    private var networkSection: some View {
        FormSection(title: "네트워크", icon: "network") {
            VStack(spacing: 0) {
                FormRow(label: "포트") {
                    VStack(alignment: .trailing, spacing: 4) {
                        HStack(spacing: 8) {
                            TextField("24900", text: $portString)
                                .textFieldStyle(.roundedBorder)
                                .frame(width: 80)
                                .onSubmit { commitPort() }
                            Text("기본값 24900")
                                .foregroundStyle(.secondary)
                                .font(.caption)
                        }
                        if let err = portError {
                            Text(err).font(.system(size: 10)).foregroundStyle(.red)
                        }
                    }
                }
                if settings.mode == .client {
                    Divider().padding(.leading, 16)
                    FormRow(label: "연결 확인 간격") {
                        VStack(alignment: .trailing, spacing: 4) {
                            Picker("", selection: $settings.reconnectInterval) {
                                Text("5초").tag(5)
                                Text("10초").tag(10)
                                Text("30초").tag(30)
                            }
                            .pickerStyle(.segmented)
                            .frame(width: 180)
                            .labelsHidden()
                            .onChange(of: settings.reconnectInterval) { _ in restartIfRunning() }
                            Text("Host 와의 연결 유지를 위한 PING 주기")
                                .font(.system(size: 10))
                                .foregroundStyle(.secondary)
                        }
                    }
                }
            }
        }
    }

    // ── 보안 ──────────────────────────────────────────────────────

    private var securitySection: some View {
        FormSection(title: "보안", icon: "lock") {
            FormRow(label: "공유 암호") {
                VStack(alignment: .trailing, spacing: 4) {
                    SecureField("비어있으면 인증 없음", text: $settings.psk)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 200)
                        .onSubmit { restartIfRunning() }
                    Text("양쪽 Mac 에 동일한 값을 입력하세요")
                        .font(.system(size: 10))
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    // ── 일반 ──────────────────────────────────────────────────────

    private var generalSection: some View {
        FormSection(title: "일반", icon: "slider.horizontal.3") {
            VStack(spacing: 0) {
                FormToggleRow(
                    label: "로그인 시 자동 실행",
                    description: "Mac 에 로그인할 때 bconn 을 자동으로 실행합니다",
                    isOn: $settings.launchAtLogin
                )
                .onChange(of: settings.launchAtLogin) { LaunchAtLogin.setEnabled($0) }

                Divider().padding(.leading, 16)

                FormToggleRow(
                    label: "앱 실행 시 자동 연결",
                    description: "앱이 시작되면 저장된 설정으로 세션을 바로 시작합니다",
                    isOn: $settings.autoStart
                )
            }
        }
    }

    // ── 단축키 ────────────────────────────────────────────────────

    private var shortcutSection: some View {
        FormSection(title: "단축키 (Host 모드)", icon: "keyboard") {
            VStack(spacing: 0) {
                ShortcutRow(
                    keys: ["⌘", "Esc"],
                    label: "로컬 강제 복귀",
                    description: "원격 모드에서 호스트로 즉시 복귀합니다"
                )
                Divider().padding(.leading, 16)
                ShortcutRow(
                    keys: ["⌘", "⇧", "Esc"],
                    label: "세션 강제 중지",
                    description: "완전히 먹통이 됐을 때 세션 전체를 종료합니다"
                )
            }
        }
    }

    // ── 헬퍼 ──────────────────────────────────────────────────────

    private var canStart: Bool {
        if settings.mode == .host { return manager.hasAccessibilityPermission }
        return !settings.hostIP.trimmingCharacters(in: .whitespaces).isEmpty
    }

    private func restartIfRunning() {
        guard manager.isRunning else { return }
        manager.restart()
    }

    private func commitPort() {
        let trimmed = portString.trimmingCharacters(in: .whitespaces)
        guard let value = Int(trimmed), (1...65535).contains(value) else {
            portError = "1~65535 범위의 숫자를 입력하세요"
            portString = String(settings.port)
            return
        }
        portError = nil
        if value != settings.port {
            settings.port = value
            restartIfRunning()
        }
    }
}

// ── 섹션 컨테이너 ───────────────────────────────────────────────────

private struct FormSection<Content: View>: View {
    let title: String
    var icon: String? = nil
    @ViewBuilder let content: () -> Content

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 5) {
                if let icon {
                    Image(systemName: icon)
                        .font(.system(size: 10, weight: .semibold))
                }
                Text(title)
                    .font(.system(size: 11, weight: .semibold))
                    .textCase(.uppercase)
                    .tracking(0.5)
            }
            .foregroundStyle(.secondary)
            .padding(.leading, 4)

            content()
                .background(.background)
                .clipShape(RoundedRectangle(cornerRadius: 10))
                .overlay(
                    RoundedRectangle(cornerRadius: 10)
                        .stroke(Color.primary.opacity(0.08), lineWidth: 1)
                )
        }
        .frame(maxWidth: .infinity)
    }
}

// ── 레이블 + 컨텐츠 행 ─────────────────────────────────────────────

private struct FormRow<Content: View>: View {
    let label: String
    @ViewBuilder let content: () -> Content

    var body: some View {
        HStack(alignment: .center) {
            Text(label)
                .font(.system(size: 13))
            Spacer(minLength: 16)
            content()
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }
}

// ── 단축키 행 ──────────────────────────────────────────────────────

private struct ShortcutRow: View {
    let keys: [String]
    let label: String
    let description: String

    var body: some View {
        HStack(alignment: .center) {
            VStack(alignment: .leading, spacing: 2) {
                Text(label).font(.system(size: 13))
                Text(description)
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
            }
            Spacer(minLength: 16)
            HStack(spacing: 3) {
                ForEach(Array(keys.enumerated()), id: \.offset) { _, key in
                    Text(key)
                        .font(.system(size: 12, weight: .medium, design: .monospaced))
                        .padding(.horizontal, 6)
                        .padding(.vertical, 3)
                        .background(Color.primary.opacity(0.07))
                        .clipShape(RoundedRectangle(cornerRadius: 5))
                        .overlay(RoundedRectangle(cornerRadius: 5)
                            .stroke(Color.primary.opacity(0.15), lineWidth: 1))
                }
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }
}

// ── 토글 행 ────────────────────────────────────────────────────────

private struct FormToggleRow: View {
    let label: String
    let description: String
    @Binding var isOn: Bool

    var body: some View {
        HStack(alignment: .center) {
            VStack(alignment: .leading, spacing: 2) {
                Text(label).font(.system(size: 13))
                Text(description)
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 16)
            Toggle("", isOn: $isOn)
                .labelsHidden()
                .toggleStyle(.switch)
                .controlSize(.small)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }
}
