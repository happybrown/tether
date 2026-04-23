import AppKit
import BconnCore

class AppDelegate: NSObject, NSApplicationDelegate {

    func applicationDidFinishLaunching(_ notification: Notification) {
        let s = AppSettings.shared
        guard s.autoStart else { return }
        // 자동 시작 옵션이 켜져 있을 때만 실행
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
