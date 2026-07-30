import Foundation
import AppKit
import ApplicationServices
import TetherCore

@MainActor
class TetherManager: ObservableObject {
    static let shared = TetherManager()

    @Published var status: TetherStatus = .idle
    @Published var statusMessage: String = "중지됨"
    @Published var localIPs: [String] = []
    @Published var isRunning = false
    @Published var connectedHostname: String = ""
    @Published var authFailed: Bool = false
    @Published var hasAccessibilityPermission: Bool = false
    // 연결된 client 의 화면 해상도 (host 모드, 미수신이면 nil). 배치 UI 시각화용.
    @Published var clientScreenSize: CGSize?

    private var permissionTimer: Timer?
    private var clipboardTimer: Timer?
    // 마지막으로 전송한 클립보드 페이로드 (중복 전송 방지)
    private var lastSentClipboard = Data()
    // 마지막으로 수신한 클립보드 페이로드 (루프 방지)
    private var lastReceivedClipboard = Data()
    // NSPasteboard changeCount 기반 변경 감지 (폴링 효율화)
    private var lastChangeCount: Int = 0
    // 큰 이미지 전송은 C 코어의 청크 페이싱으로 수백 ms 걸리므로 메인 스레드 밖에서 보낸다
    private let clipboardSendQueue = DispatchQueue(label: "tether.clipboard.send")

    private init() {
        refreshLocalIPs()
        refreshAccessibilityPermission()
        startPermissionPolling()
        registerClipboardCallback()
        registerForceStopCallback()
        observeScreenChanges()
    }

