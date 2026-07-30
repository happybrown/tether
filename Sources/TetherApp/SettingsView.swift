import SwiftUI

/// 설정 창의 탭. 로그 뷰도 여기에 통합되어 하나의 창으로 동작한다.
/// 사이드바 항목의 아이콘·라벨도 여기서 단일 소스로 관리한다.
///
/// 분류 기준:
/// - 연결: 세션을 맺는 데 필요한 모든 것 (역할·IP·포트·공유 암호). "적용" 바를 공유하는
///   IP/Port/PSK 를 한 곳에 모아 흩어진 저장 동작을 없앤다.
/// - 제어: 연결 이후의 커서·입력 동작 (모니터 배치, 복귀 예측, 단축키). host 전용.
/// - 진단: 네트워크 성능 측정. (연결 탭의 "포트" 와 혼동되던 옛 "네트워크" 탭을 개명)
enum SettingsTab: Hashable, CaseIterable, Identifiable {
    case connection, control, diagnostics, general, logs

    var id: Self { self }

    var label: String {
        switch self {
        case .connection:  return "연결"
        case .control:     return "제어"
        case .diagnostics: return "진단"
        case .general:     return "일반"
        case .logs:        return "로그"
        }
    }

    var icon: String {
        switch self {
        case .connection:  return "network"
        case .control:     return "cursorarrow.motionlines"
        case .diagnostics: return "speedometer"
        case .general:     return "slider.horizontal.3"
        case .logs:        return "doc.text.magnifyingglass"
        }
    }

    /// 사이드바 아이콘 배경 틴트 (시각적 구분).
    var tint: Color {
        switch self {
        case .connection:  return .blue
        case .control:     return .indigo
        case .diagnostics: return .orange
        case .general:     return .gray
        case .logs:        return .purple
        }
    }

    /// 모드별 표시 여부. 제어 탭의 항목(배치·복귀 예측·단축키)은 전부 host 전용이라
    /// client 모드에서는 탭 자체를 숨긴다.
    static func visibleTabs(for mode: AppMode) -> [SettingsTab] {
        allCases.filter { $0 != .control || mode == .host }
    }
}

struct SettingsView: View {
    @ObservedObject private var settings = AppSettings.shared
    @ObservedObject private var manager  = TetherManager.shared

    /// 외부(메뉴바)에서 특정 탭을 선택해 열 수 있도록 바인딩으로 받는다.
    @Binding var selectedTab: SettingsTab

