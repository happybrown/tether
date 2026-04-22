import AppKit
import SwiftUI
import BconnCore

class AppDelegate: NSObject, NSApplicationDelegate {
    static weak var shared: AppDelegate!

    private var statusItem: NSStatusItem!
    private var popover: NSPopover!
    private var settingsWindow: NSWindow?

    func applicationDidFinishLaunching(_ notification: Notification) {
        AppDelegate.shared = self

        // Dock 아이콘 숨김 (menu bar 전용 앱)
        NSApp.setActivationPolicy(.accessory)

        setupStatusItem()
        setupPopover()

        // 설정이 완료된 상태이면 자동 시작
        let s = AppSettings.shared
        let ready = s.mode == .host || !s.hostIP.isEmpty
        if ready {
            BconnManager.shared.start()
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        BconnManager.shared.stop()
    }

    // ── Status Bar ────────────────────────────────────────────────────

    private func setupStatusItem() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)

        if let button = statusItem.button {
            button.image = NSImage(systemSymbolName: "display.2", accessibilityDescription: "bconn")
            button.action = #selector(togglePopover)
            button.target = self
        }

        NotificationCenter.default.addObserver(
            forName: .bconnStatusChanged, object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor [weak self] in self?.updateStatusIcon() }
        }
    }

    @objc private func togglePopover() {
        guard let button = statusItem.button else { return }
        if popover.isShown {
            popover.performClose(nil)
        } else {
            popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
            popover.contentViewController?.view.window?.makeKey()
        }
    }

    @MainActor private func updateStatusIcon() {
        let status = BconnManager.shared.status
        let name: String
        switch status {
        case .idle:      name = "display.2"
        case .waiting:   name = "display.2"
        case .connected: name = "display.2"
        case .remote:    name = "cursorarrow.rays"
        case .error:     name = "exclamationmark.triangle"
        default:         name = "display.2"
        }
        statusItem.button?.image = NSImage(systemSymbolName: name, accessibilityDescription: "bconn")
    }

    // ── Popover ───────────────────────────────────────────────────────

    private func setupPopover() {
        popover = NSPopover()
        popover.contentSize  = NSSize(width: 240, height: 260)
        popover.behavior     = .transient
        popover.animates     = false
        popover.contentViewController = NSHostingController(rootView: MenuBarView())
    }

    // ── Settings Window ───────────────────────────────────────────────

    func openSettings() {
        popover.performClose(nil)

        if let win = settingsWindow, win.isVisible {
            win.makeKeyAndOrderFront(nil)
            NSApp.activate(ignoringOtherApps: true)
            return
        }

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 480, height: 400),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered,
            defer: false
        )
        window.title = "bconn 설정"
        window.contentViewController = NSHostingController(rootView: SettingsView())
        window.center()
        window.isReleasedWhenClosed = false
        settingsWindow = window
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }
}

extension Notification.Name {
    static let bconnStatusChanged = Notification.Name("bconnStatusChanged")
}
