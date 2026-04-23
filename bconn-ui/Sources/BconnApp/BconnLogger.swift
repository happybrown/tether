import Foundation
import BconnCore

struct LogEntry: Identifiable {
    let id = UUID()
    let date: Date
    let level: BconnLogLevel
    let category: String
    let message: String

    var levelLabel: String {
        switch level {
        case BCONN_LOG_DEBUG: return "DEBUG"
        case BCONN_LOG_INFO:  return "INFO"
        case BCONN_LOG_ERROR: return "ERROR"
        default:              return "???"
        }
    }
}

class BconnLogger: ObservableObject {
    static let shared = BconnLogger()

    @Published var entries: [LogEntry] = []
    @Published var minLevel: BconnLogLevel = BCONN_LOG_DEBUG
    @Published var fileLoggingEnabled = false {
        didSet { fileLoggingEnabled ? openLogFile() : closeLogFile() }
    }

    private let maxEntries = 2000
    private var fileHandle: FileHandle?
    private(set) var logFileURL: URL?

    private init() {
        // self를 unretained 포인터로 넘기고, C 콜백에서 메인 큐로 디스패치한다.
        // BconnLogger는 앱 생존 기간 내내 유지되므로 unretained 사용이 안전하다.
        let ptr = Unmanaged.passUnretained(self).toOpaque()
        bconn_set_log_callback({ level, cat, msg, ctx in
            guard let ctx, let cat, let msg else { return }
            let logger = Unmanaged<BconnLogger>.fromOpaque(ctx).takeUnretainedValue()
            let entry = LogEntry(
                date: Date(),
                level: level,
                category: String(cString: cat),
                message: String(cString: msg)
            )
            // C 콜백은 임의 스레드에서 호출되므로 메인 큐로 전달
            DispatchQueue.main.async { [weak logger] in
                logger?.append(entry)
            }
        }, ptr)
    }

    var filtered: [LogEntry] {
        entries.filter { $0.level.rawValue >= minLevel.rawValue }
    }

    func clear() {
        entries.removeAll()
    }

    private func append(_ entry: LogEntry) {
        entries.append(entry)
        if entries.count > maxEntries {
            entries.removeFirst(entries.count - maxEntries)
        }
        writeToFile(entry)
    }

    // ── 파일 출력 ─────────────────────────────────────────────────────

    private func openLogFile() {
        let dir = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Logs/Bconn", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)

        let fmt = DateFormatter()
        fmt.dateFormat = "yyyyMMdd_HHmmss"
        let url = dir.appendingPathComponent("bconn_\(fmt.string(from: Date())).log")
        FileManager.default.createFile(atPath: url.path, contents: nil)
        fileHandle = try? FileHandle(forWritingTo: url)
        logFileURL = url
    }

    private func closeLogFile() {
        fileHandle?.closeFile()
        fileHandle = nil
        logFileURL = nil
    }

    private func writeToFile(_ entry: LogEntry) {
        guard let fh = fileHandle else { return }
        let fmt = ISO8601DateFormatter()
        let line = "\(fmt.string(from: entry.date)) [\(entry.levelLabel)] [\(entry.category)] \(entry.message)\n"
        fh.write(Data(line.utf8))
    }
}
