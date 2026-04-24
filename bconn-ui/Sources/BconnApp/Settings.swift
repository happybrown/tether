import Foundation
import Security

enum ClientEdge: Int, CaseIterable, Identifiable, Codable {
    case right  = 0
    case left   = 1
    case top    = 2
    case bottom = 3

    var id: Int { rawValue }

    var label: String {
        switch self {
        case .right:  return "오른쪽"
        case .left:   return "왼쪽"
        case .top:    return "위쪽"
        case .bottom: return "아래쪽"
        }
    }
}

enum AppMode: String, CaseIterable, Identifiable, Codable {
    case host   = "host"
    case client = "client"

    var id: String { rawValue }
    var label: String { self == .host ? "Host" : "Client" }
}

class AppSettings: ObservableObject {
    static let shared = AppSettings()

    @Published var mode: AppMode {
        didSet { save() }
    }
    @Published var hostIP: String {
        didSet { save() }
    }
    @Published var port: Int {
        didSet { save() }
    }
    @Published var clientEdge: ClientEdge {
        didSet { save() }
    }
    @Published var launchAtLogin: Bool {
        didSet { save() }
    }
    @Published var autoStart: Bool {
        didSet { save() }
    }
    @Published var psk: String {
        didSet {
            psk.isEmpty ? KeychainHelper.delete() : KeychainHelper.write(psk)
            save()
        }
    }
    @Published var reconnectInterval: Int {
        didSet { save() }
    }

    private let defaults = UserDefaults.standard

    private init() {
        mode               = AppMode(rawValue: defaults.string(forKey: "mode") ?? "") ?? .host
        hostIP             = defaults.string(forKey: "hostIP") ?? ""
        port               = defaults.integer(forKey: "port").nonZero ?? 24900
        clientEdge         = ClientEdge(rawValue: defaults.integer(forKey: "clientEdge")) ?? .right
        launchAtLogin      = defaults.bool(forKey: "launchAtLogin")
        autoStart          = defaults.bool(forKey: "autoStart")
        psk                = KeychainHelper.read()
        reconnectInterval  = defaults.integer(forKey: "reconnectInterval").nonZero ?? 5
    }

    private func save() {
        defaults.set(mode.rawValue,       forKey: "mode")
        defaults.set(hostIP,              forKey: "hostIP")
        defaults.set(port,                forKey: "port")
        defaults.set(clientEdge.rawValue, forKey: "clientEdge")
        defaults.set(launchAtLogin,       forKey: "launchAtLogin")
        defaults.set(autoStart,           forKey: "autoStart")
        defaults.set(reconnectInterval,   forKey: "reconnectInterval")
        // psk는 didSet에서 Keychain에 직접 저장하므로 여기서는 생략
    }
}

private extension Int {
    var nonZero: Int? { self == 0 ? nil : self }
}

// PSK는 UserDefaults 대신 Keychain에 저장한다.
enum KeychainHelper {
    private static let service = "com.bconn.app"
    private static let account = "psk"

    static func read() -> String {
        let query: [CFString: Any] = [
            kSecClass:            kSecClassGenericPassword,
            kSecAttrService:      service,
            kSecAttrAccount:      account,
            kSecReturnData:       true,
            kSecMatchLimit:       kSecMatchLimitOne,
        ]
        var result: AnyObject?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data,
              let str  = String(data: data, encoding: .utf8) else { return "" }
        return str
    }

    static func write(_ value: String) {
        let data = Data(value.utf8)
        let query: [CFString: Any] = [
            kSecClass:       kSecClassGenericPassword,
            kSecAttrService: service,
            kSecAttrAccount: account,
        ]
        // kSecAttrAccessible은 생성 후 변경 불가 + 프롬프트 방지를 위해 항상 삭제 후 재삽입.
        SecItemDelete(query as CFDictionary)
        var item = query
        item[kSecValueData]      = data
        item[kSecAttrAccessible] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        SecItemAdd(item as CFDictionary, nil)
    }

    static func delete() {
        let query: [CFString: Any] = [
            kSecClass:       kSecClassGenericPassword,
            kSecAttrService: service,
            kSecAttrAccount: account,
        ]
        SecItemDelete(query as CFDictionary)
    }
}
