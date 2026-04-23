import XCTest
import BconnCore

// ── BconnConfig 브리징 테스트 ─────────────────────────────────────────
//
// Swift에서 BconnConfig 구조체를 올바르게 채우는지 검증한다.
// BconnManager는 application 번들이라 직접 import할 수 없으므로
// BconnConfig 조립 로직을 독립적으로 테스트한다.

final class BconnConfigBridgingTests: XCTestCase {

    func test_hostIp_copiedCorrectly() {
        var config = BconnConfig()
        let ip = "192.168.1.100"
        ip.withCString { src in
            withUnsafeMutableBytes(of: &config.host_ip) { ptr in
                let dest = ptr.baseAddress!.assumingMemoryBound(to: CChar.self)
                _ = strlcpy(dest, src, 64)
            }
        }
        let result = withUnsafeBytes(of: config.host_ip) { ptr in
            String(cString: ptr.baseAddress!.assumingMemoryBound(to: CChar.self))
        }
        XCTAssertEqual(result, ip)
    }

    func test_psk_copiedCorrectly() {
        var config = BconnConfig()
        let psk = "my_shared_secret"
        psk.withCString { src in
            withUnsafeMutableBytes(of: &config.psk) { ptr in
                let dest = ptr.baseAddress!.assumingMemoryBound(to: CChar.self)
                _ = strlcpy(dest, src, 128)
            }
        }
        let result = withUnsafeBytes(of: config.psk) { ptr in
            String(cString: ptr.baseAddress!.assumingMemoryBound(to: CChar.self))
        }
        XCTAssertEqual(result, psk)
    }

    func test_psk_empty_copiedAsEmpty() {
        var config = BconnConfig()
        "".withCString { src in
            withUnsafeMutableBytes(of: &config.psk) { ptr in
                let dest = ptr.baseAddress!.assumingMemoryBound(to: CChar.self)
                _ = strlcpy(dest, src, 128)
            }
        }
        let result = withUnsafeBytes(of: config.psk) { ptr in
            String(cString: ptr.baseAddress!.assumingMemoryBound(to: CChar.self))
        }
        XCTAssertEqual(result, "")
    }

    func test_psk_truncatedAt128Chars() {
        var config = BconnConfig()
        let longPsk = String(repeating: "x", count: 200)
        longPsk.withCString { src in
            withUnsafeMutableBytes(of: &config.psk) { ptr in
                let dest = ptr.baseAddress!.assumingMemoryBound(to: CChar.self)
                _ = strlcpy(dest, src, 128)
            }
        }
        // strlcpy는 127자 + null terminator로 잘라냄
        let result = withUnsafeBytes(of: config.psk) { ptr in
            String(cString: ptr.baseAddress!.assumingMemoryBound(to: CChar.self))
        }
        XCTAssertEqual(result.count, 127)
    }

    func test_port_assignedCorrectly() {
        var config = BconnConfig()
        config.port = 24900
        XCTAssertEqual(config.port, 24900)
    }

    func test_isHost_flag() {
        var hostConfig = BconnConfig()
        hostConfig.is_host = true
        XCTAssertTrue(hostConfig.is_host)

        var clientConfig = BconnConfig()
        clientConfig.is_host = false
        XCTAssertFalse(clientConfig.is_host)
    }

    func test_clientEdge_values() {
        XCTAssertEqual(BCONN_EDGE_RIGHT.rawValue,  0)
        XCTAssertEqual(BCONN_EDGE_LEFT.rawValue,   1)
        XCTAssertEqual(BCONN_EDGE_TOP.rawValue,    2)
        XCTAssertEqual(BCONN_EDGE_BOTTOM.rawValue, 3)
    }
}

// ── BconnStatus Swift 확장 테스트 ─────────────────────────────────────

final class BconnStatusTests: XCTestCase {

    func test_statusValues() {
        XCTAssertEqual(BCONN_STATUS_IDLE.rawValue,        0)
        XCTAssertEqual(BCONN_STATUS_WAITING.rawValue,     1)
        XCTAssertEqual(BCONN_STATUS_CONNECTED.rawValue,   2)
        XCTAssertEqual(BCONN_STATUS_REMOTE.rawValue,      3)
        XCTAssertEqual(BCONN_STATUS_ERROR.rawValue,       4)
        XCTAssertEqual(BCONN_STATUS_AUTH_FAILED.rawValue, 5)
    }

    func test_statusLabel_mapping() {
        // statusLabel 로직을 인라인으로 검증 (BconnManager를 import할 수 없으므로)
        func label(for status: BconnStatus) -> String {
            switch status {
            case BCONN_STATUS_IDLE:        return "중지됨"
            case BCONN_STATUS_WAITING:     return "대기 중"
            case BCONN_STATUS_CONNECTED:   return "연결됨"
            case BCONN_STATUS_REMOTE:      return "원격 활성"
            case BCONN_STATUS_ERROR:       return "오류"
            case BCONN_STATUS_AUTH_FAILED: return "인증 실패"
            default:                       return "알 수 없음"
            }
        }

        XCTAssertEqual(label(for: BCONN_STATUS_IDLE),        "중지됨")
        XCTAssertEqual(label(for: BCONN_STATUS_WAITING),     "대기 중")
        XCTAssertEqual(label(for: BCONN_STATUS_CONNECTED),   "연결됨")
        XCTAssertEqual(label(for: BCONN_STATUS_REMOTE),      "원격 활성")
        XCTAssertEqual(label(for: BCONN_STATUS_AUTH_FAILED), "인증 실패")
    }
}

