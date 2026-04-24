import SwiftUI

@main
struct BconnApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @ObservedObject private var manager = BconnManager.shared

    var body: some Scene {
        // ── 설정 창 ──────────────────────────────────────────────
        Window("bconn 설정", id: "settings") {
            SettingsView()
        }
        .windowResizability(.contentSize)
        .defaultPosition(.center)
        .commandsRemoved()

        // ── 로그 창 ──────────────────────────────────────────────
        Window("bconn 로그", id: "logs") {
            LogView()
        }
        .windowResizability(.contentMinSize)
        .defaultPosition(.center)
        .commandsRemoved()

        // ── 메뉴바 ───────────────────────────────────────────────
        // 상태에 따라 모니터 화면 색이 바뀌므로 연결/원격/에러 여부를 메뉴바만 봐도 알 수 있다.
        MenuBarExtra {
            MenuBarView()
        } label: {
            Image(nsImage: MenuBarIcon.image(tint: manager.menuBarTint))
                .accessibilityLabel(manager.statusLabel)
        }
        .menuBarExtraStyle(.window)
    }
}
