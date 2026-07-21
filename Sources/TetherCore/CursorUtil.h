#pragma once
#include <ApplicationServices/ApplicationServices.h>

/// 커서 표시/숨김 유틸리티.
/// CGDisplayHideCursor/ShowCursor 를 쓴다 — NSCursor.hide 와 달리 앱 활성·창 여부와
/// 무관하게 시스템 레벨에서 커서를 숨기며, 물리 마우스가 움직여도 숨김이 유지된다.
/// (메뉴바 앱은 창이 없어 NSCursor.hide 로는 원격 화면에 커서가 되살아난다.)
/// hide/show 는 내부 카운터 기반이라 반드시 짝을 맞춰 호출해야 한다.
/// 호출부(g_client.cursor_hidden 등)가 중복 호출을 막아 카운터 짝을 보장한다.

static inline void tether_hide_cursor() {
    CGDisplayHideCursor(kCGDirectMainDisplay);
}

static inline void tether_show_cursor() {
    CGDisplayShowCursor(kCGDirectMainDisplay);
}
