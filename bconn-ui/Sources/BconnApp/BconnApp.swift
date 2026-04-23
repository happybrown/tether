import SwiftUI

@main
struct BconnApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

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
        MenuBarExtra("bconn", systemImage: "display.2") {
            MenuBarView()
        }
        .menuBarExtraStyle(.window)
    }
}
