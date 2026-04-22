import SwiftUI

struct SettingsView: View {
    @ObservedObject private var settings = AppSettings.shared
    @ObservedObject private var manager  = BconnManager.shared

    var body: some View {
        Form {
            // ── 모드 선택 ──────────────────────────────────────────
            Section("모드") {
                Picker("실행 모드", selection: $settings.mode) {
                    ForEach(AppMode.allCases) { mode in
                        Text(mode.label).tag(mode)
                    }
                }
                .pickerStyle(.segmented)
                .onChange(of: settings.mode) { _ in restartIfRunning() }
            }

            // ── Host 설정 ──────────────────────────────────────────
            if settings.mode == .host {
                Section("Host 설정") {
                    LabeledContent("내 IP 주소") {
                        VStack(alignment: .trailing, spacing: 2) {
                            ForEach(manager.localIPs, id: \.self) { ip in
                                Text(ip)
                                    .font(.system(.body, design: .monospaced))
                                    .textSelection(.enabled)
                            }
                        }
                    }

                    LabeledContent("Client 위치") {
                        Picker("", selection: $settings.clientEdge) {
                            ForEach(ClientEdge.allCases) { edge in
                                Text(edge.label).tag(edge)
                            }
                        }
                        .pickerStyle(.segmented)
                        .frame(width: 240)
                        .onChange(of: settings.clientEdge) { _ in restartIfRunning() }
                    }
                }
            }

            // ── Client 설정 ────────────────────────────────────────
            if settings.mode == .client {
                Section("Client 설정") {
                    LabeledContent("Host IP") {
                        TextField("예: 192.168.0.10", text: $settings.hostIP)
                            .textFieldStyle(.roundedBorder)
                            .frame(width: 200)
                            .onSubmit { restartIfRunning() }
                    }
                }
            }

            // ── 공통 네트워크 설정 ─────────────────────────────────
            Section("네트워크") {
                LabeledContent("포트") {
                    HStack {
                        TextField("24900", value: $settings.port, format: .number)
                            .textFieldStyle(.roundedBorder)
                            .frame(width: 80)
                            .onSubmit { restartIfRunning() }
                        Text("기본값: 24900")
                            .foregroundStyle(.secondary)
                            .font(.caption)
                    }
                }
            }

            // ── 기타 ───────────────────────────────────────────────
            Section("기타") {
                Toggle("로그인 시 자동 시작", isOn: $settings.launchAtLogin)
                    .onChange(of: settings.launchAtLogin) { enabled in
                        LaunchAtLogin.setEnabled(enabled)
                    }
            }
        }
        .formStyle(.grouped)
        .padding()
        .frame(width: 480)
        .onAppear { manager.refreshLocalIPs() }
    }

    private func restartIfRunning() {
        guard manager.isRunning else { return }
        manager.stop()
        manager.start()
    }
}
