import Foundation
import AppKit
import ApplicationServices
import BconnCore

@MainActor
class BconnManager: ObservableObject {
    static let shared = BconnManager()

    @Published var status: BconnStatus = .idle
    @Published var statusMessage: String = "중지됨"
    @Published var localIPs: [String] = []
    @Published var isRunning = false
    @Published var connectedHostname: String = ""
    @Published var authFailed: Bool = false
    @Published var hasAccessibilityPermission: Bool = false

    private var permissionTimer: Timer?
    private var clipboardTimer: Timer?
    // 마지막으로 전송한 클립보드 내용 (중복 전송 방지)
    private var lastSentClipboard: String = ""
    // 마지막으로 수신한 클립보드 내용 (루프 방지)
    private var lastReceivedClipboard: String = ""
    // NSPasteboard changeCount 기반 변경 감지 (폴링 효율화)
    private var lastChangeCount: Int = 0

    private init() {
        refreshLocalIPs()
        refreshAccessibilityPermission()
        startPermissionPolling()
        registerClipboardCallback()
        registerForceStopCallback()
    }

    /// 손쉬운 사용 권한 상태를 갱신한다. Host 모드에서 CGEventTap 이 동작하려면 필요.
    func refreshAccessibilityPermission() {
        hasAccessibilityPermission = AXIsProcessTrusted()
    }

    /// 시스템 설정 > 개인 정보 보호 및 보안 > 손쉬운 사용 창을 연다.
    func openAccessibilitySettings() {
        let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")!
        NSWorkspace.shared.open(url)
    }

