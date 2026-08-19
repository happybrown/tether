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
///     등은 tether_core.cpp 에 정의된 전역을 extern으로 참조한다.
#pragma once
#include "include/tether_core.h"
#include "protocol.h"
#include <CoreGraphics/CoreGraphics.h>
#include <atomic>
#include <chrono>
#include <functional>

/// 원격 모드 상태 (tether_core.cpp 에 인스턴스, EventTap.cpp 에서 접근)
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
    // 진입한 경계 모니터의 bounds. 진입/복귀 비율을 이 모니터 기준으로 계산해
    // 멀티모니터(세로 배치, 해상도 상이)에서도 클라이언트 화면과 일관되게 대응시킨다.
    CGRect entry_display = CGRectZero;
    int    entry_guard = 0;
    // 복귀 후 즉시 재진입 방지: 이벤트 카운트 대신 시간 기반으로 판단 (200ms)
    std::chrono::steady_clock::time_point return_time{};
};

/// 디스플레이 레이아웃을 경계(edge) 방향으로 초기화한다.
/// CGGetActiveDisplayList를 호출해 현재 연결된 모든 디스플레이를 탐색한다.
void refresh_layout(TetherEdge edge);

/// 연결/실행 중에 client 위치(edge)를 실시간으로 변경한다.
/// g_cfg.client_edge 를 갱신하고 레이아웃을 다시 계산한다. 원격 모드 중이면 먼저 복귀한다.
/// 레이아웃/커서 접근을 RunLoop 스레드로 일원화하기 위해 내부에서 dispatch 한다.
void apply_client_edge(TetherEdge edge);

/// 전환 기준 화면(디스플레이 UUID)을 실시간으로 변경한다. NULL/빈 문자열이면 자동
/// (전체 데스크톱 최외곽 경계). apply_client_edge 와 같은 방식으로 RunLoop 에 위임한다.
void apply_trigger_display(const char* uuid);

/// 기준 화면 UUID 를 보관한다 (RunLoop 스레드 전용, refresh_layout 이 참조).
void set_trigger_display_uuid(const char* uuid);

/// 마우스 이벤트 탭(CGEventTap)을 생성하고 RunLoop에 등록한다.
/// 이 함수는 g_run_loop_thread 내부에서 호출되어야 한다.
/// 실패 시 TETHER_STATUS_ERROR 로 상태를 변경한다.
void setup_event_taps();

/// 마우스/키보드 이벤트 탭을 비활성화하고 CFMachPortRef를 해제한다.
void teardown_event_taps();

/// 외부 스레드에서 원격 모드를 강제 종료한다.
/// RunLoop 스레드로 leave_remote 를 dispatch 하여 커서/포인터 연동을 복구한다.
/// RunLoop 이 없거나 정지 중이면 CGAssociateMouseAndMouseCursorPosition(true) 만 폴백 호출.
void force_leave_remote(const char* reason);

/// 클라이언트 LeavePacket 수신 시 호출한다 (recv 스레드). 원격 복귀의 정규 경로로,
/// ratio(0~1)를 복귀 좌표로 변환해 복귀한다.
void request_leave_remote(double ratio);

/// Cmd+Shift+Esc 세션 강제 중지 시 호출될 콜백을 등록한다.
/// CGEventTap 콜백(RunLoop 스레드)에서 호출되므로 내부에서 비동기 처리해야 한다.
void set_force_stop_cb(std::function<void()> cb);
