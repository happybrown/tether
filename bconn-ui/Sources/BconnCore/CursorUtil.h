#pragma once
#include <objc/objc-runtime.h>

/// 커서 표시/숨김 유틸리티.
/// NSCursor의 hide/unhide를 objc_msgSend로 호출한다.
/// 여러 파일에서 중복 정의되던 것을 하나로 통합.

static inline void bconn_hide_cursor() {
    id cls = (id)objc_getClass("NSCursor");
    ((void(*)(id, SEL))objc_msgSend)(cls, sel_registerName("hide"));
}

static inline void bconn_show_cursor() {
    id cls = (id)objc_getClass("NSCursor");
    ((void(*)(id, SEL))objc_msgSend)(cls, sel_registerName("unhide"));
}