    // 연결 설정(IP/Port/PSK)은 편집 중 즉시 반영하지 않고 로컬 State 에 담아둔다.
    // "적용" 을 눌러야 한 번에 저장 + (실행 중이면) 재시작 1회 → 부분 반영으로 인한
    // 중간 상태 재연결(연결 끊김)을 방지한다.
    @State private var portString: String = ""
    @State private var portError: String?
    @State private var hostIPString: String = ""
    @State private var hostIPError: String?
    @State private var pskString: String = ""

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
            syncEditFields()
            manager.refreshLocalIPs()
            manager.refreshDisplays()
            manager.refreshAccessibilityPermission()
        }
        .onChange(of: settings.port) { newValue in
            if portString != String(newValue) { portString = String(newValue) }
        }
        .onChange(of: settings.hostIP) { newValue in
            if hostIPString != newValue { hostIPString = newValue }
        }
        .onChange(of: settings.psk) { newValue in
            if pskString != newValue { pskString = newValue }
        }
        // client 모드로 바뀌면 host 전용 탭(제어)이 사라지므로 연결 탭으로 되돌린다.
        .onChange(of: settings.mode) { newMode in
            if !SettingsTab.visibleTabs(for: newMode).contains(selectedTab) {
                selectedTab = .connection
            }
        }
    }

    /// 편집 State 를 저장값과 동기화한다 (창 열릴 때 / 적용 후).
    private func syncEditFields() {
        portString = String(settings.port)
        hostIPString = settings.hostIP
        pskString = settings.psk
        portError = nil
        hostIPError = nil
    }

    // ── 사이드바 ──────────────────────────────────────────────────

    private var sidebar: some View {
        List(selection: $selectedTab) {
            ForEach(SettingsTab.visibleTabs(for: settings.mode)) { tab in
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
        case .connection:  connectionTab
        case .control:     controlTab
        case .diagnostics: NetworkTestView()
        case .general:     generalTab
        case .logs:        LogView()
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
    // 세션을 맺는 데 필요한 모든 설정: 역할·권한·연결 대상·포트·공유 암호.
    // "적용" 바를 공유하는 IP/Port/PSK 가 모두 이 탭에 있다.

    private var connectionTab: some View {
        tabScroll {
            modeSection
            if !manager.hasAccessibilityPermission {
                permissionSection
            }
            connectionSection
            networkSection
            securitySection
            connectionApplyBar
        }
    }

    // ── 탭: 제어 (host 전용) ──────────────────────────────────────
    // 연결 이후의 커서·입력 동작: 모니터 배치, 복귀 예측, 단축키.

    private var controlTab: some View {
        tabScroll {
            layoutSection
            behaviorSection
            shortcutSection
        }
    }

    // ── 탭: 일반 ──────────────────────────────────────────────────
    // 자동 실행 등 앱 수준 옵션과 정보.

    private var generalTab: some View {
        tabScroll {
            generalSection
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
            } else {
                FormRow(label: "Host IP") {
                    VStack(alignment: .trailing, spacing: 4) {
                        TextField("예: 192.168.0.10", text: $hostIPString)
                            .textFieldStyle(.roundedBorder)
                            .frame(width: 200)
                            .onSubmit { applyConnectionSettings() }
                        if let err = hostIPError {
                            Text(err).font(.system(size: 10)).foregroundStyle(.red)
                        }
                    }
                }
            }
        }
    }

    // ── 모니터 배치 (제어 탭) ──────────────────────────────────────

    private var layoutSection: some View {
        FormSection(title: "모니터 배치", icon: "display.2") {
            VStack(alignment: .leading, spacing: 10) {
                // host 의 모든 디스플레이를 실제 배치대로 그리고, 상대 Mac 을 원하는
                // 화면의 변에 붙여 기준 화면·방향·오프셋을 한 번에 지정한다.
                MonitorLayoutView(
                    edge: $settings.clientEdge,
                    offset: $settings.clientOffset,
                    triggerUUID: $settings.triggerDisplayUUID,
                    displays: manager.displays,
                    clientResolution: manager.clientScreenSize
                )
                .frame(maxWidth: .infinity)
                .onChange(of: settings.clientEdge) { manager.updateClientEdge($0) }
                .onChange(of: settings.clientOffset) { manager.updateClientOffset($0) }
                .onChange(of: settings.triggerDisplayUUID) { manager.updateTriggerDisplay($0) }

                HStack(spacing: 8) {
                    Text("전환 기준")
                        .font(.system(size: 10))
                        .foregroundStyle(.secondary)
                    Text("\(triggerDisplayLabel) \(settings.clientEdge.label)")
                        .font(.system(size: 11, weight: .medium))
                    Text(String(format: "오프셋 %+d%%", Int((settings.clientOffset * 100).rounded())))
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(.secondary)
                    Spacer()
                    Button("초기화") {
                        settings.clientOffset = 0
                        settings.triggerDisplayUUID = ""
                    }
                    .buttonStyle(.link)
                    .font(.system(size: 10))
                    .disabled(settings.clientOffset == 0 && settings.triggerDisplayUUID.isEmpty)
                }
                Text(manager.displays.count > 1
                     ? "상대 Mac 사각형을 원하는 화면의 변에 끌어다 붙이세요. 그 화면의 그 방향에서만 전환됩니다."
                     : "상대 Mac 사각형을 끌어 배치하세요. 위아래(또는 좌우)로 어긋나면 경계 방향으로 끌어 보정합니다.")
                    .font(.system(size: 10))
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
        }
    }

    // ── 커서 동작 (제어 탭) ────────────────────────────────────────

    private var behaviorSection: some View {
        FormSection(title: "커서 동작", icon: "cursorarrow") {
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
        }
    }

    // ── 연결 설정 적용 바 (IP/Port/PSK 공통) ─────────────────────────
    // 미저장 변경이 있을 때만 나타나, 눌러야 한 번에 저장 + 재시작 1회.
    // 연결 탭(IP·Port)과 보안 탭(PSK) 하단에 공통으로 배치한다.

    /// 현재 전환 기준 화면의 표시 이름. 지정 없으면 자동 모드임을 알린다.
    private var triggerDisplayLabel: String {
        if settings.triggerDisplayUUID.isEmpty { return "전체 화면 바깥쪽" }
        return manager.displays.first { $0.uuid == settings.triggerDisplayUUID }?.label
            ?? "연결 안 된 화면"
    }

    @ViewBuilder
    private var connectionApplyBar: some View {
        if connectionDirty {
            HStack(spacing: 8) {
                Image(systemName: "pencil.circle.fill")
                    .foregroundStyle(.orange)
                    .font(.system(size: 12))
                Text(manager.isRunning
                     ? "미저장 변경 — 적용 시 재연결됩니다"
                     : "미저장 변경")
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(.secondary)
                Spacer()
                Button("되돌리기") { syncEditFields() }
                    .controlSize(.small)
                Button("적용") { applyConnectionSettings() }
                    .controlSize(.small)
                    .buttonStyle(.borderedProminent)
                    .keyboardShortcut(.return, modifiers: [.command])
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .background(Color.orange.opacity(0.08))
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.orange.opacity(0.25), lineWidth: 1))
            .clipShape(RoundedRectangle(cornerRadius: 10))
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
                                .onSubmit { applyConnectionSettings() }
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
                    SecureField("비어있으면 인증 없음", text: $pskString)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 200)
                        .onSubmit { applyConnectionSettings() }
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
                    description: "Mac 에 로그인할 때 tether 을 자동으로 실행합니다",
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
        FormSection(title: "단축키", icon: "keyboard") {
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

    /// 편집값(IP/Port/PSK)이 저장값과 달라 적용이 필요한 상태인지.
    /// trim 후 비교해 공백만 다른 경우는 dirty 로 치지 않는다.
    private var connectionDirty: Bool {
        if pskString != settings.psk { return true }
        if portString.trimmingCharacters(in: .whitespaces) != String(settings.port) { return true }
        if settings.mode == .client,
           hostIPString.trimmingCharacters(in: .whitespaces) != settings.hostIP { return true }
        return false
    }

    /// IP/Port/PSK 편집값을 한 번에 검증·저장하고, 실행 중이면 재시작을 정확히 1회 수행한다.
    /// 개별 필드가 즉시 반영되며 중간 상태로 재연결되던 문제(연결 끊김)를 막는다.
    private func applyConnectionSettings() {
        // 1) Port 검증
        let portTrim = portString.trimmingCharacters(in: .whitespaces)
        guard let portValue = Int(portTrim), (1...65535).contains(portValue) else {
            portError = "1~65535 범위의 숫자를 입력하세요"
            return
        }
        portError = nil

        // 2) Host IP 검증 (client 모드). 빈 값이나 잘못된 형식은 적용을 거부한다.
        //    빈 IP 로 저장하면 host 없이 재시작돼 연결이 끊기므로(예: IP 를 지우다
        //    되돌리기 시 onSubmit 이 빈 값을 커밋) 여기서 막는다.
        let ipTrim = hostIPString.trimmingCharacters(in: .whitespaces)
        if settings.mode == .client {
            if ipTrim.isEmpty {
                hostIPError = "Host IP 를 입력하세요"
                return
            }
            if !Self.isValidIPv4(ipTrim) {
                hostIPError = "올바른 IPv4 주소를 입력하세요 (예: 192.168.0.10)"
                return
            }
        }
        hostIPError = nil

        // 3) 검증 통과 → 저장값과 다른 것만 반영. 저장은 값 변경 시 즉시 이뤄지므로
        //    먼저 모두 저장한 뒤, 실행 중이면 재시작을 마지막에 한 번만 호출한다.
        var changed = false
        if portValue != settings.port { settings.port = portValue; changed = true }
        if pskString != settings.psk { settings.psk = pskString; changed = true }
        if settings.mode == .client, ipTrim != settings.hostIP {
            settings.hostIP = ipTrim; changed = true
        }
        // 편집 필드를 정규화된 값으로 되돌린다 (예: 앞뒤 공백 제거).
        portString = String(settings.port)
        hostIPString = settings.hostIP

        if changed { restartIfRunning() }
    }

    /// IPv4 점4분 표기 엄격 검증: 옥텟 4개, 각 0~255, 선행 0 없는 순수 숫자.
    static func isValidIPv4(_ s: String) -> Bool {
        let parts = s.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4 else { return false }
        for p in parts {
            guard !p.isEmpty, p.count <= 3, p.allSatisfy(\.isNumber),
                  let n = Int(p), (0...255).contains(n) else { return false }
            // "01", "007" 같은 선행 0 표기 거부 (한 자리 "0"은 허용).
            if p.count > 1 && p.first == "0" { return false }
        }
        return true
    }
}
