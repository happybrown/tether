import SwiftUI

/// 설정 창의 탭. 로그 뷰도 여기에 통합되어 하나의 창으로 동작한다.
/// 사이드바 항목의 아이콘·라벨도 여기서 단일 소스로 관리한다.
enum SettingsTab: Hashable, CaseIterable, Identifiable {
    case connection, security, network, general, logs

    var id: Self { self }

    var label: String {
        switch self {
        case .connection: return "연결"
        case .security:   return "보안"
        case .network:    return "네트워크"
        case .general:    return "일반"
        case .logs:       return "로그"
        }
    }

    var icon: String {
        switch self {
        case .connection: return "network"
        case .security:   return "lock"
        case .network:    return "speedometer"
        case .general:    return "slider.horizontal.3"
        case .logs:       return "doc.text.magnifyingglass"
        }
    }

    /// 사이드바 아이콘 배경 틴트 (시각적 구분).
    var tint: Color {
        switch self {
        case .connection: return .blue
        case .security:   return .green
        case .network:    return .orange
        case .general:    return .gray
        case .logs:       return .purple
        }
    }
}

struct SettingsView: View {
    @ObservedObject private var settings = AppSettings.shared
    @ObservedObject private var manager  = BconnManager.shared

    /// 외부(메뉴바)에서 특정 탭을 선택해 열 수 있도록 바인딩으로 받는다.
    @Binding var selectedTab: SettingsTab

    @State private var portString: String = ""
    @State private var portError: String?

    init(selectedTab: Binding<SettingsTab> = .constant(.connection)) {
        _selectedTab = selectedTab
    }

    var body: some View {
        NavigationSplitView {
            sidebar
                .navigationSplitViewColumnWidth(184)
        } detail: {
            VStack(spacing: 0) {
                detailContent
                // 로그 탭은 자체 상태바를 가지므로 시작/중지 하단바를 숨긴다.
                if selectedTab != .logs {
                    sessionBar
                }
            }
            .frame(minWidth: 440)
            .navigationTitle(selectedTab.label)
        }
        .frame(minWidth: 660, minHeight: 560)
        .onAppear {
            portString = String(settings.port)
            manager.refreshLocalIPs()
            manager.refreshAccessibilityPermission()
        }
        .onChange(of: settings.port) { newValue in
            if portString != String(newValue) { portString = String(newValue) }
        }
    }

    // ── 사이드바 ──────────────────────────────────────────────────

    private var sidebar: some View {
        List(selection: $selectedTab) {
            ForEach(SettingsTab.allCases) { tab in
                Label {
                    Text(tab.label)
                        .font(.system(size: 13))
                } icon: {
                    // 시스템 설정 스타일의 둥근 컬러 아이콘 칩.
                    Image(systemName: tab.icon)
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(.white)
                        .frame(width: 20, height: 20)
                        .background(tab.tint.gradient, in: RoundedRectangle(cornerRadius: 5))
                }
                .tag(tab)
                .padding(.vertical, 2)
            }
        }
        .listStyle(.sidebar)
    }

    // ── 선택 탭 콘텐츠 ────────────────────────────────────────────

    @ViewBuilder
    private var detailContent: some View {
        switch selectedTab {
        case .connection: connectionTab
        case .security:   securityTab
        case .network:    NetworkTestView()
        case .general:    generalTab
        case .logs:       LogView()
        }
    }

    // ── 하단 세션 상태바 + 시작/중지 버튼 ──────────────────────────

    private var sessionBar: some View {
        VStack(spacing: 0) {
            Divider()
            HStack(spacing: 10) {
                // 상태 pill — 아이콘+라벨을 상태색 배경에 담아 한눈에 구분.
                HStack(spacing: 6) {
                    Image(systemName: manager.statusHeaderSymbol)
                        .font(.system(size: 11, weight: .semibold))
                    Text(manager.statusLabel)
                        .font(.system(size: 12, weight: .semibold))
                }
                .foregroundStyle(manager.statusColor)
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .background(manager.statusColor.opacity(0.12),
                            in: Capsule())

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
            .padding(.horizontal, 20)
            .padding(.vertical, 12)
            .background(.bar)
        }
    }

    // ── 탭: 연결 ──────────────────────────────────────────────────
    // 모드·권한·연결 대상·네트워크 등 세션을 맺는 데 필요한 설정을 모은다.

    private var connectionTab: some View {
        tabScroll {
            modeSection
            if !manager.hasAccessibilityPermission {
                permissionSection
            }
            connectionSection
            networkSection
        }
    }

    // ── 탭: 보안 ──────────────────────────────────────────────────

    private var securityTab: some View {
        tabScroll {
            securitySection
        }
    }

    // ── 탭: 일반 ──────────────────────────────────────────────────
    // 자동 실행 등 일반 옵션과 host 모드 단축키 안내.

    private var generalTab: some View {
        tabScroll {
            generalSection
            if settings.mode == .host {
                shortcutSection
            }
            aboutSection
        }
    }

    /// 각 탭 공통 스크롤 컨테이너. 탭이 길어져도 창 높이를 넘지 않도록 스크롤한다.
    private func tabScroll<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        ScrollView {
            VStack(spacing: 18) { content() }
                .padding(.horizontal, 22)
                .padding(.vertical, 20)
                .frame(maxWidth: .infinity, alignment: .top)
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
                Text(settings.mode == .host
                     ? "Host 모드는 키보드·마우스 이벤트를 가로채기 위해 시스템 권한이 필요합니다."
                     : "Client 모드는 원격 클릭·키 입력을 주입하기 위해 시스템 권한이 필요합니다.")
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
                            .onChange(of: settings.clientEdge) { manager.updateClientEdge($0) }
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 12)
                    Divider().padding(.leading, 16)
                    FormRow(label: "원격 복귀 예측") {
                        VStack(alignment: .trailing, spacing: 4) {
                            Toggle("", isOn: $settings.predictLeave)
                                .labelsHidden()
                                .toggleStyle(.switch)
                                .onChange(of: settings.predictLeave) { manager.updatePredictLeave($0) }
                            Text("반대편 경계 이탈을 Host 가 직접 예측해 지연 없이 복귀합니다.\n끄면 Client 신호로만 복귀합니다(약간 느리지만 오탐 없음).")
                                .font(.system(size: 10))
                                .foregroundStyle(.secondary)
                                .multilineTextAlignment(.trailing)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
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

    // ── 앱 정보 ────────────────────────────────────────────────────

    private var aboutSection: some View {
        FormSection(title: "정보", icon: "info.circle") {
            FormRow(label: "버전") {
                Text(manager.appVersion)
                    .font(.system(.body, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }
        }
    }

    // ── 헬퍼 ──────────────────────────────────────────────────────

    private var canStart: Bool {
        // 두 모드 모두 손쉬운 사용 권한 필요 (host: 캡처, client: 주입).
        guard manager.hasAccessibilityPermission else { return false }
        if settings.mode == .host { return true }
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
