import SwiftUI
import BconnCore

struct LogView: View {
    @ObservedObject private var logger = BconnLogger.shared
    @State private var autoscroll = true
    @State private var searchText = ""

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            Divider()
            logList
        }
        .frame(minWidth: 720, minHeight: 420)
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

            searchField
                .frame(maxWidth: 240)

            Toggle("파일 저장", isOn: $logger.fileLoggingEnabled)
                .toggleStyle(.checkbox)
                .help("활성화하면 이후 로그를 디스크에 기록합니다")

            if let url = logger.logFileURL {
                Button {
                    NSWorkspace.shared.activateFileViewerSelecting([url])
                } label: {
                    Image(systemName: "folder")
                }
                .buttonStyle(.borderless)
                .help("로그 파일: \(url.path)")
            }

            Spacer()

            Button {
                copyVisibleLogs()
            } label: {
                Image(systemName: "doc.on.doc")
            }
            .buttonStyle(.borderless)
            .help("표시된 로그를 클립보드에 복사")

            Toggle("자동 스크롤", isOn: $autoscroll)
                .toggleStyle(.checkbox)

            Button("지우기") { logger.clear() }
                .buttonStyle(.borderless)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }

    private var searchField: some View {
        HStack(spacing: 6) {
            Image(systemName: "magnifyingglass")
                .foregroundStyle(.secondary)
                .font(.system(size: 11))
            TextField("메시지 검색", text: $searchText)
                .textFieldStyle(.plain)
                .font(.system(size: 12))
            if !searchText.isEmpty {
                Button {
                    searchText = ""
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .foregroundStyle(.secondary)
                        .font(.system(size: 11))
                }
                .buttonStyle(.plain)
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(Color.gray.opacity(0.12))
        .clipShape(RoundedRectangle(cornerRadius: 6))
    }

    // ── 필터링 ──────────────────────────────────────────────────────

    private var visibleEntries: [LogEntry] {
        let base = logger.filtered
        guard !searchText.isEmpty else { return base }
        let needle = searchText
        return base.filter {
            $0.message.localizedCaseInsensitiveContains(needle) ||
            $0.category.localizedCaseInsensitiveContains(needle)
        }
    }

    // ── 로그 목록 ────────────────────────────────────────────────────

    private var logList: some View {
        ScrollViewReader { proxy in
            Group {
                if visibleEntries.isEmpty {
                    emptyState
                } else {
                    List(visibleEntries) { entry in
                        LogRow(entry: entry)
                            .id(entry.id)
                            .listRowSeparator(.hidden)
                            .listRowInsets(.init(top: 1, leading: 8, bottom: 1, trailing: 8))
                    }
                    .listStyle(.plain)
                    .font(.system(size: 12, design: .monospaced))
                    .onChange(of: visibleEntries.count) { _ in
                        if autoscroll, let last = visibleEntries.last {
                            proxy.scrollTo(last.id, anchor: .bottom)
                        }
                    }
                }
            }
        }
    }

    private var emptyState: some View {
        VStack(spacing: 8) {
            Image(systemName: searchText.isEmpty ? "tray" : "magnifyingglass")
                .font(.system(size: 28))
                .foregroundStyle(.secondary)
            Text(searchText.isEmpty ? "로그 없음" : "검색 결과 없음")
                .font(.system(size: 13))
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func copyVisibleLogs() {
        let fmt = ISO8601DateFormatter()
        let text = visibleEntries.map { entry in
            "\(fmt.string(from: entry.date)) [\(entry.levelLabel)] [\(entry.category)] \(entry.message)"
        }.joined(separator: "\n")

        let pb = NSPasteboard.general
        pb.clearContents()
        pb.setString(text, forType: .string)
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
