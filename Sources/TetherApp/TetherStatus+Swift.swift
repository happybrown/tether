import TetherCore

// C enum을 Swift에서 편하게 쓰기 위한 타입 별칭 및 상수
typealias Status = TetherStatus

extension TetherStatus {
    static let idle       = TETHER_STATUS_IDLE
    static let waiting    = TETHER_STATUS_WAITING
    static let connected  = TETHER_STATUS_CONNECTED
    static let remote     = TETHER_STATUS_REMOTE
    static let error      = TETHER_STATUS_ERROR
    static let authFailed = TETHER_STATUS_AUTH_FAILED
}
