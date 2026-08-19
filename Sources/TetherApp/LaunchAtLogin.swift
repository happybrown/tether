import ServiceManagement

/// 로그인 시 자동 시작을 제어하는 유틸리티.
/// macOS 13+ SMAppService API를 사용한다.
enum LaunchAtLogin {
    static func setEnabled(_ enabled: Bool) {
        let service = SMAppService.mainApp
        do {
            if enabled {
                try service.register()
            } else {
                try service.unregister()
            }
        } catch {
            // 실패해도 앱 동작에 영향 없음 — 로그만 남긴다
            NSLog("[LaunchAtLogin] %@ failed: %@",
                  enabled ? "register" : "unregister", error.localizedDescription)
        }
    }
}
