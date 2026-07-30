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
    case host
    case client

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
        didSet { save() }
    }
    @Published var reconnectInterval: Int {
        didSet { save() }
    }
    @Published var predictLeave: Bool {
        didSet { save() }
    }
    // client 화면 정렬 오프셋 (-1.0 ~ +1.0, client 화면 크기 대비 비율).
    // 해상도 차이로 전환 위치가 어긋날 때 경계 축 방향 미세조정에 쓴다.
    @Published var clientOffset: Double {
        didSet { save() }
    }
    // 전환 기준 화면의 디스플레이 UUID. 빈 문자열이면 자동(데스크톱 최외곽 경계).
    // 멀티 모니터에서 "맥북 화면 오른쪽 → 상대 Mac" 처럼 특정 화면을 지정할 때 쓴다.
    @Published var triggerDisplayUUID: String {
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
        psk                = defaults.string(forKey: "psk") ?? ""
        reconnectInterval  = defaults.integer(forKey: "reconnectInterval").nonZero ?? 5
        // 기본값 false. bool(forKey:)는 미설정 시 false 라 키 존재 여부로 판단한다.
        predictLeave       = defaults.object(forKey: "predictLeave") as? Bool ?? false
        clientOffset       = defaults.object(forKey: "clientOffset") as? Double ?? 0
        triggerDisplayUUID = defaults.string(forKey: "triggerDisplayUUID") ?? ""
    }

    private func save() {
        defaults.set(mode.rawValue, forKey: "mode")
        defaults.set(hostIP, forKey: "hostIP")
        defaults.set(port, forKey: "port")
        defaults.set(clientEdge.rawValue, forKey: "clientEdge")
        defaults.set(launchAtLogin, forKey: "launchAtLogin")
        defaults.set(autoStart, forKey: "autoStart")
        defaults.set(reconnectInterval, forKey: "reconnectInterval")
        defaults.set(psk, forKey: "psk")
        defaults.set(predictLeave, forKey: "predictLeave")
        defaults.set(clientOffset, forKey: "clientOffset")
        defaults.set(triggerDisplayUUID, forKey: "triggerDisplayUUID")
    }
}

private extension Int {
    var nonZero: Int? { self == 0 ? nil : self }
}
