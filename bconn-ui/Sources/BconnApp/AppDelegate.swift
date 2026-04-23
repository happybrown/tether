import AppKit
import BconnCore

class AppDelegate: NSObject, NSApplicationDelegate {

    func applicationDidFinishLaunching(_ notification: Notification) {
        // 로그 콜백을 먼저 등록한 뒤 세션을 시작해야 초기 로그를 놓치지 않는다
        _ = BconnLogger.shared

        let s = AppSettings.shared
        guard s.autoStart else { return }
        if s.mode == .host || !s.hostIP.isEmpty {
            BconnManager.shared.start()
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        // LSUIElement 앱은 창이 없어도 살아있어야 한다.
        // Window scene이 닫히거나 아직 열리지 않은 상태에서도 종료되지 않도록 한다.
        return false
    }

    func applicationWillTerminate(_ notification: Notification) {
        BconnManager.shared.stop()
    }
}

extension Notification.Name {
    static let bconnStatusChanged = Notification.Name("bconnStatusChanged")
}
