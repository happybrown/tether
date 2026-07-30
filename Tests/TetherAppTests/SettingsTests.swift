import XCTest
import TetherCore

// ── TetherConfig 브리징 테스트 ─────────────────────────────────────────
//
// Swift에서 TetherConfig 구조체를 올바르게 채우는지 검증한다.
// TetherManager는 application 번들이라 직접 import할 수 없으므로
// TetherConfig 조립 로직을 독립적으로 테스트한다.

final class TetherConfigBridgingTests: XCTestCase {

    func test_hostIp_copiedCorrectly() {
        var config = TetherConfig()
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
        var config = TetherConfig()
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
        var config = TetherConfig()
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
        var config = TetherConfig()
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
        var config = TetherConfig()
        config.port = 24900
        XCTAssertEqual(config.port, 24900)
    }

    func test_clientSize_unknownBeforeConnection() {
        // ClientInfoPacket 수신 전(미실행 포함)에는 false — UI 가 "미상"으로 표시하는 근거
        var w: Int32 = -1, h: Int32 = -1
        XCTAssertFalse(tether_get_client_size(&w, &h))
    }

    func test_clientOffset_defaultsToZeroAndAssigns() {
        var config = TetherConfig()
        // 미설정 시 0 (오프셋 없음 = 기존 동작)
        XCTAssertEqual(config.client_offset, 0)
        config.client_offset = -0.25
        XCTAssertEqual(config.client_offset, -0.25)
        // 실행 전 호출해도 안전해야 한다 (IDLE 상태에선 값만 보관)
        tether_set_client_offset(0.5)
        tether_set_client_offset(0)
    }

    func test_isHost_flag() {
        var hostConfig = TetherConfig()
        hostConfig.is_host = true
        XCTAssertTrue(hostConfig.is_host)

        var clientConfig = TetherConfig()
        clientConfig.is_host = false
        XCTAssertFalse(clientConfig.is_host)
    }

    func test_clientEdge_values() {
        XCTAssertEqual(TETHER_EDGE_RIGHT.rawValue, 0)
        XCTAssertEqual(TETHER_EDGE_LEFT.rawValue, 1)
        XCTAssertEqual(TETHER_EDGE_TOP.rawValue, 2)
        XCTAssertEqual(TETHER_EDGE_BOTTOM.rawValue, 3)
    }
}

// ── TetherStatus Swift 확장 테스트 ─────────────────────────────────────

final class TetherStatusTests: XCTestCase {

    func test_statusValues() {
        XCTAssertEqual(TETHER_STATUS_IDLE.rawValue, 0)
        XCTAssertEqual(TETHER_STATUS_WAITING.rawValue, 1)
        XCTAssertEqual(TETHER_STATUS_CONNECTED.rawValue, 2)
        XCTAssertEqual(TETHER_STATUS_REMOTE.rawValue, 3)
        XCTAssertEqual(TETHER_STATUS_ERROR.rawValue, 4)
        XCTAssertEqual(TETHER_STATUS_AUTH_FAILED.rawValue, 5)
    }

    func test_statusLabel_mapping() {
        // statusLabel 로직을 인라인으로 검증 (TetherManager를 import할 수 없으므로)
        func label(for status: TetherStatus) -> String {
            switch status {
            case TETHER_STATUS_IDLE:        return "중지됨"
            case TETHER_STATUS_WAITING:     return "대기 중"
            case TETHER_STATUS_CONNECTED:   return "연결됨"
            case TETHER_STATUS_REMOTE:      return "원격 활성"
            case TETHER_STATUS_ERROR:       return "오류"
            case TETHER_STATUS_AUTH_FAILED: return "인증 실패"
            default:                       return "알 수 없음"
            }
        }

        XCTAssertEqual(label(for: TETHER_STATUS_IDLE), "중지됨")
        XCTAssertEqual(label(for: TETHER_STATUS_WAITING), "대기 중")
        XCTAssertEqual(label(for: TETHER_STATUS_CONNECTED), "연결됨")
        XCTAssertEqual(label(for: TETHER_STATUS_REMOTE), "원격 활성")
        XCTAssertEqual(label(for: TETHER_STATUS_AUTH_FAILED), "인증 실패")
    }
}

// ── tether_get_local_ips C API 테스트 ──────────────────────────────────

final class LocalIPsTests: XCTestCase {

    func test_getLocalIps_returnsNonEmpty() {
        var buf = [CChar](repeating: 0, count: 512)
        let count = buf.withUnsafeMutableBufferPointer { ptr in
            tether_get_local_ips(ptr.baseAddress, Int32(ptr.count))
        }
        // 네트워크 인터페이스가 최소 1개(Wi-Fi 또는 Ethernet) 존재
        XCTAssertGreaterThan(count, 0, "로컬 IP가 최소 1개 있어야 함")
    }

    func test_getLocalIps_formattedAsNewlineSeparated() {
        var buf = [CChar](repeating: 0, count: 512)
        let count = buf.withUnsafeMutableBufferPointer { ptr in
            tether_get_local_ips(ptr.baseAddress, Int32(ptr.count))
        }
        guard count > 0 else { return }

        let raw = String(cString: buf)
        let ips = raw.split(separator: "\n").map(String.init)
        XCTAssertEqual(ips.count, Int(count), "IP 개수와 줄 수가 일치해야 함")
    }

