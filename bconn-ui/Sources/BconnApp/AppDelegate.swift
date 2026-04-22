import AppKit
import BconnCore

class AppDelegate: NSObject, NSApplicationDelegate {

    func applicationDidFinishLaunching(_ notification: Notification) {
        // 설정이 완료된 상태이면 자동 시작
        let s = AppSettings.shared
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
