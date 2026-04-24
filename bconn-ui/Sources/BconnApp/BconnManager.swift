import Foundation
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

    private init() {
        refreshLocalIPs()
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
    }

    func stop() {
        isRunning = false
        status = .idle
        statusMessage = "중지됨"
        connectedHostname = ""
        authFailed = false
        NotificationCenter.default.post(name: .bconnStatusChanged, object: nil)

        // bconn_stop()은 내부 스레드(run_loop_thread, heartbeat_thread)를 join하므로
        // 메인 스레드에서 직접 호출하면 set_status 콜백의 DispatchQueue.main.async와
        // 데드락이 발생한다. 백그라운드에서 실행한다.
        Task.detached { bconn_stop() }
    }

    /// 재시작 시 stop 과 start 가 race 하지 않도록 완료 대기.
    /// C++ 측 g_lifecycle_mu 가 실제 상호 배제를 수행하지만, 여기서는 main thread 가
    /// 잠시라도 block 되는 걸 피하기 위해 background 에서 순차 실행한다.
    func restart() {
        isRunning = false
        status = .idle
        statusMessage = "재시작 중..."
        connectedHostname = ""
        authFailed = false
        NotificationCenter.default.post(name: .bconnStatusChanged, object: nil)

        Task.detached { [weak self] in
            bconn_stop()
            await MainActor.run {
                self?.start()
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
}

// SwiftUI Color를 Foundation에서 쓰기 위한 import
import SwiftUI
