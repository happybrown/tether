#include "NetworkEngine.h"
#include <ApplicationServices/ApplicationServices.h>
#include <objc/objc-runtime.h>
#include <iostream>
#include <string>
#include <algorithm>
#include <atomic>

#define EVENT_SYNC_CURSOR 9999
#define EVENT_SHOW_CURSOR 9998
// EVENT_CLIENT_INFO 9996 은 NetworkEngine.h 에 정의됨

bool is_remote_mode = false;
double global_right_edge  = 0.0;
double global_top         = 0.0;
double global_bottom      = 0.0;
double host_screen_height = 0.0;

double entry_y   = 0.0;
double virtual_x = 0.0;
int entry_guard = 0;
const int ENTRY_GUARD_COUNT = 5;

// 복귀 직후 재진입 방지 가드
int return_guard = 0;
const int RETURN_GUARD_COUNT = 10;

std::atomic<int32_t> client_screen_width{32000};

// NSCursor hide/unhide를 Objective-C 런타임으로 직접 호출
// CGDisplayHideCursor는 CLI 프로세스에서 무시되는 경우가 있음
void hide_cursor() {
    id cls = (id)objc_getClass("NSCursor");
    SEL sel = sel_registerName("hide");
    ((void(*)(id,SEL))objc_msgSend)(cls, sel);
}

void show_cursor() {
    id cls = (id)objc_getClass("NSCursor");
    SEL sel = sel_registerName("unhide");
    ((void(*)(id,SEL))objc_msgSend)(cls, sel);
}

void print_display_info() {
    CGDirectDisplayID displays[16];
    uint32_t count = 0;
    CGGetActiveDisplayList(16, displays, &count);

    std::cout << "=== 모니터 정보 (" << count << "개) ===" << std::endl;
    for (uint32_t i = 0; i < count; ++i) {
        CGRect bounds  = CGDisplayBounds(displays[i]);
        CGSize size_mm = CGDisplayScreenSize(displays[i]);
        bool is_main   = (displays[i] == CGMainDisplayID());
        std::cout << "  [" << i << "] "
                  << (is_main ? "(main) " : "       ")
                  << "origin=(" << (int)bounds.origin.x << "," << (int)bounds.origin.y << ")"
                  << "  size=" << (int)bounds.size.width << "x" << (int)bounds.size.height
                  << "  physical=" << (int)size_mm.width << "x" << (int)size_mm.height << "mm"
                  << std::endl;
    }
    std::cout << "=========================" << std::endl;
}

void update_global_bounds() {
    CGDirectDisplayID displays[16];
    uint32_t count = 0;
    CGGetActiveDisplayList(16, displays, &count);

    double max_x = -999999.0, min_y = 999999.0, max_y = -999999.0;
    for (uint32_t i = 0; i < count; ++i) {
        CGRect bounds = CGDisplayBounds(displays[i]);
        double right  = bounds.origin.x + bounds.size.width;
        double top    = bounds.origin.y;
        double bottom = bounds.origin.y + bounds.size.height;
        if (right  > max_x) max_x = right;
        if (top    < min_y) min_y = top;
        if (bottom > max_y) max_y = bottom;
    }
    global_right_edge  = max_x;
    global_top         = min_y;
    global_bottom      = max_y;
    host_screen_height = max_y - min_y;
}

bool is_at_right_edge(CGPoint loc) {
    if (loc.x < global_right_edge - 3.0) return false;

    CGDirectDisplayID displays[16];
    uint32_t count = 0;
    CGGetActiveDisplayList(16, displays, &count);

    for (uint32_t i = 0; i < count; ++i) {
        CGRect bounds = CGDisplayBounds(displays[i]);
        double right  = bounds.origin.x + bounds.size.width;
        if (right < global_right_edge - 1.0) continue;
        if (loc.y >= bounds.origin.y && loc.y <= bounds.origin.y + bounds.size.height)
            return true;
    }
    return false;
}

