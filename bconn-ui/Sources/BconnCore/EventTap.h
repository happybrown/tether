/// EventTap.h — 호스트 전용 CGEventTap 관련 기능
///
/// 역할:
///   - 디스플레이 레이아웃 계산 (DisplayLayout, refresh_layout)
///   - 원격 모드 진입/복귀 상태 관리 (RemoteState, enter_remote, leave_remote)
///   - CGEventTap 콜백 (mouse_callback, keyboard_callback)
///   - RunLoop 기반 이벤트 탭 설정/해제 (setup_event_taps, teardown_event_taps)
///
/// 의존성:
///   - g_engine, g_cfg, g_remote, g_client_width, g_client_height, set_status
///     등은 bconn_core.cpp 에 정의된 전역을 extern으로 참조한다.
#pragma once
#include "include/bconn_core.h"
#include "protocol.h"
#include <atomic>

/// 원격 모드 상태 (bconn_core.cpp 에 인스턴스, EventTap.cpp 에서 접근)
/// active 만 atomic — 외부 스레드(watchdog, signal, kill switch)에서 조회/복구 가능.
/// 나머지 필드는 RunLoop 스레드의 mouse_callback 에서만 touch 한다.
struct RemoteState {
    std::atomic<bool> active{false};
    double virtual_x    = 0.0;
    double virtual_y    = 0.0;
    double entry_x      = 0.0;
    double entry_y      = 0.0;
    int    entry_guard  = 0;
    int    return_guard = 0;
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