    /// 모니터 연결/해제/배치 변경을 관찰해 C 코어에 통지한다.
    /// host 는 커서 전환 경계 레이아웃을, client 는 화면 해상도를 재계산한다.
    /// 실행 중이 아니면 C 쪽에서 무시하므로 항상 관찰해도 안전하다.
    private func observeScreenChanges() {
        NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification,
            object: nil, queue: .main
        ) { _ in
            tether_displays_changed()
        }
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
    /// client 해상도도 함께 갱신한다 (연결 중 디스플레이 변경 반영, 콜백 없이 폴링으로 충분).
    private func startPermissionPolling() {
        permissionTimer = Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.refreshAccessibilityPermission()
                self?.refreshClientScreenSize()
            }
        }
    }

    /// C 코어에서 client 화면 크기를 읽어 UI 상태로 반영한다 (host 전용, 미수신이면 nil).
    func refreshClientScreenSize() {
        var w: Int32 = 0, h: Int32 = 0
        if tether_get_client_size(&w, &h), w > 0, h > 0 {
            clientScreenSize = CGSize(width: CGFloat(w), height: CGFloat(h))
        } else {
            clientScreenSize = nil
        }
    }

    /// 앱 표시용 버전 문자열 (예: "0.8.1"). Info.plist 의 CFBundleShortVersionString 을 읽는다.
    var appVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "—"
    }

    /// 앱 표시용 이름 (예: "Tether"). 번들 이름을 읽어 rename 시 자동 반영된다.
    var appName: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String ?? "Tether"
    }

    func refreshLocalIPs() {
        var buf = [CChar](repeating: 0, count: 512)
        let count = buf.withUnsafeMutableBufferPointer { ptr in
            tether_get_local_ips(ptr.baseAddress, Int32(ptr.count))
        }
        guard count > 0 else { localIPs = []; return }
        localIPs = String(cString: buf)
            .split(separator: "\n")
            .map(String.init)
    }

    func start() {
        let settings = AppSettings.shared
        var config = TetherConfig()

        config.is_host     = settings.mode == .host
        config.port        = UInt16(settings.port)
        config.client_edge = TetherEdge(UInt32(settings.clientEdge.rawValue))

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
        config.predict_leave = settings.predictLeave
        config.client_offset = settings.clientOffset

        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        tether_start(&config, { status, msg, ctx in
            guard let ctx else { return }
            let manager = Unmanaged<TetherManager>.fromOpaque(ctx).takeUnretainedValue()
            let message = msg.map { String(cString: $0) } ?? ""
            DispatchQueue.main.async {
                manager.status = status
                manager.statusMessage = message
                manager.refreshClientScreenSize()
                if status == TETHER_STATUS_CONNECTED && !message.isEmpty
                    && message != "연결됨" && message != "재연결 중..." {
                    // host: message에 클라이언트 호스트명이 담겨 있음
                    manager.connectedHostname = message
                    manager.authFailed = false
                } else if status == TETHER_STATUS_AUTH_FAILED {
                    manager.authFailed = true
                    manager.connectedHostname = ""
                }
                NotificationCenter.default.post(name: .tetherStatusChanged, object: nil)
            }
        }, selfPtr)

        isRunning = true
        status = tether_get_status()
        startClipboardPolling()
    }

    /// client 위치(모니터 배치)를 실시간으로 반영한다.
    /// 실행 중이면 세션 재시작 없이 host 의 커서 전환 경계만 즉시 갱신하고,
    /// 실행 전이면 다음 start() 에서 설정값이 그대로 적용된다.
    func updateClientEdge(_ edge: ClientEdge) {
        tether_set_client_edge(TetherEdge(UInt32(edge.rawValue)))
    }

    /// 원격 복귀 예측 on/off 를 실행 중에도 재시작 없이 즉시 반영한다 (host 전용).
    func updatePredictLeave(_ enabled: Bool) {
        tether_set_predict_leave(enabled)
    }

    /// client 화면 정렬 오프셋을 실행 중에도 재시작 없이 즉시 반영한다 (host 전용).
    func updateClientOffset(_ offset: Double) {
        tether_set_client_offset(offset)
    }

    // MARK: - 네트워크 성능 측정

    /// 연결된 상대 Mac 으로 probe 를 왕복시켜 RTT 를 측정한다.
    /// C 코어가 백그라운드 스레드에서 측정 후 콜백하므로, 결과를 메인 큐로 넘겨 전달한다.
    /// 반환값: 측정을 시작했으면 true (미연결/중복 실행 시 false).
    @discardableResult
    func runNetPerf(count: Int, intervalMs: Int,
                    completion: @escaping (NetPerfResult) -> Void) -> Bool {
        // completion 을 heap 에 박스로 담아 opaque 포인터로 C 에 넘긴다.
        // 콜백에서 회수·해제하므로 정확히 한 번만 호출된다.
        let box = Box(completion)
        let ctx = Unmanaged.passRetained(box).toOpaque()

        let started = tether_run_netperf(Int32(count), Int32(intervalMs), { result, ctx in
            guard let ctx else { return }
            let box = Unmanaged<Box<(NetPerfResult) -> Void>>.fromOpaque(ctx).takeRetainedValue()
            let r = result?.pointee
            let parsed = NetPerfResult(
                sent: Int(r?.sent ?? 0),
                received: Int(r?.received ?? 0),
                lossPct: r?.loss_pct ?? 0,
                minMs: r?.min_ms ?? 0,
                avgMs: r?.avg_ms ?? 0,
                maxMs: r?.max_ms ?? 0,
                stddevMs: r?.stddev_ms ?? 0
            )
            DispatchQueue.main.async { box.value(parsed) }
        }, ctx)

        // 시작 못 했으면(미연결/중복) 박스를 회수해 누수를 막는다.
        if !started {
            Unmanaged<Box<(NetPerfResult) -> Void>>.fromOpaque(ctx).release()
        }
        return started
    }

    // MARK: - Escape Shortcuts

    /// Cmd+Shift+Esc 감지 시 세션을 강제 중지한다.
    /// CGEventTap RunLoop 스레드에서 호출되므로 Task.detached 로 비동기 처리한다.
    private func registerForceStopCallback() {
        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        tether_set_force_stop_callback({ ctx in
            guard let ctx else { return }
            let manager = Unmanaged<TetherManager>.fromOpaque(ctx).takeUnretainedValue()
            Task.detached {
                await MainActor.run { manager.stop() }
            }
        }, selfPtr)
    }

    // MARK: - Clipboard Sharing

    /// C++ 쪽에서 클립보드 수신 시 호출될 콜백을 등록한다.
    /// 페이로드는 [형식 1B(TetherClipFormat)] + 내용. 버퍼는 콜백 반환 후 무효화되므로 즉시 복사한다.
    private func registerClipboardCallback() {
        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        tether_set_clipboard_callback({ data, len, ctx in
            guard let data, let ctx, len > 1 else { return }
            let manager = Unmanaged<TetherManager>.fromOpaque(ctx).takeUnretainedValue()
            let payload = Data(bytes: data, count: Int(len))
            DispatchQueue.main.async {
                manager.applyReceivedClipboard(payload)
            }
        }, selfPtr)
    }

    // Universal Clipboard(Handoff) 재동기화 제외 마커.
    // 이 타입이 붙은 pasteboard 항목은 macOS 가 다른 기기로 다시 동기화하지 않는다
    // (1Password 등이 비밀번호에 쓰는 관례). tether 이 이미 UDP 로 빠르게 동기화하므로,
    // 이 마커로 macOS 의 느린 중복 동기화(“다른 컴퓨터에서 붙여넣는 중…” 창)를 막는다.
    private static let concealedType = NSPasteboard.PasteboardType("org.nspasteboard.ConcealedType")

    private func applyReceivedClipboard(_ payload: Data) {
        guard payload != lastReceivedClipboard else { return }
        lastReceivedClipboard = payload
        // 로컬 클립보드에 적용 (이 값을 다시 감지해 전송하지 않도록 lastSentClipboard 도 갱신)
        lastSentClipboard = payload

        // 첫 바이트가 형식(TetherClipFormat), 나머지가 내용
        let content = payload.dropFirst()
        let item = NSPasteboardItem()
        if payload[payload.startIndex] == UInt8(TETHER_CLIP_PNG.rawValue) {
            item.setData(Data(content), forType: .png)
        } else {
            guard let text = String(data: content, encoding: .utf8), !text.isEmpty else { return }
            item.setString(text, forType: .string)
        }
        item.setString("", forType: Self.concealedType)
        NSPasteboard.general.clearContents()
        NSPasteboard.general.writeObjects([item])
        // changeCount를 갱신해 다음 폴링에서 자체 변경을 무시
        lastChangeCount = NSPasteboard.general.changeCount
    }

    /// 현재 클립보드 내용을 전송용 페이로드([형식 1B] + 내용)로 만든다.
    /// 이미지가 있으면 이미지를 우선한다 (이미지 복사 시 부가 텍스트가 함께 실리는 경우 대비).
    private func currentClipboardPayload() -> Data? {
        let pb = NSPasteboard.general
        if let png = imagePNGData(from: pb) {
            return Data([UInt8(TETHER_CLIP_PNG.rawValue)]) + png
        }
        if let text = pb.string(forType: .string), !text.isEmpty {
            return Data([UInt8(TETHER_CLIP_TEXT.rawValue)]) + Data(text.utf8)
        }
        return nil
    }

    /// 클립보드의 이미지를 PNG 데이터로 읽는다. PNG 가 없으면 TIFF 를 PNG 로 변환한다
    /// (스크린샷은 PNG, 대부분의 앱 이미지 복사는 TIFF 로 실린다).
    private func imagePNGData(from pb: NSPasteboard) -> Data? {
        if let png = pb.data(forType: .png) { return png }
        guard let tiff = pb.data(forType: .tiff),
              let rep = NSBitmapImageRep(data: tiff) else { return nil }
        return rep.representation(using: .png, properties: [:])
    }

    /// 연결된 동안 클립보드를 0.5초 간격으로 폴링해 변경 시 상대방에 전송한다.
    private func startClipboardPolling() {
        stopClipboardPolling()
        lastSentClipboard = currentClipboardPayload() ?? Data()
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

        guard let payload = currentClipboardPayload(),
              payload != lastSentClipboard,
              payload != lastReceivedClipboard else { return }
        // C 코어 상한 초과분(초대형 이미지)은 어차피 폐기되므로 복사 전에 거른다
        guard payload.count <= Int(TETHER_CLIPBOARD_MAX_BYTES) else { return }

        lastSentClipboard = payload
        // 큰 이미지는 청크 페이싱으로 전송에 수백 ms 걸리므로 메인 스레드를 막지 않는다
        clipboardSendQueue.async {
            payload.withUnsafeBytes { ptr in
                if let base = ptr.bindMemory(to: UInt8.self).baseAddress {
                    tether_send_clipboard(base, UInt32(payload.count))
                }
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
        NotificationCenter.default.post(name: .tetherStatusChanged, object: nil)

        // tether_stop()은 진입 직후 g_cb=nullptr로 콜백을 해제하므로
        // set_status→DispatchQueue.main.async 데드락은 발생하지 않는다.
        // 동기 호출하여 앱 종료 시 C++ 스레드가 확실히 join되도록 한다.
        tether_stop()
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
        NotificationCenter.default.post(name: .tetherStatusChanged, object: nil)

        Task.detached {
            tether_stop()
            await MainActor.run {
                TetherManager.shared.start()
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

    /// 메뉴바 커스텀 아이콘(Asset) 이름. 연결 관련 3상태를 모니터 2대 아이콘의
    /// 화면 색으로 구분한다. 화면에 파랑이 들어온 쪽이 현재 제어 위치를 뜻한다.
    ///   미연결 → 양쪽 비움 / 로컬 → 왼쪽 파랑 / 원격 → 오른쪽 파랑
    /// 오류·인증실패는 경고성 SF Symbol 이 더 직관적이라 nil 을 반환해 그쪽으로 폴백한다.
    /// Asset 은 light/dark 두 벌 + template-rendering-intent=original 이라 부분 컬러가
    /// 메뉴바에서 그대로 보인다.
    var menuBarAsset: String? {
        switch status {
        case .idle, .waiting: return "monitors_default"
        case .connected:      return "monitors_left"
        case .remote:         return "monitors_right"
        default:              return nil   // 오류·인증실패: SF Symbol 폴백
        }
    }

    /// menuBarAsset 이 nil 인 상태(오류·인증실패)에서 쓰는 SF Symbol.
    var menuBarSymbol: String {
        switch status {
        case .error:      return "exclamationmark.triangle.fill"
        case .authFailed: return "lock.trianglebadge.exclamationmark.fill"
        default:          return "display.2"
        }
    }

    /// SF Symbol 폴백 상태의 아이콘 틴트. 커스텀 아이콘 상태는 자체 색을 담고 있어
    /// 틴트를 쓰지 않는다(nil).
    var menuBarTint: Color? {
        switch status {
        case .error:      return .red
        case .authFailed: return .orange
        default:          return nil
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

// ── 네트워크 성능 측정 결과 ────────────────────────────────────────────

struct NetPerfResult: Codable, Identifiable {
    var id = UUID()
    var date = Date()
    let sent: Int
    let received: Int
    let lossPct: Double
    let minMs: Double
    let avgMs: Double
    let maxMs: Double
    let stddevMs: Double

    var hasData: Bool { received > 0 }

    /// 평균 RTT 기준 체감 등급. 마우스 원격 조작 기준.
    var grade: (label: String, color: Color) {
        switch avgMs {
        case ..<5:   return ("아주 좋음", .green)
        case ..<15:  return ("좋음", .green)
        case ..<30:  return ("보통", .yellow)
        case ..<60:  return ("느림", .orange)
        default:     return ("매우 느림", .red)
        }
    }
}

/// Swift 클로저를 C opaque 포인터로 넘기기 위한 heap 박스.
final class Box<T> {
    let value: T
    init(_ value: T) { self.value = value }
}