CGEventRef mouse_callback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void* userInfo) {
    NetworkEngine* engine = static_cast<NetworkEngine*>(userInfo);

    if (!is_remote_mode) {
        CGPoint location = CGEventGetLocation(event);

        // 마우스 위치 디버그 (50ms 간격)
        static double last_log_time = 0;
        double now = CFAbsoluteTimeGetCurrent();
        if (now - last_log_time > 0.05) {
            std::cout << "[mouse] x=" << (int)location.x << " y=" << (int)location.y
                      << "  right_edge=" << (int)global_right_edge << std::endl;
            last_log_time = now;
        }

        if (return_guard > 0) {
            --return_guard;
            return event;
        }

        if (is_at_right_edge(location)) {
            is_remote_mode = true;
            virtual_x = 0.0;
            entry_y = location.y;
            entry_guard = ENTRY_GUARD_COUNT;

            std::cout << "[원격 모드 진입] x=" << (int)location.x
                      << " y=" << (int)entry_y
                      << " client_width=" << (int)client_screen_width.load() << std::endl;

            hide_cursor();
            CGAssociateMouseAndMouseCursorPosition(false);
            // 커서를 화면 밖으로 날림 — 분리 상태라 이후 delta에 영향 없음
            CGWarpMouseCursorPosition(CGPointMake(global_right_edge, 99999.0));

            MousePacket sync_packet{};
            sync_packet.event_type = EVENT_SYNC_CURSOR;
            double y_ratio = (host_screen_height > 0)
                ? ((entry_y - global_top) / host_screen_height) : 0.5;
            if (y_ratio < 0.0) y_ratio = 0.0;
            if (y_ratio > 1.0) y_ratio = 1.0;
            sync_packet.dy = static_cast<int16_t>(y_ratio * 32767.0);
            engine->push_event(sync_packet);

            return NULL;
        }
    } else {
        int64_t delta_x = CGEventGetIntegerValueField(event, kCGMouseEventDeltaX);
        int64_t delta_y = CGEventGetIntegerValueField(event, kCGMouseEventDeltaY);

        bool is_click = (type == kCGEventLeftMouseDown  || type == kCGEventLeftMouseUp ||
                         type == kCGEventRightMouseDown || type == kCGEventRightMouseUp);

        // 원격 모드 중 커서가 화면에 다시 나타나지 않도록 매 이벤트마다 밀어냄
        CGWarpMouseCursorPosition(CGPointMake(global_right_edge, 99999.0));

        if (!is_click) {
            if (entry_guard > 0) {
                --entry_guard;
                return NULL;
            }
            virtual_x += delta_x;
            double max_vx = (double)client_screen_width.load(std::memory_order_relaxed);
            if (virtual_x > max_vx) virtual_x = max_vx;
        }

        // 로컬 복귀: virtual_x가 0 이하가 되는 순간 바로 전환
        if (virtual_x <= 0.0) {
            is_remote_mode = false;
            std::cout << "[로컬 모드 복귀] virtual_x=" << (int)virtual_x << std::endl;

            MousePacket show_packet{};
            show_packet.event_type = EVENT_SHOW_CURSOR;
            engine->push_event(show_packet);

            // 경계에서 충분히 안쪽에 커서 배치 (너무 가까우면 즉시 재진입 루프 발생)
            CGWarpMouseCursorPosition(CGPointMake(global_right_edge - 50.0, entry_y));
            CGAssociateMouseAndMouseCursorPosition(true);
            show_cursor();
            return_guard = RETURN_GUARD_COUNT;

            return NULL;
        }

        MousePacket packet{};
        packet.dx = static_cast<int16_t>(std::max(-32768LL, std::min((int64_t)32767, delta_x)));
        packet.dy = static_cast<int16_t>(std::max(-32768LL, std::min((int64_t)32767, delta_y)));
        packet.event_type = type;

        engine->push_event(packet);
        return NULL;
    }

    return event;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "사용법: ./host_app <client_ip> <port>" << std::endl;
        return 1;
    }

    print_display_info();
    update_global_bounds();
    std::cout << "[경계] right_edge=" << (int)global_right_edge
              << " top=" << (int)global_top
              << " bottom=" << (int)global_bottom
              << " height=" << (int)host_screen_height << std::endl;

    auto on_client_packet = [](const MousePacket& pkt) {
        if (pkt.event_type == EVENT_CLIENT_INFO) {
            int32_t w = ((int32_t)(uint16_t)pkt.dy << 16) | (uint16_t)pkt.dx;
            client_screen_width.store(w, std::memory_order_relaxed);
            std::cout << "[← client] 화면 너비 수신: " << w << "px" << std::endl;
        }
    };

    int port = std::stoi(argv[2]);
    NetworkEngine engine(argv[1], port, on_client_packet);
    engine.start();

    CGEventMask event_mask = CGEventMaskBit(kCGEventMouseMoved) |
                             CGEventMaskBit(kCGEventLeftMouseDragged) |
                             CGEventMaskBit(kCGEventRightMouseDragged) |
                             CGEventMaskBit(kCGEventLeftMouseDown) |
                             CGEventMaskBit(kCGEventLeftMouseUp) |
                             CGEventMaskBit(kCGEventRightMouseDown) |
                             CGEventMaskBit(kCGEventRightMouseUp);

    CFMachPortRef event_tap = CGEventTapCreate(
        kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
        event_mask, mouse_callback, &engine
    );

    if (!event_tap) {
        std::cerr << "오류: Event Tap 권한 확인 필요 (손쉬운 사용 권한)" << std::endl;
        exit(1);
    }

    CFRunLoopSourceRef run_loop_source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, event_tap, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), run_loop_source, kCFRunLoopCommonModes);
    CGEventTapEnable(event_tap, true);

    std::cout << "Host 대기 중... (우측 끝으로 마우스를 이동하면 클라이언트로 전환)" << std::endl;
    CFRunLoopRun();

    // 종료 시 안전장치
    CGAssociateMouseAndMouseCursorPosition(true);
    show_cursor();
    engine.stop();
    return 0;
}
