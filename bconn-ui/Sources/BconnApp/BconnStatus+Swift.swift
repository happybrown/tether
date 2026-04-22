import BconnCore

// C enum을 Swift에서 편하게 쓰기 위한 타입 별칭 및 상수
typealias Status = BconnStatus

extension BconnStatus {
    static let idle      = BCONN_STATUS_IDLE
    static let waiting   = BCONN_STATUS_WAITING
    static let connected = BCONN_STATUS_CONNECTED
    static let remote    = BCONN_STATUS_REMOTE
    static let error     = BCONN_STATUS_ERROR
}
