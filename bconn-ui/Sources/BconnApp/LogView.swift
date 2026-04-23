import SwiftUI
import BconnCore

struct LogView: View {
    @ObservedObject private var logger = BconnLogger.shared
    @State private var autoscroll = true

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            Divider()
            logList
        }
        .frame(minWidth: 700, minHeight: 400)
    }

    // ── 툴바 ────────────────────────────────────────────────────────

    private var toolbar: some View {
        HStack(spacing: 12) {
            Picker("레벨", selection: $logger.minLevel) {
                Text("DEBUG").tag(BCONN_LOG_DEBUG)
                Text("INFO").tag(BCONN_LOG_INFO)
                Text("ERROR").tag(BCONN_LOG_ERROR)
            }
            .pickerStyle(.segmented)
            .frame(width: 200)
            .labelsHidden()

            Toggle("파일 저장", isOn: $logger.fileLoggingEnabled)
                .toggleStyle(.checkbox)

            if let url = logger.logFileURL {
                Button {
                    NSWorkspace.shared.activateFileViewerSelecting([url])
                } label: {
                    Image(systemName: "folder")
                }
                .buttonStyle(.borderless)
                .help(url.path)
            }

            Spacer()

            Toggle("자동 스크롤", isOn: $autoscroll)
                .toggleStyle(.checkbox)

            Button("지우기") { logger.clear() }
                .buttonStyle(.borderless)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }

    // ── 로그 목록 ────────────────────────────────────────────────────

    private var logList: some View {
        ScrollViewReader { proxy in
            List(logger.filtered) { entry in
                LogRow(entry: entry)
                    .id(entry.id)
                    .listRowSeparator(.hidden)
                    .listRowInsets(.init(top: 1, leading: 8, bottom: 1, trailing: 8))
            }
            .listStyle(.plain)
            .font(.system(size: 12, design: .monospaced))
            .onChange(of: logger.filtered.count) { _ in
                if autoscroll, let last = logger.filtered.last {
                    proxy.scrollTo(last.id, anchor: .bottom)
                }
            }
        }
    }
}

private struct LogRow: View {
    let entry: LogEntry

    private static let timeFmt: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Text(Self.timeFmt.string(from: entry.date))
                .foregroundStyle(.secondary)
                .frame(width: 90, alignment: .leading)

            Text(entry.levelLabel)
                .foregroundStyle(levelColor)
                .frame(width: 44, alignment: .leading)

            Text("[\(entry.category)]")
                .foregroundStyle(.secondary)
                .frame(width: 70, alignment: .leading)

            Text(entry.message)
                .foregroundStyle(.primary)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var levelColor: Color {
        switch entry.level {
        case BCONN_LOG_ERROR: return .red
        case BCONN_LOG_INFO:  return .primary
        default:              return .secondary
        }
    }
}
