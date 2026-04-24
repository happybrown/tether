import SwiftUI

struct SettingsView: View {
    @ObservedObject private var settings = AppSettings.shared
    @ObservedObject private var manager  = BconnManager.shared

    @State private var portString: String = ""
    @State private var portError: String?

    var body: some View {
        VStack(spacing: 0) {
            // ── 모드 선택 ──────────────────────────────────────────
            SettingsSection(title: "모드") {
                VStack(spacing: 10) {
                    Picker("", selection: $settings.mode) {
                        ForEach(AppMode.allCases) { mode in
                            Text(mode.label).tag(mode)
                        }
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    .disabled(manager.isRunning)

                    if manager.isRunning {
                        HStack(spacing: 4) {
                            Image(systemName: "lock.fill")
                                .font(.system(size: 10))
                            Text("실행 중에는 모드를 변경할 수 없습니다.")
                                .font(.system(size: 11))
                        }
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .center)
                    }
                }
            }

            // ── 권한 (Host 모드에서만) ─────────────────────────────
            if settings.mode == .host && !manager.hasAccessibilityPermission {
                SettingsSection(title: "권한") {
                    HStack(alignment: .top, spacing: 10) {
                        Image(systemName: "lock.shield.fill")
                            .foregroundStyle(.orange)
                            .font(.system(size: 16))
                        VStack(alignment: .leading, spacing: 4) {
                            Text("손쉬운 사용 권한이 필요합니다")
                                .font(.system(size: 13, weight: .medium))
                            Text("Host 모드는 이 Mac 의 키보드·마우스 이벤트를 가로채기 위해 시스템 권한이 필요합니다.")
                                .font(.system(size: 11))
                                .foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        Spacer(minLength: 0)
                        Button("시스템 설정 열기") {
                            manager.openAccessibilitySettings()
                        }
                        .controlSize(.small)
                    }
                }
            }

            // ── Host 설정 ──────────────────────────────────────────
            if settings.mode == .host {
                SettingsSection(title: "Host 설정") {
                    VStack(spacing: 10) {
                        SettingsRow(label: "내 IP 주소") {
                            VStack(alignment: .trailing, spacing: 2) {
                                if manager.localIPs.isEmpty {
                                    Text("없음")
                                        .foregroundStyle(.secondary)
                                        .font(.system(.body, design: .monospaced))
                                } else {
                                    ForEach(manager.localIPs, id: \.self) { ip in
                                        Text(ip)
                                            .font(.system(.body, design: .monospaced))
                                            .textSelection(.enabled)
                                    }
                                }
                            }
                        }

                        Divider()

                        SettingsRow(label: "Client 위치") {
                            Picker("", selection: $settings.clientEdge) {
                                ForEach(ClientEdge.allCases) { edge in
                                    Text(edge.label).tag(edge)
                                }
                            }
                            .pickerStyle(.segmented)
                            .frame(width: 220)
                            .labelsHidden()
                        }
                    }
                }
            }

            // ── Client 설정 ────────────────────────────────────────
            if settings.mode == .client {
                SettingsSection(title: "Client 설정") {
                    SettingsRow(label: "Host IP") {
                        TextField("예: 192.168.0.10", text: $settings.hostIP)
                            .textFieldStyle(.roundedBorder)
                            .frame(width: 200)
                            .onSubmit { restartIfRunning() }
                    }
                }
            }

            // ── 네트워크 ───────────────────────────────────────────
            SettingsSection(title: "네트워크") {
                VStack(spacing: 10) {
                    SettingsRow(label: "포트") {
                        VStack(alignment: .trailing, spacing: 4) {
                            HStack(spacing: 8) {
                                TextField("24900", text: $portString)
                                    .textFieldStyle(.roundedBorder)
                                    .frame(width: 80)
                                    .onSubmit { commitPort() }
                                Text("기본 24900")
                                    .foregroundStyle(.secondary)
                                    .font(.caption)
                            }
                            if let err = portError {
                                Text(err)
                                    .font(.system(size: 10))
                                    .foregroundStyle(.red)
                            }
                        }
                    }

                    if settings.mode == .client {
                        Divider()

                        SettingsRow(label: "연결 확인 간격") {
                            VStack(alignment: .trailing, spacing: 4) {
                                Picker("", selection: $settings.reconnectInterval) {
                                    Text("5초").tag(5)
                                    Text("10초").tag(10)
                                    Text("30초").tag(30)
                                }
                                .pickerStyle(.segmented)
                                .frame(width: 180)
                                .labelsHidden()
                                .onChange(of: settings.reconnectInterval) { _ in
                                    restartIfRunning()
                                }
                                Text("Host 와의 연결 유지를 위한 PING 주기")
                                    .font(.system(size: 10))
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                }
            }

            // ── 보안 ───────────────────────────────────────────────
            SettingsSection(title: "보안") {
                SettingsRow(label: "공유 암호") {
                    VStack(alignment: .trailing, spacing: 4) {
                        SecureField("비어있으면 인증 없음", text: $settings.psk)
                            .textFieldStyle(.roundedBorder)
                            .frame(width: 200)
                            .onSubmit { restartIfRunning() }
                        Text("양쪽 Mac 에 동일한 값을 입력해야 연결됩니다")
                            .font(.system(size: 10))
                            .foregroundStyle(.secondary)
                    }
                }
            }

            // ── 기타 ───────────────────────────────────────────────
            SettingsSection(title: "기타") {
                VStack(spacing: 10) {
                    DescribedToggle(
                        title: "로그인 시 자동 실행",
                        description: "Mac 에 로그인할 때 bconn 을 자동으로 실행합니다",
                        isOn: $settings.launchAtLogin
                    )
                    .onChange(of: settings.launchAtLogin) { enabled in
                        LaunchAtLogin.setEnabled(enabled)
                    }

                    Divider()

                    DescribedToggle(
                        title: "앱 실행 시 자동으로 연결 시작",
                        description: "앱이 시작되면 저장된 설정으로 세션을 바로 시작합니다",
                        isOn: $settings.autoStart
                    )
                }
            }

            Spacer(minLength: 0)
        }
        .padding(20)
        .frame(width: 480)
        .onAppear {
            portString = String(settings.port)
            manager.refreshLocalIPs()
            manager.refreshAccessibilityPermission()
        }
        .onChange(of: settings.port) { newValue in
            // 외부 변경(예: UserDefaults 복원)과 동기화
            if portString != String(newValue) {
                portString = String(newValue)
            }
        }
    }

    private func restartIfRunning() {
        guard manager.isRunning else { return }
        manager.restart()
    }

    /// 포트 문자열을 파싱·검증 후 실제 설정에 반영한다.
    /// 1~65535 범위가 아니거나 숫자가 아니면 오류 메시지를 표시하고 기존 값을 유지한다.
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
private struct SettingsSection<Content: View>: View {
    let title: String
    @ViewBuilder let content: () -> Content

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(.secondary)
                .textCase(.uppercase)
                .tracking(0.5)

            VStack(spacing: 0) {
                content()
            }
            .padding(12)
            .background(.background.opacity(0.6))
            .overlay(
                RoundedRectangle(cornerRadius: 10)
                    .stroke(Color.primary.opacity(0.08), lineWidth: 1)
            )
            .clipShape(RoundedRectangle(cornerRadius: 10))
        }
        .padding(.bottom, 16)
    }
}

// ── 레이블 + 컨텐츠 행 ─────────────────────────────────────────────
private struct SettingsRow<Content: View>: View {
    let label: String
    @ViewBuilder let content: () -> Content

    var body: some View {
        HStack(alignment: .top) {
            Text(label)
                .font(.system(size: 13))
                .foregroundStyle(.primary)
                .padding(.top, 3)
            Spacer()
            content()
        }
    }
}

/// 제목 + 부가 설명 + 토글 조합 — 기능의 의도를 명확히 전달한다.
private struct DescribedToggle: View {
    let title: String
    let description: String
    @Binding var isOn: Bool

    var body: some View {
        HStack(alignment: .top) {
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.system(size: 13))
                Text(description)
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 12)
            Toggle("", isOn: $isOn)
                .labelsHidden()
                .toggleStyle(.switch)
                .controlSize(.small)
        }
    }
}
