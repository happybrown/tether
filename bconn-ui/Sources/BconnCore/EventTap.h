/// EventTap.h — 호스트 전용 CGEventTap 관련 기능
///
/// 역할:
///   - 디스플레이 레이아웃 계산 (DisplayLayout, refresh_layout)
///   - 원격 모드 진입/복귀 상태 관리 (RemoteState, enter_remote, leave_remote)
///   - CGEventTap 콜백 (mouse_callback, keyboard_callback)
///   - RunLoop 기반 이벤트 탭 설정/해제 (setup_event_taps, teardown_event_taps)
///   - 디스플레이 재설정 감지 (핫플러그 대응)
///
/// 의존성:
///   - g_engine, g_cfg, g_remote, g_client_width, g_client_height, set_status
///     등은 bconn_core.cpp 에 정의된 전역을 extern으로 참조한다.
#pragma once
#include "include/bconn_core.h"
#include "protocol.h"
#include <CoreGraphics/CoreGraphics.h>
#include <atomic>
#include <chrono>
#include <functional>

/// 원격 모드 상태 (bconn_core.cpp 에 인스턴스, EventTap.cpp 에서 접근)
/// active 만 atomic — 외부 스레드(watchdog, signal, kill switch)에서 조회/복구 가능.
/// 나머지 필드는 RunLoop 스레드의 mouse_callback 에서만 touch 한다.
struct RemoteState {
    std::atomic<bool> active{false};
    double entry_x   = 0.0;
    double entry_y   = 0.0;
    // 복귀 좌표. 클라이언트가 보낸 복귀 비율로 계산한다.
    // 진입 지점이 아니라 클라이언트에서 실제로 경계를 넘은 지점에 대응한다.
    double return_x  = 0.0;
    double return_y  = 0.0;
    // 클라이언트 커서의 가상 위치. host 가 원격 중 delta 를 client 와 동일하게 누적/clamp 해
    // 반대편 경계 도달을 host 가 직접 예측한다 → 복귀 패킷 왕복 지연 없이 즉시 복귀.
    double client_x  = 0.0;
    double client_y  = 0.0;
    // 진입한 경계 모니터의 bounds. 진입/복귀 비율을 이 모니터 기준으로 계산해
    // 멀티모니터(세로 배치, 해상도 상이)에서도 클라이언트 화면과 일관되게 대응시킨다.
    CGRect entry_display = CGRectZero;
    int    entry_guard = 0;
    // 복귀 후 즉시 재진입 방지: 이벤트 카운트 대신 시간 기반으로 판단 (200ms)
    std::chrono::steady_clock::time_point return_time{};
};

/// 디스플레이 레이아웃을 경계(edge) 방향으로 초기화한다.
/// CGGetActiveDisplayList를 호출해 현재 연결된 모든 디스플레이를 탐색한다.
void refresh_layout(BconnEdge edge);

/// 마우스 이벤트 탭(CGEventTap)을 생성하고 RunLoop에 등록한다.
/// 이 함수는 g_run_loop_thread 내부에서 호출되어야 한다.
/// 실패 시 BCONN_STATUS_ERROR 로 상태를 변경한다.
void setup_event_taps();

/// 마우스/키보드 이벤트 탭을 비활성화하고 CFMachPortRef를 해제한다.
void teardown_event_taps();

/// 외부 스레드에서 원격 모드를 강제 종료한다.
/// RunLoop 스레드로 leave_remote 를 dispatch 하여 커서/포인터 연동을 복구한다.
/// RunLoop 이 없거나 정지 중이면 CGAssociateMouseAndMouseCursorPosition(true) 만 폴백 호출.
void force_leave_remote(const char* reason);

/// 클라이언트 LeavePacket 수신 시 호출한다 (recv 스레드). host 자체 예측이 주 경로이며,
/// 이 경로는 그것이 놓친 경우의 안전망이다. ratio(0~1)를 복귀 좌표로 변환해 복귀한다.
void request_leave_remote(double ratio);

/// Cmd+Shift+Esc 세션 강제 중지 시 호출될 콜백을 등록한다.
/// CGEventTap 콜백(RunLoop 스레드)에서 호출되므로 내부에서 비동기 처리해야 한다.
void set_force_stop_cb(std::function<void()> cb);
