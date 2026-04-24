import SwiftUI

struct SettingsView: View {
    @ObservedObject private var settings = AppSettings.shared
    @ObservedObject private var manager  = BconnManager.shared

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
                        HStack(spacing: 8) {
                            TextField("24900", value: $settings.port, format: .number)
                                .textFieldStyle(.roundedBorder)
                                .frame(width: 80)
                                .onSubmit { restartIfRunning() }
                            Text("기본값: 24900")
                                .foregroundStyle(.secondary)
                                .font(.caption)
                        }
                    }

                    if settings.mode == .client {
                        Divider()

                        SettingsRow(label: "재연결 간격") {
                            HStack(spacing: 8) {
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
                            }
                        }
                    }
                }
            }

            // ── 보안 ───────────────────────────────────────────────
            SettingsSection(title: "보안") {
                SettingsRow(label: "공유 암호") {
                    SecureField("비어있으면 인증 없음", text: $settings.psk)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 200)
                        .onSubmit { restartIfRunning() }
                }
            }

            // ── 기타 ───────────────────────────────────────────────
            SettingsSection(title: "기타") {
                VStack(spacing: 10) {
                    Toggle("앱 시작 시 자동으로 실행", isOn: $settings.autoStart)

                    Divider()

                    Toggle("로그인 시 자동 시작", isOn: $settings.launchAtLogin)
                        .onChange(of: settings.launchAtLogin) { enabled in
                            LaunchAtLogin.setEnabled(enabled)
                        }
                }
            }

            Spacer(minLength: 0)
        }
        .padding(20)
        .frame(width: 460)
        .onAppear { manager.refreshLocalIPs() }
    }

    private func restartIfRunning() {
        guard manager.isRunning else { return }
        manager.restart()
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
        HStack {
            Text(label)
                .font(.system(size: 13))
                .foregroundStyle(.primary)
            Spacer()
            content()
        }
    }
}
