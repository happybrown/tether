import AppKit

// NSApplicationMain 대신 수동으로 앱 진입점 구성
// AppDelegate는 @MainActor이므로 NSApplication.shared.delegate에 직접 설정
final class Application: NSApplication {
    private let appDelegate = AppDelegate()

    override init() {
        super.init()
        self.delegate = appDelegate
    }

    required init?(coder: NSCoder) { fatalError() }
}

_ = Application.shared
Application.shared.run()
