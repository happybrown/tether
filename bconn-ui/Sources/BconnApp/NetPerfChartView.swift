import SwiftUI
import Charts

/// 최근 측정들의 RTT 추이 차트. 평균을 선으로, 최소~최대를 옅은 밴드로 겹쳐
/// 지연과 지터(변동폭)를 한눈에 보여준다. 단일 시리즈라 범례 없이 제목이 명명한다.
struct NetPerfChartView: View {
    let entries: [NetPerfResult]

    // dataviz blue(single hue). 라이트/다크 표면 대비를 각각 만족하는 스텝.
    private var lineColor: Color {
        Color(light: Color(hex: 0x2a78d6), dark: Color(hex: 0x3987e5))
    }
    private var bandColor: Color {
        Color(light: Color(hex: 0x2a78d6), dark: Color(hex: 0x3987e5)).opacity(0.14)
    }

    // x축은 측정 순서(1-based). 시간 간격이 불규칙하므로 순번으로 균등 배치한다.
    private var indexed: [(idx: Int, r: NetPerfResult)] {
        entries.enumerated().map { ($0.offset + 1, $0.element) }
    }

    var body: some View {
        Chart {
            ForEach(indexed, id: \.r.id) { item in
                // 최소~최대 밴드
                AreaMark(
                    x: .value("측정", item.idx),
                    yStart: .value("최소", item.r.minMs),
                    yEnd: .value("최대", item.r.maxMs)
                )
                .foregroundStyle(bandColor)
                .interpolationMethod(.monotone)
            }

            ForEach(indexed, id: \.r.id) { item in
                // 평균 RTT 선
                LineMark(
                    x: .value("측정", item.idx),
                    y: .value("평균 RTT", item.r.avgMs)
                )
                .foregroundStyle(lineColor)
                .lineStyle(StrokeStyle(lineWidth: 2))
                .interpolationMethod(.monotone)
            }

            // 최신 값만 점 + 직접 라벨 (모든 점에 라벨 금지 원칙)
            if let last = indexed.last {
                PointMark(
                    x: .value("측정", last.idx),
                    y: .value("평균 RTT", last.r.avgMs)
                )
                .foregroundStyle(lineColor)
                .symbolSize(60)
                .annotation(position: .top, spacing: 4) {
                    Text(String(format: "%.0f ms", last.r.avgMs))
                        .font(.system(size: 10, weight: .semibold, design: .monospaced))
                        .foregroundStyle(.primary)
                }
            }
        }
        .chartXAxis {
            AxisMarks(values: .automatic(desiredCount: 5)) {
                AxisGridLine().foregroundStyle(Color.primary.opacity(0.06))
                AxisTick().foregroundStyle(Color.primary.opacity(0.15))
                AxisValueLabel().font(.system(size: 9))
            }
        }
        .chartYAxis {
            AxisMarks(position: .leading) { value in
                AxisGridLine().foregroundStyle(Color.primary.opacity(0.06))
                AxisValueLabel {
                    if let ms = value.as(Double.self) {
                        Text("\(Int(ms))")
                            .font(.system(size: 9))
                    }
                }
            }
        }
        .chartYScale(domain: 0...yMax)
        .frame(height: 160)
    }

    // Y축 상한: 최대값에 10% 여유. 데이터 없으면 안전한 기본값.
    private var yMax: Double {
        let peak = entries.map(\.maxMs).max() ?? 50
        return max(peak * 1.1, 10)
    }
}

// ── 색상 유틸 ──────────────────────────────────────────────────────────

extension Color {
    /// 0xRRGGBB 정수에서 Color 생성.
    init(hex: UInt32) {
        self.init(
            .sRGB,
            red:   Double((hex >> 16) & 0xFF) / 255.0,
            green: Double((hex >> 8) & 0xFF) / 255.0,
            blue:  Double(hex & 0xFF) / 255.0
        )
    }

    /// 라이트/다크 모드에 따라 다른 색을 쓴다 (차트 표면 대비 유지).
    init(light: Color, dark: Color) {
        self.init(nsColor: NSColor(name: nil) { appearance in
            let isDark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            return NSColor(isDark ? dark : light)
        })
    }
}
