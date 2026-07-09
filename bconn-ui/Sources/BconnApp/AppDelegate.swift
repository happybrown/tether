import AppKit
import ApplicationServices
import BconnCore

class AppDelegate: NSObject, NSApplicationDelegate {

    // SIGTERM/SIGINT/SIGHUP 수신 시 bconn_stop → terminate 경로로 유도한다.
    // Swift runtime 이 기본값(프로세스 즉시 종료)을 쓰면 cleanup 이 생략되므로
    // signal() 로 기본 핸들러를 무시한 뒤 DispatchSource 로 포착한다.
    private var signalSources: [DispatchSourceSignal] = []

    func applicationWillFinishLaunching(_ notification: Notification) {
        // 자기 자신을 PID 로 명시 제외하고 다른 인스턴스만 센다.
        // runningApplications(withBundleIdentifier:) 는 open 실행 시 LaunchServices 가
        // 자기 프로세스를 잠시 이중 계산해 count>1 이 되는 경쟁이 있어, 단순 count 비교로는
        // 유일 인스턴스인데도 자살하는 문제가 있다.
        let bundleID = Bundle.main.bundleIdentifier!
        let myPID = ProcessInfo.processInfo.processIdentifier
        let others = NSRunningApplication.runningApplications(withBundleIdentifier: bundleID)
            .filter { $0.processIdentifier != myPID && !$0.isTerminated }
        if let existing = others.first {
            existing.activate()
            NSApp.terminate(nil)
        }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // 로그 콜백을 먼저 등록한 뒤 세션을 시작해야 초기 로그를 놓치지 않는다
        _ = BconnLogger.shared

        installSignalHandlers()

        let s = AppSettings.shared

        // 최초 실행 시 시스템 권한 prompt 표시 (이후에는 자동으로 안 뜸).
        // Host: CGEventTap 으로 입력을 가로채고, Client: CGEventPost 로 입력을 주입하므로
        // 두 모드 모두 손쉬운 사용 권한이 필요하다. 사용자가 이를 인지하도록 프롬프트를 띄운다.
        let options: [String: Any] = [
            kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true
        ]
        _ = AXIsProcessTrustedWithOptions(options as CFDictionary)
        BconnManager.shared.refreshAccessibilityPermission()

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

    private func installSignalHandlers() {
        for sig in [SIGTERM, SIGINT, SIGHUP] {
            signal(sig, SIG_IGN)
            let src = DispatchSource.makeSignalSource(signal: sig, queue: .main)
            src.setEventHandler {
                MainActor.assumeIsolated {
                    BconnManager.shared.stop()
                    NSApplication.shared.terminate(nil)
                }
            }
            src.resume()
            signalSources.append(src)
        }
    }
}

extension Notification.Name {
    static let bconnStatusChanged = Notification.Name("bconnStatusChanged")
}
