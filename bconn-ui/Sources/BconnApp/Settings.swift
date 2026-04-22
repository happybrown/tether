import Foundation

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

    private let defaults = UserDefaults.standard

    private init() {
        mode        = AppMode(rawValue: defaults.string(forKey: "mode") ?? "") ?? .host
        hostIP      = defaults.string(forKey: "hostIP") ?? ""
        port        = defaults.integer(forKey: "port").nonZero ?? 24900
        clientEdge  = ClientEdge(rawValue: defaults.integer(forKey: "clientEdge")) ?? .right
        launchAtLogin = defaults.bool(forKey: "launchAtLogin")
    }

    private func save() {
        defaults.set(mode.rawValue,      forKey: "mode")
        defaults.set(hostIP,             forKey: "hostIP")
        defaults.set(port,               forKey: "port")
        defaults.set(clientEdge.rawValue, forKey: "clientEdge")
        defaults.set(launchAtLogin,      forKey: "launchAtLogin")
    }
}

private extension Int {
    var nonZero: Int? { self == 0 ? nil : self }
}
