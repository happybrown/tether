import SwiftUI

@main
struct TetherApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @ObservedObject private var manager = TetherManager.shared
    // 언어 변경 시 창 제목 등 Scene 수준 문자열이 다시 그려지도록 관찰한다.
    @ObservedObject private var l10n = L10n.shared

    // 설정 창의 선택 탭. 메뉴바에서 "로그..." 를 누르면 로그 탭으로 열리도록 공유한다.
    @State private var settingsTab: SettingsTab = .connection

    var body: some Scene {
        // ── 설정 창 (로그 탭 포함) ────────────────────────────────
        Window(L("app.settings_title"), id: SettingsWindow.id) {
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
            // 연결 3상태는 모니터 2대 커스텀 아이콘(부분 컬러)으로, 오류·인증실패는
            // 경고성 SF Symbol + 틴트로 표시한다.
            if let asset = manager.menuBarAsset {
                // .original: 메뉴바가 아이콘을 단색 template 으로 강제하지 않게 해
                // 화면의 부분 파랑이 그대로 보이게 한다.
                // 크기는 asset SVG 의 width/height(높이 17pt)로 정해진다 — MenuBarExtra
                // label 은 SwiftUI frame 을 무시하고 이미지 고유 크기를 쓰기 때문이다.
                Image(asset).renderingMode(.original)
            } else {
                let icon = Image(systemName: manager.menuBarSymbol)
                if let tint = manager.menuBarTint {
                    icon.foregroundStyle(tint)
                } else {
                    icon
                }
            }
        }
        .menuBarExtraStyle(.window)
    }
}
