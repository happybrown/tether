import AppKit

/// 설정 창(Window scene id: "settings")을 AppKit 레벨에서 제어하는 헬퍼.
/// SwiftUI 의 openWindow 만으로는 (1) 전체화면(별도 Space)에서 창을 현재 화면으로
/// 끌어오지 못하고, (2) 여러 모니터 중 사용자가 보던 화면에 배치하지 못한다.
/// 그래서 NSWindow 를 직접 찾아 Space·화면·위치를 보정한다.
enum SettingsWindow {
    static let id = "settings"

    /// 설정 창 NSWindow 를 찾는다. SwiftUI Window 는 identifier 에 scene id 를 넣는다.
    private static func window() -> NSWindow? {
        NSApp.windows.first { $0.identifier?.rawValue == id }
    }

    /// 열려 있으면 닫는다. 시작 시 자동 복원된 창을 없애는 용도.
    static func closeIfOpen() {
        window()?.close()
    }

    /// 설정 창을 지정한 화면 중앙에 띄운다. 전체화면 Space 위에서도 보이게 한다.
    ///
    /// 대상 화면(screen)은 호출부가 "메뉴를 클릭한 순간"에 캡처해 넘긴다. 그 시점의
    /// 마우스 위치가 사용자가 보던 화면을 가장 정확히 가리키기 때문이다. openWindow 로
    /// 창이 만들어지는 데엔 몇 런루프가 걸릴 수 있어, 창이 준비될 때까지 짧게 폴링한다.
    static func present(on screen: NSScreen?) {
        NSApp.activate(ignoringOtherApps: true)
        waitForWindow(retries: 20) { win in
            place(win, on: screen)
        }
    }

    /// 창이 나타나고 최종 크기로 레이아웃될 때까지 다음 런루프마다 재시도한다.
    /// (openWindow 직후엔 NSWindow 가 없거나 frame 이 0 인 순간이 있다.)
    private static func waitForWindow(retries: Int, _ body: @escaping (NSWindow) -> Void) {
        if let win = window(), win.frame.width > 0, win.frame.height > 0 {
            body(win)
            return
        }
        guard retries > 0 else { return }
        DispatchQueue.main.async {
            waitForWindow(retries: retries - 1, body)
        }
    }

    /// 창을 대상 화면 중앙에 놓는다.
    private static func place(_ win: NSWindow, on screen: NSScreen?) {
        // 위치를 먼저 확정한다. moveToActiveSpace 는 쓰지 않는다 — 그건 창을 "시스템이
        // 정한 활성 Space" 로 강제 이동시키며 이 setFrameOrigin 좌표를 무효화해,
        // 마우스가 있는 화면이 아닌 엉뚱한 모니터로 튀는 원인이 된다.
        if let screen {
            let visible = screen.visibleFrame          // 메뉴바·Dock 제외한 전역 좌표
            let size = win.frame.size
            win.setFrameOrigin(NSPoint(
                x: visible.midX - size.width / 2,
                y: visible.midY - size.height / 2
            ))
        }

        // 전체화면 앱의 Space 에서도 그 위로 뜨게 하려면 fullScreenAuxiliary 가 필요하다.
        // 단, 영구히 켜두면 이후 모든 전체화면 앱 위에 늘 겹쳐 떠 방해가 되므로,
        // 창을 앞으로 올리는 이 순간에만 켰다가 안착 후 되돌린다.
        let original = win.collectionBehavior
        win.collectionBehavior.insert(.fullScreenAuxiliary)
        win.makeKeyAndOrderFront(nil)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            win.collectionBehavior = original
        }
    }

    /// 마우스 커서가 있는 화면. 메뉴를 클릭한 순간 호출해 "보던 화면"을 캡처한다.
    static func screenUnderMouse() -> NSScreen? {
        let mouse = NSEvent.mouseLocation
        return NSScreen.screens.first { $0.frame.contains(mouse) } ?? NSScreen.main
    }
}