// ── bconn_get_local_ips C API 테스트 ──────────────────────────────────

final class LocalIPsTests: XCTestCase {

    func test_getLocalIps_returnsNonEmpty() {
        var buf = [CChar](repeating: 0, count: 512)
        let count = buf.withUnsafeMutableBufferPointer { ptr in
            bconn_get_local_ips(ptr.baseAddress, Int32(ptr.count))
        }
        // 네트워크 인터페이스가 최소 1개(Wi-Fi 또는 Ethernet) 존재
        XCTAssertGreaterThan(count, 0, "로컬 IP가 최소 1개 있어야 함")
    }

    func test_getLocalIps_formattedAsNewlineSeparated() {
        var buf = [CChar](repeating: 0, count: 512)
        let count = buf.withUnsafeMutableBufferPointer { ptr in
            bconn_get_local_ips(ptr.baseAddress, Int32(ptr.count))
        }
        guard count > 0 else { return }

        let raw = String(cString: buf)
        let ips = raw.split(separator: "\n").map(String.init)
        XCTAssertEqual(ips.count, count, "IP 개수와 줄 수가 일치해야 함")
    }

    func test_getLocalIps_noLoopback() {
        var buf = [CChar](repeating: 0, count: 512)
        let count = buf.withUnsafeMutableBufferPointer { ptr in
            bconn_get_local_ips(ptr.baseAddress, Int32(ptr.count))
        }
        guard count > 0 else { return }

        let raw = String(cString: buf)
        let ips = raw.split(separator: "\n").map(String.init)
        for ip in ips {
            XCTAssertFalse(ip.hasPrefix("127."), "루프백 주소는 포함되면 안 됨: \(ip)")
        }
    }

    func test_getLocalIps_smallBufferDoesNotCrash() {
        // 버퍼가 작아도 크래시 없이 처리
        var buf = [CChar](repeating: 0, count: 10)
        let count = buf.withUnsafeMutableBufferPointer { ptr in
            bconn_get_local_ips(ptr.baseAddress, Int32(ptr.count))
        }
        // 개수가 0이거나 버퍼 초과 방지 로직에 의해 잘림 (크래시 없음)
        XCTAssertGreaterThanOrEqual(count, 0)
    }
}

// ── bconn_start / bconn_stop 생명주기 테스트 ─────────────────────────

final class BconnLifecycleTests: XCTestCase {

    override func tearDown() {
        super.tearDown()
        // 테스트 후 반드시 중지 (다음 테스트에 영향 없도록)
        bconn_stop()
    }

    func test_initialStatus_isIdle() {
        // 앱 기동 직후 (또는 stop 후) 상태는 IDLE
        let status = bconn_get_status()
        XCTAssertEqual(status, BCONN_STATUS_IDLE)
    }

    func test_start_host_setsWaitingStatus() {
        var config = BconnConfig()
        config.is_host    = true
        config.port       = 29800
        config.client_edge = BCONN_EDGE_RIGHT
        // PSK 없음 (open mode)
        "".withCString { src in
            withUnsafeMutableBytes(of: &config.psk) { ptr in
                _ = strlcpy(ptr.baseAddress!.assumingMemoryBound(to: CChar.self), src, 128)
            }
        }

        let started = bconn_start(&config, nil, nil)
        // Event Tap 권한이 없으면 ERROR, 권한이 있으면 WAITING
        // 테스트 환경에서는 권한 없을 수 있으므로 started 여부만 확인
        XCTAssertTrue(started, "bconn_start 호출 자체는 성공해야 함")

        let status = bconn_get_status()
        XCTAssertTrue(
            status == BCONN_STATUS_WAITING || status == BCONN_STATUS_ERROR,
            "host 시작 후 상태는 WAITING 또는 ERROR여야 함 (실제: \(status.rawValue))"
        )
    }

    func test_stop_resetsToIdle() {
        var config = BconnConfig()
        config.is_host = true
        config.port    = 29801
        config.client_edge = BCONN_EDGE_RIGHT

        _ = bconn_start(&config, nil, nil)
        bconn_stop()

        XCTAssertEqual(bconn_get_status(), BCONN_STATUS_IDLE)
    }

    func test_doubleStart_returnsFalse() {
        var config = BconnConfig()
        config.is_host = true
        config.port    = 29802
        config.client_edge = BCONN_EDGE_RIGHT

        let first  = bconn_start(&config, nil, nil)
        let second = bconn_start(&config, nil, nil)  // 이미 실행 중

        XCTAssertTrue(first,   "첫 번째 start는 성공해야 함")
        XCTAssertFalse(second, "이미 실행 중일 때 start는 false 반환")
    }

    func test_statusCallback_called() {
        var config = BconnConfig()
        config.is_host    = true
        config.port       = 29803
        config.client_edge = BCONN_EDGE_RIGHT

        let expectation = self.expectation(description: "상태 콜백 호출")
        expectation.assertForOverFulfill = false

        // C 함수 포인터로 클로저를 직접 넘길 수 없으므로 박스 사용
        class Box { var fulfill: (() -> Void)? }
        let box = Box()
        box.fulfill = { expectation.fulfill() }
        let ptr = Unmanaged.passRetained(box).toOpaque()

        bconn_start(&config, { _, _, ctx in
            guard let ctx else { return }
            let b = Unmanaged<Box>.fromOpaque(ctx).takeUnretainedValue()
            b.fulfill?()
        }, ptr)

        wait(for: [expectation], timeout: 2.0)
        Unmanaged<Box>.fromOpaque(ptr).release()
    }
}
