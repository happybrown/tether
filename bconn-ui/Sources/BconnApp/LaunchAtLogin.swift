import ServiceManagement

enum LaunchAtLogin {
    static func setEnabled(_ enabled: Bool) {
        if #available(macOS 13.0, *) {
            do {
                if enabled {
                    try SMAppService.mainApp.register()
                } else {
                    try SMAppService.mainApp.unregister()
                }
            } catch {
                // 권한 없거나 샌드박스 미지원 시 무시
            }
        }
    }
}
