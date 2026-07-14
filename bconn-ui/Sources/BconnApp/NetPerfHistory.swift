import Foundation

/// 네트워크 성능 측정 이력을 최근 N개까지 누적·영속한다 (UserDefaults, JSON).
/// 차트가 관찰하도록 ObservableObject 로 노출한다.
final class NetPerfHistory: ObservableObject {
    static let shared = NetPerfHistory()

    /// 차트에 쓸 최근 측정들 (오래된 것 → 최신 순).
    @Published private(set) var entries: [NetPerfResult] = []

    /// 유지할 최대 개수. 초과분은 오래된 것부터 버린다.
    static let maxEntries = 30

    private let key = "netPerfHistory"
    private let defaults = UserDefaults.standard

    private init() { load() }

    /// 응답이 있는(유효한) 측정만 이력에 추가한다. 미연결/무응답 측정은 추이를 왜곡하므로 제외.
    func add(_ result: NetPerfResult) {
        guard result.hasData else { return }
        entries.append(result)
        if entries.count > Self.maxEntries {
            entries.removeFirst(entries.count - Self.maxEntries)
        }
        save()
    }

    func clear() {
        entries.removeAll()
        save()
    }

    // ── 영속 ──────────────────────────────────────────────────────────

    private func load() {
        guard let data = defaults.data(forKey: key),
              let decoded = try? JSONDecoder().decode([NetPerfResult].self, from: data)
        else { return }
        entries = decoded
    }

    private func save() {
        guard let data = try? JSONEncoder().encode(entries) else { return }
        defaults.set(data, forKey: key)
    }
}
