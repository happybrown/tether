import Foundation
import BconnCore

@MainActor
class BconnManager: ObservableObject {
    static let shared = BconnManager()

    @Published var status: BconnStatus = .idle
    @Published var statusMessage: String = "중지됨"
    @Published var localIPs: [String] = []
    @Published var isRunning = false

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

        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        bconn_start(&config, { status, msg, ctx in
            guard let ctx else { return }
            let manager = Unmanaged<BconnManager>.fromOpaque(ctx).takeUnretainedValue()
            let message = msg.map { String(cString: $0) } ?? ""
            DispatchQueue.main.async {
                manager.status = status
                manager.statusMessage = message
                NotificationCenter.default.post(name: .bconnStatusChanged, object: nil)
            }
        }, selfPtr)

        isRunning = true
        status = bconn_get_status()
    }

    func stop() {
        bconn_stop()
        isRunning = false
        status = .idle
        statusMessage = "중지됨"
        NotificationCenter.default.post(name: .bconnStatusChanged, object: nil)
    }

    var statusLabel: String {
        switch status {
        case .idle:      return "중지됨"
        case .waiting:   return "대기 중"
        case .connected: return "연결됨"
        case .remote:    return "원격 활성"
        case .error:     return statusMessage
        default:         return "알 수 없음"
        }
    }

    var statusColor: Color {
        switch status {
        case .idle:      return .gray
        case .waiting:   return .yellow
        case .connected: return .green
        case .remote:    return .blue
        case .error:     return .red
        default:         return .gray
        }
    }
}

// SwiftUI Color를 Foundation에서 쓰기 위한 import
import SwiftUI
