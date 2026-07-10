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
        // 상태에 따라 아이콘 모양·색이 바뀌어 연결/원격/에러 여부를 메뉴바만 봐도 안다.
        // 연결/원격 등에서는 컬러 틴트로 강조하고, 대기·중지 시엔 기본 단색을 쓴다.
        // label 에는 색 제어를 위해 단일 Image 만 넣는다 (복잡한 커스텀 뷰는 .window
        // 팝오버를 깨뜨릴 수 있어 지양).
        MenuBarExtra {
            MenuBarView(settingsTab: $settingsTab)
        } label: {
            let icon = Image(systemName: manager.menuBarSymbol)
            if let tint = manager.menuBarTint {
                icon.foregroundStyle(tint)
            } else {
                icon
            }
        }
        .menuBarExtraStyle(.window)
    }
}
