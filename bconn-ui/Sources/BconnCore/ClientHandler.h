/// ClientHandler.h — 클라이언트 전용 패킷 수신/커서 주입 기능
///
/// 역할:
///   - 클라이언트 화면 영역 조회 (ScreenBounds, get_screen_bounds)
///   - host가 보낸 EVENT_SYNC_CURSOR 처리 — 커서 초기 위치 설정 (client_handle_sync)
///   - host가 보낸 마우스 이동/클릭/스크롤 패킷 처리 (client_handle_move)
///   - host가 보낸 키보드 패킷 처리 (client_handle_key)
///   - 클라이언트 화면 정보를 host에 전송 (send_client_info)
///
/// 의존성:
///   - g_engine, g_client, g_auth_ok 는 bconn_core.cpp 에 정의된 전역을 extern으로 참조한다.
#pragma once
#include "include/bconn_core.h"
#include "protocol.h"
#include <CoreGraphics/CoreGraphics.h>

/// 클라이언트 전체 화면 영역 (멀티모니터 통합 경계)
struct ScreenBounds {
    double left, top, right, bottom;
    double width()  const { return right - left; }
    double height() const { return bottom - top; }
};

/// 현재 연결된 모든 디스플레이의 통합 경계를 반환한다.
ScreenBounds get_screen_bounds();

/// 클라이언트 커서 상태 (NetworkEngine recv 콜백에서 접근)
struct ClientState {
    ScreenBounds screen;
    double cur_x      = 0;
    double cur_y      = 0;
    bool pos_init      = false; // true이면 cur_x/cur_y가 유효함
    bool cursor_hidden = false;
    // host가 진입한 경계 방향(BconnEdge). 반대편 경계 이탈 판정에 쓴다.
    // 예: host가 RIGHT 로 진입 → 클라이언트 화면의 왼쪽 경계에 닿으면 복귀.
    int  entry_edge   = 0;
};

/// 클라이언트 화면 크기를 host에 전송한다.
/// EVENT_SYNC_CURSOR 수신 직후와 AUTH_ACK 수신 후 호출된다.
void send_client_info();

/// 모니터 연결/해제/배치 변경 시 클라이언트 화면 경계를 재계산한다.
/// 새 크기를 host 에 재전송하고, 원격 모드 중이었다면 좌표계가 무의미해지므로
/// 안전하게 로컬로 복원한다. 호출 스레드에서 커서를 직접 조작하므로
/// client 의 다른 커서 조작(recv 스레드)과 동일 스레드에서 호출해야 안전하다.
void client_apply_display_change();

/// host의 EVENT_SYNC_CURSOR 패킷을 처리한다.
/// 진입 비율을 자신의 화면에 매핑해 커서를 워프한다.
void client_handle_sync(const MousePacket& pkt);

/// host의 마우스 이동/클릭/스크롤 패킷을 처리한다.
/// kCGEventMouseMoved는 CGWarpMouseCursorPosition 선처리 후 CGEventPost한다.
void client_handle_move(const MousePacket& pkt);

/// host의 키보드 패킷을 CGEventPost로 주입한다.
void client_handle_key(const KeyPacket& pkt);