    func test_getLocalIps_noLoopback() {
        var buf = [CChar](repeating: 0, count: 512)
        let count = buf.withUnsafeMutableBufferPointer { ptr in
            tether_get_local_ips(ptr.baseAddress, Int32(ptr.count))
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
            tether_get_local_ips(ptr.baseAddress, Int32(ptr.count))
        }
        // 개수가 0이거나 버퍼 초과 방지 로직에 의해 잘림 (크래시 없음)
        XCTAssertGreaterThanOrEqual(count, 0)
    }
}

// ── tether_start / tether_stop 생명주기 테스트 ─────────────────────────

final class TetherLifecycleTests: XCTestCase {

    override func tearDown() {
        super.tearDown()
        // 테스트 후 반드시 중지 (다음 테스트에 영향 없도록)
        tether_stop()
    }

    func test_initialStatus_isIdle() {
        // 앱 기동 직후 (또는 stop 후) 상태는 IDLE
        let status = tether_get_status()
        XCTAssertEqual(status, TETHER_STATUS_IDLE)
    }

    func test_start_host_setsWaitingStatus() {
        var config = TetherConfig()
        config.is_host    = true
        config.port       = 29800
        config.client_edge = TETHER_EDGE_RIGHT
        // PSK 없음 (open mode)
        "".withCString { src in
            withUnsafeMutableBytes(of: &config.psk) { ptr in
                _ = strlcpy(ptr.baseAddress!.assumingMemoryBound(to: CChar.self), src, 128)
            }
        }

        let started = tether_start(&config, nil, nil)
        // Event Tap 권한이 없으면 ERROR, 권한이 있으면 WAITING
        // 테스트 환경에서는 권한 없을 수 있으므로 started 여부만 확인
        XCTAssertTrue(started, "tether_start 호출 자체는 성공해야 함")

        let status = tether_get_status()
        XCTAssertTrue(
            status == TETHER_STATUS_WAITING || status == TETHER_STATUS_ERROR,
            "host 시작 후 상태는 WAITING 또는 ERROR여야 함 (실제: \(status.rawValue))"
        )
    }

    func test_stop_resetsToIdle() {
        var config = TetherConfig()
        config.is_host = true
        config.port    = 29801
        config.client_edge = TETHER_EDGE_RIGHT

        _ = tether_start(&config, nil, nil)
        tether_stop()

        XCTAssertEqual(tether_get_status(), TETHER_STATUS_IDLE)
    }

    func test_doubleStart_returnsFalse() {
        var config = TetherConfig()
        config.is_host = true
        config.port    = 29802
        config.client_edge = TETHER_EDGE_RIGHT

        let first  = tether_start(&config, nil, nil)
        let second = tether_start(&config, nil, nil)  // 이미 실행 중

        XCTAssertTrue(first, "첫 번째 start는 성공해야 함")
        XCTAssertFalse(second, "이미 실행 중일 때 start는 false 반환")
    }

    func test_setClientEdge_whileIdle_isNoOpAndDoesNotCrash() {
        // IDLE 상태에서 호출해도 크래시 없이 무시된다 (실행 전 설정은 config 로 전달됨)
        XCTAssertEqual(tether_get_status(), TETHER_STATUS_IDLE)
        tether_set_client_edge(TETHER_EDGE_LEFT)
        tether_set_client_edge(TETHER_EDGE_TOP)
        // 여전히 IDLE — 세션을 시작시키지 않는다
        XCTAssertEqual(tether_get_status(), TETHER_STATUS_IDLE)
    }

    func test_setClientEdge_whileRunning_doesNotRestartSession() {
        var config = TetherConfig()
        config.is_host    = true
        config.port       = 29804
        config.client_edge = TETHER_EDGE_RIGHT

        XCTAssertTrue(tether_start(&config, nil, nil))
        let before = tether_get_status()

        // 실행 중 위치 변경 — 재시작(IDLE 경유) 없이 상태가 유지되어야 한다
        tether_set_client_edge(TETHER_EDGE_BOTTOM)
        tether_set_client_edge(TETHER_EDGE_LEFT)

        let after = tether_get_status()
        XCTAssertNotEqual(after, TETHER_STATUS_IDLE, "위치 변경이 세션을 중지시키면 안 됨")
        XCTAssertEqual(after, before, "위치 변경 전후 상태가 동일해야 함 (재시작 없음)")
    }

    func test_statusCallback_called() {
        var config = TetherConfig()
        config.is_host    = true
        config.port       = 29803
        config.client_edge = TETHER_EDGE_RIGHT

        let expectation = self.expectation(description: "상태 콜백 호출")
        expectation.assertForOverFulfill = false

        // C 함수 포인터로 클로저를 직접 넘길 수 없으므로 박스 사용
        class Box { var fulfill: (() -> Void)? }
        let box = Box()
        box.fulfill = { expectation.fulfill() }
        let ptr = Unmanaged.passRetained(box).toOpaque()

        tether_start(&config, { _, _, ctx in
            guard let ctx else { return }
            let b = Unmanaged<Box>.fromOpaque(ctx).takeUnretainedValue()
            b.fulfill?()
        }, ptr)

        wait(for: [expectation], timeout: 2.0)
        Unmanaged<Box>.fromOpaque(ptr).release()
    }
}