    /// 사용자가 시스템 설정에서 허용을 체크했는지 5초 간격으로 polling.
    /// 허용되면 자동으로 UI 가 갱신되어 별도 조작이 필요 없다.
    private func startPermissionPolling() {
        permissionTimer = Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refreshAccessibilityPermission() }
        }
    }

    func refreshLocalIPs() {
        var buf = [CChar](repeating: 0, count: 512)
        let count = buf.withUnsafeMutableBufferPointer { ptr in
            bconn_get_local_ips(ptr.baseAddress, Int32(ptr.count))
        }
        guard count > 0 else { localIPs = []; return }
        localIPs = String(cString: buf)
            .split(separator: "\n")
            .map(String.init)
    }

    func start() {
        let settings = AppSettings.shared
        var config = BconnConfig()

        config.is_host     = settings.mode == .host
        config.port        = UInt16(settings.port)
        config.client_edge = BconnEdge(UInt32(settings.clientEdge.rawValue))

        withUnsafeMutableBytes(of: &config.host_ip) { ptr in
            settings.hostIP.withCString { src in
                let dest = ptr.baseAddress!.assumingMemoryBound(to: CChar.self)
                strlcpy(dest, src, 64)
            }
        }

        withUnsafeMutableBytes(of: &config.psk) { ptr in
            settings.psk.withCString { src in
                let dest = ptr.baseAddress!.assumingMemoryBound(to: CChar.self)
                strlcpy(dest, src, 128)
            }
        }

        config.reconnect_interval_s = UInt16(settings.reconnectInterval)

        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        bconn_start(&config, { status, msg, ctx in
            guard let ctx else { return }
            let manager = Unmanaged<BconnManager>.fromOpaque(ctx).takeUnretainedValue()
            let message = msg.map { String(cString: $0) } ?? ""
            DispatchQueue.main.async {
                manager.status = status
                manager.statusMessage = message
                if status == BCONN_STATUS_CONNECTED && !message.isEmpty
                    && message != "연결됨" && message != "재연결 중..." {
                    // host: message에 클라이언트 호스트명이 담겨 있음
                    manager.connectedHostname = message
                    manager.authFailed = false
                } else if status == BCONN_STATUS_AUTH_FAILED {
                    manager.authFailed = true
                    manager.connectedHostname = ""
                }
                NotificationCenter.default.post(name: .bconnStatusChanged, object: nil)
            }
        }, selfPtr)

        isRunning = true
        status = bconn_get_status()
        startClipboardPolling()
    }

    // MARK: - Escape Shortcuts

    /// Cmd+Shift+Esc 감지 시 세션을 강제 중지한다.
    /// CGEventTap RunLoop 스레드에서 호출되므로 Task.detached 로 비동기 처리한다.
    private func registerForceStopCallback() {
        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        bconn_set_force_stop_callback({ ctx in
            guard let ctx else { return }
            let manager = Unmanaged<BconnManager>.fromOpaque(ctx).takeUnretainedValue()
            Task.detached {
                await MainActor.run { manager.stop() }
            }
        }, selfPtr)
    }

    // MARK: - Clipboard Sharing

    /// C++ 쪽에서 클립보드 수신 시 호출될 콜백을 등록한다.
    private func registerClipboardCallback() {
        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        bconn_set_clipboard_callback({ data, len, ctx in
            guard let data, let ctx else { return }
            let manager = Unmanaged<BconnManager>.fromOpaque(ctx).takeUnretainedValue()
            let text = String(bytes: UnsafeBufferPointer(start: data, count: Int(len)),
                              encoding: .utf8) ?? ""
            guard !text.isEmpty else { return }
            DispatchQueue.main.async {
                manager.applyReceivedClipboard(text)
            }
        }, selfPtr)
    }

    private func applyReceivedClipboard(_ text: String) {
        guard text != lastReceivedClipboard else { return }
        lastReceivedClipboard = text
        // 로컬 클립보드에 적용 (이 값을 다시 감지해 전송하지 않도록 lastSentClipboard 도 갱신)
        lastSentClipboard = text
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        // changeCount를 갱신해 다음 폴링에서 자체 변경을 무시
        lastChangeCount = NSPasteboard.general.changeCount
    }

    /// 연결된 동안 클립보드를 0.5초 간격으로 폴링해 변경 시 상대방에 전송한다.
    private func startClipboardPolling() {
        stopClipboardPolling()
        lastSentClipboard = NSPasteboard.general.string(forType: .string) ?? ""
        lastChangeCount = NSPasteboard.general.changeCount
        clipboardTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.pollAndSendClipboard() }
        }
    }

    private func stopClipboardPolling() {
        clipboardTimer?.invalidate()
        clipboardTimer = nil
    }

    private func pollAndSendClipboard() {
        guard isRunning else { return }
        // changeCount가 바뀌지 않았으면 내용 비교 없이 스킵 (효율화)
        let currentCount = NSPasteboard.general.changeCount
        guard currentCount != lastChangeCount else { return }
        lastChangeCount = currentCount

        let current = NSPasteboard.general.string(forType: .string) ?? ""
        guard !current.isEmpty,
              current != lastSentClipboard,
              current != lastReceivedClipboard else { return }

        lastSentClipboard = current
        guard let data = current.data(using: .utf8) else { return }
        data.withUnsafeBytes { ptr in
            if let base = ptr.bindMemory(to: UInt8.self).baseAddress {
                bconn_send_clipboard(base, UInt32(data.count))
            }
        }
    }

    func stop() {
        stopClipboardPolling()
        isRunning = false
        status = .idle
        statusMessage = "중지됨"
        connectedHostname = ""
        authFailed = false
        NotificationCenter.default.post(name: .bconnStatusChanged, object: nil)

        // bconn_stop()은 진입 직후 g_cb=nullptr로 콜백을 해제하므로
        // set_status→DispatchQueue.main.async 데드락은 발생하지 않는다.
        // 동기 호출하여 앱 종료 시 C++ 스레드가 확실히 join되도록 한다.
        bconn_stop()
    }

    /// 재시작 시 stop 과 start 가 race 하지 않도록 완료 대기.
    /// C++ 측 g_lifecycle_mu 가 실제 상호 배제를 수행하지만, 여기서는 main thread 가
    /// 잠시라도 block 되는 걸 피하기 위해 background 에서 순차 실행한다.
    func restart() {
        stopClipboardPolling()
        isRunning = false
        status = .idle
        statusMessage = "재시작 중..."
        connectedHostname = ""
        authFailed = false
        NotificationCenter.default.post(name: .bconnStatusChanged, object: nil)

        Task.detached {
            bconn_stop()
            await MainActor.run {
                BconnManager.shared.start()
            }
        }
    }

    var statusLabel: String {
        switch status {
        case .idle:        return "중지됨"
        case .waiting:     return statusMessage.isEmpty ? "대기 중" : statusMessage
        case .connected:   return "연결됨"
        case .remote:      return "원격 활성"
        case .error:       return statusMessage
        case .authFailed:  return "인증 실패"
        default:           return "알 수 없음"
        }
    }

    var statusColor: Color {
        switch status {
        case .idle:        return .gray
        case .waiting:     return .yellow
        case .connected:   return .green
        case .remote:      return .blue
        case .error:       return .red
        case .authFailed:  return .orange
        default:           return .gray
        }
    }

    /// 메뉴바에 노출될 SF Symbol 이름. 상태를 아이콘 모양으로 구분해 빠른 확인을 돕는다.
    /// 모든 이름은 실제 존재하는 SF Symbol 이어야 한다 (없는 이름은 빈 아이콘으로 렌더됨).
    var menuBarSymbol: String {
        switch status {
        case .idle:        return "display.2"
        case .waiting:     return "display.2"              // 대기: 기본 아이콘
        case .connected:   return "display.2"              // 연결: 기본 아이콘
        case .remote:      return "arrow.left.arrow.right" // 원격: 화살표로 전환 상태 표시
        case .error:       return "exclamationmark.triangle.fill"
        case .authFailed:  return "lock.trianglebadge.exclamationmark.fill"
        default:           return "display.2"
        }
    }

    /// 팝오버 상태 헤더에 표시할 SF Symbol. 상태를 아이콘으로도 구분한다.
    var statusHeaderSymbol: String {
        switch status {
        case .idle:        return "pause.circle.fill"
        case .waiting:     return "clock.fill"
        case .connected:   return "checkmark.circle.fill"
        case .remote:      return "arrow.left.arrow.right.circle.fill"
        case .error:       return "exclamationmark.triangle.fill"
        case .authFailed:  return "lock.trianglebadge.exclamationmark.fill"
        default:           return "circle.fill"
        }
    }
}

// SwiftUI Color를 Foundation에서 쓰기 위한 import
import SwiftUI
