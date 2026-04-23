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

    func applicationWillTerminate(_ notification: Notification) {
        BconnManager.shared.stop()
    }
}

extension Notification.Name {
    static let bconnStatusChanged = Notification.Name("bconnStatusChanged")
}
