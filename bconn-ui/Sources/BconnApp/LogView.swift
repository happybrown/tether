import SwiftUI
import AppKit
import BconnCore

struct LogView: View {
    @ObservedObject private var logger = BconnLogger.shared

    @State private var search = ""
    @State private var autoScroll = true
    @State private var copiedAll = false

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            Divider()
            logList
            Divider()
            statusBar
        }
        .frame(minWidth: 560, minHeight: 320)
    }

    // ── 툴바 ──────────────────────────────────────────────────────────

    private var toolbar: some View {
        HStack(spacing: 10) {
            Picker("레벨", selection: $logger.minLevel) {
                Text("전체").tag(BCONN_LOG_DEBUG)
                Text("INFO").tag(BCONN_LOG_INFO)
                Text("ERROR").tag(BCONN_LOG_ERROR)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .frame(width: 180)

            searchField

            Spacer(minLength: 8)

            Toggle(isOn: $autoScroll) {
                Label("자동 스크롤", systemImage: "arrow.down.to.line")
            }
            .toggleStyle(.button)
            .controlSize(.small)
            .help("새 로그가 오면 맨 아래로 따라갑니다")

            Toggle(isOn: $logger.fileLoggingEnabled) {
                Label("파일 저장", systemImage: "square.and.arrow.down")
            }
            .toggleStyle(.button)
            .controlSize(.small)
            .help("로그를 ~/Library/Logs/Bconn 에 저장합니다")

            Menu {
                Button("전체 복사") { copyAll() }
                Button("로그 폴더 열기") { revealLogFolder() }
                    .disabled(logger.logFileURL == nil)
                Divider()
                Button("지우기", role: .destructive) { logger.clear() }
            } label: {
                Image(systemName: "ellipsis.circle")
            }
            .menuStyle(.borderlessButton)
            .frame(width: 28)
            .help("더보기")
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }

    private var searchField: some View {
        HStack(spacing: 5) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
            TextField("검색", text: $search)
                .textFieldStyle(.plain)
                .font(.system(size: 12))
            if !search.isEmpty {
                Button { search = "" } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(Color.primary.opacity(0.06))
        .clipShape(RoundedRectangle(cornerRadius: 6))
        .frame(maxWidth: 240)
    }

    // ── 로그 목록 ──────────────────────────────────────────────────────

    private var visibleEntries: [LogEntry] {
        let base = logger.filtered
        let q = search.trimmingCharacters(in: .whitespaces).lowercased()
        guard !q.isEmpty else { return base }
        return base.filter {
            $0.message.lowercased().contains(q) || $0.category.lowercased().contains(q)
        }
    }

    private var logList: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    ForEach(Array(visibleEntries.enumerated()), id: \.element.id) { idx, entry in
                        LogRow(entry: entry, zebra: idx % 2 == 1)
                            .id(entry.id)
                    }
                }
            }
            .background(Color(nsColor: .textBackgroundColor))
            .overlay(alignment: .center) {
                if visibleEntries.isEmpty { emptyState }
            }
            .onChange(of: logger.entries.count) { _ in
                guard autoScroll, let last = visibleEntries.last else { return }
                withAnimation(.linear(duration: 0.1)) {
                    proxy.scrollTo(last.id, anchor: .bottom)
                }
            }
        }
    }

    private var emptyState: some View {
        VStack(spacing: 6) {
            Image(systemName: search.isEmpty ? "text.append" : "magnifyingglass")
                .font(.system(size: 28))
                .foregroundStyle(.tertiary)
            Text(search.isEmpty ? "로그가 없습니다" : "검색 결과 없음")
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
        }
    }

    // ── 하단 상태바 ────────────────────────────────────────────────────

    private var statusBar: some View {
        HStack(spacing: 8) {
            Text("\(visibleEntries.count)개")
                .font(.system(size: 11, design: .monospaced))
                .foregroundStyle(.secondary)
            if !search.isEmpty || logger.minLevel != BCONN_LOG_DEBUG {
                Text("· 필터됨").font(.system(size: 11)).foregroundStyle(.tertiary)
            }
            Spacer()
            if copiedAll {
                Label("복사됨", systemImage: "checkmark")
                    .font(.system(size: 11))
                    .foregroundStyle(.green)
            }
            if logger.fileLoggingEnabled, let url = logger.logFileURL {
                Button { revealLogFolder() } label: {
                    Label(url.lastPathComponent, systemImage: "doc")
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                .buttonStyle(.plain)
                .help("로그 파일 위치 열기")
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
    }

    // ── 액션 ──────────────────────────────────────────────────────────

    private func copyAll() {
        let fmt = LogRow.timeFormatter
        let text = visibleEntries.map {
            "\(fmt.string(from: $0.date)) [\($0.levelLabel)] [\($0.category)] \($0.message)"
        }.joined(separator: "\n")
        let pb = NSPasteboard.general
        pb.clearContents()
        pb.setString(text, forType: .string)
        copiedAll = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) { copiedAll = false }
    }

    private func revealLogFolder() {
        let dir = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Logs/Bconn", isDirectory: true)
        NSWorkspace.shared.open(dir)
    }
}

// ── 로그 행 ────────────────────────────────────────────────────────────

private struct LogRow: View {
    let entry: LogEntry
    let zebra: Bool

    static let timeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Text(Self.timeFormatter.string(from: entry.date))
                .font(.system(size: 10, design: .monospaced))
                .foregroundStyle(.secondary)
                .frame(width: 78, alignment: .leading)

            Text(entry.levelLabel)
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundStyle(levelColor)
                .frame(width: 42, alignment: .leading)

            Text(entry.category)
                .font(.system(size: 10, design: .monospaced))
                .foregroundStyle(.secondary)
                .frame(width: 60, alignment: .leading)

            Text(entry.message)
                .font(.system(size: 11, design: .monospaced))
                .foregroundStyle(.primary)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 3)
        .background(zebra ? Color.primary.opacity(0.035) : Color.clear)
    }

    private var levelColor: Color {
        switch entry.level {
        case BCONN_LOG_DEBUG: return .secondary
        case BCONN_LOG_INFO:  return .blue
        case BCONN_LOG_ERROR: return .red
        default:              return .primary
        }
    }
}
