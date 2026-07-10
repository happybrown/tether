import SwiftUI

@main
struct BconnApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @ObservedObject private var manager = BconnManager.shared

    // 설정 창의 선택 탭. 메뉴바에서 "로그..." 를 누르면 로그 탭으로 열리도록 공유한다.
    @State private var settingsTab: SettingsTab = .connection

    var body: some Scene {
        // ── 설정 창 (로그 탭 포함) ────────────────────────────────
        Window("bconn 설정", id: "settings") {
            SettingsView(selectedTab: $settingsTab)
        }
        .windowResizability(.contentMinSize)
        .defaultPosition(.center)
        .commandsRemoved()

        // ── 메뉴바 ───────────────────────────────────────────────
        // 상태에 따라 아이콘이 바뀌므로 연결/원격/에러 여부를 메뉴바만 봐도 알 수 있다.
        // systemImage 표준 이니셜라이저 + .window 스타일 조합이 커스텀 팝오버를
        // 안정적으로 렌더링한다. (label 에 커스텀 뷰를 넣으면 팝오버가 깨질 수 있어 지양)
        MenuBarExtra("bconn", systemImage: manager.menuBarSymbol) {
            MenuBarView(settingsTab: $settingsTab)
        }
        .menuBarExtraStyle(.window)
    }
}
