import AppKit
import CoreGraphics

// tether 앱 아이콘 렌더러 (추상·모던 기호형).
// 컨셉: 두 노드가 흐름(링크)으로 연결 — "두 Mac 사이 입력 공유".
// 다크 프리미엄 배경 위에 네온 글로우 심볼. 1024pt 기준.

let BASE: CGFloat = 1024
let CS = CGColorSpaceCreateDeviceRGB()

func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> CGColor {
    CGColor(colorSpace: CS, components: [r, g, b, a])!
}
func alpha(_ c: CGColor, _ a: CGFloat) -> CGColor {
    let p = c.components ?? [1,1,1,1]; return rgb(p[0], p[1], p[2], a)
}
func grad(_ stops: [(CGFloat, CGColor)]) -> CGGradient {
    CGGradient(colorsSpace: CS, colors: stops.map{$0.1} as CFArray, locations: stops.map{$0.0})!
}

func makeContext(_ size: Int) -> CGContext {
    let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8,
                        bytesPerRow: 0, space: CS,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    let s = CGFloat(size)/BASE
    ctx.scaleBy(x: s, y: s)
    ctx.setAllowsAntialiasing(true)
    ctx.interpolationQuality = .high
    return ctx
}

func iconRect() -> CGRect {
    let i = BASE * 0.085
    return CGRect(x: i, y: i, width: BASE - 2*i, height: BASE - 2*i)
}
func roundedPath(_ r: CGRect, _ rad: CGFloat) -> CGPath {
    CGPath(roundedRect: r, cornerWidth: rad, cornerHeight: rad, transform: nil)
}

// 브랜드 액센트
let ACCENT = rgb(0.36, 0.66, 1.00)     // 시안-블루
let ACCENT2 = rgb(0.62, 0.44, 1.00)    // 바이올렛

// ── 배경: 딥 네이비 그라디언트 + 미세 광원/비네팅 ──
func drawBackground(_ ctx: CGContext) {
    let rect = iconRect()
    let rad = rect.width * 0.2237
    let path = roundedPath(rect, rad)

    ctx.saveGState(); ctx.addPath(path); ctx.clip()

    // 딥 다크 베이스 (상단 약간 밝은 네이비 → 하단 거의 블랙)
    let base = grad([
        (0.0, rgb(0.10, 0.13, 0.26)),
        (0.55, rgb(0.07, 0.09, 0.19)),
        (1.0, rgb(0.04, 0.05, 0.11)),
    ])
    ctx.drawLinearGradient(base,
        start: CGPoint(x: rect.midX, y: rect.maxY),
        end:   CGPoint(x: rect.midX, y: rect.minY),
        options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])

    // 중앙 뒤쪽 은은한 액센트 광 (심볼을 받쳐주는 배경 글로우)
    let halo = grad([
        (0.0, alpha(ACCENT, 0.28)),
        (0.5, alpha(ACCENT2, 0.10)),
        (1.0, alpha(ACCENT, 0.0)),
    ])
    ctx.drawRadialGradient(halo,
        startCenter: CGPoint(x: rect.midX, y: rect.midY), startRadius: 0,
        endCenter:   CGPoint(x: rect.midX, y: rect.midY), endRadius: rect.width*0.62,
        options: [])
    ctx.restoreGState()

    // 상단 테두리 하이라이트 (유리 엣지)
    ctx.saveGState(); ctx.addPath(path); ctx.clip()
    ctx.addPath(roundedPath(rect.insetBy(dx: BASE*0.005, dy: BASE*0.005), rad))
    ctx.setStrokeColor(rgb(1,1,1,0.14))
    ctx.setLineWidth(BASE*0.006)
    ctx.strokePath()
    ctx.restoreGState()
}

// 발광 스트로크 헬퍼: 글로우(넓고 흐리게) → 코어(선명)
func glowStroke(_ ctx: CGContext, _ path: CGPath, color: CGColor, width: CGFloat, glow: CGFloat) {
    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: glow, color: alpha(color, 0.9))
    ctx.addPath(path)
    ctx.setStrokeColor(color)
    ctx.setLineWidth(width)
    ctx.setLineCap(.round)
    ctx.strokePath()
    ctx.restoreGState()
}

// ── 심볼: 두 노드 + 연결 흐름 ──
// 왼쪽 노드 = 속 빈 링(출발), 오른쪽 노드 = 채워진 발광 코어(도착) → 방향성
func drawSymbol(_ ctx: CGContext) {
    let rect = iconRect()
    let cy = rect.midY
    let nodeR = rect.width * 0.115
    let offset = rect.width * 0.235
    let left  = CGPoint(x: rect.midX - offset, y: cy)
    let right = CGPoint(x: rect.midX + offset, y: cy)

    let stroke = rect.width * 0.052

    // ── 연결 흐름: 두 노드를 잇는 살짝 아치형 곡선 (S 없이 위로 부드럽게) ──
    let arcLift = rect.height * 0.0   // 직선 기반, 필요시 아치
    let flow = CGMutablePath()
    flow.move(to: CGPoint(x: left.x + nodeR*0.2, y: cy))
    let c1 = CGPoint(x: rect.midX - offset*0.3, y: cy + arcLift)
    let c2 = CGPoint(x: rect.midX + offset*0.3, y: cy + arcLift)
    flow.addCurve(to: CGPoint(x: right.x - nodeR*0.2, y: cy), control1: c1, control2: c2)

    // 흐름 그라디언트: 왼쪽 옅은 시안 → 오른쪽 진한 블루 (마스크로 그라디언트 적용)
    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: BASE*0.03, color: alpha(ACCENT, 0.8))
    ctx.addPath(flow)
    ctx.setLineWidth(stroke*0.9)
    ctx.setLineCap(.round)
    ctx.replacePathWithStrokedPath()
    ctx.clip()
    let lineGrad = grad([(0.0, alpha(ACCENT, 0.55)), (1.0, ACCENT)])
    ctx.drawLinearGradient(lineGrad,
        start: CGPoint(x: left.x, y: cy), end: CGPoint(x: right.x, y: cy),
        options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
    ctx.restoreGState()

    // 흐름 위 방향 광점 (중앙에서 오른쪽으로 이동감)
    for (t, a, rm) in [(0.46, 0.65, 0.7), (0.66, 1.0, 1.05)] as [(CGFloat,CGFloat,CGFloat)] {
        let x = left.x + (right.x - left.x)*t
        let r = BASE*0.018*rm
        ctx.saveGState()
        ctx.setShadow(offset: .zero, blur: BASE*0.018, color: rgb(0.8,0.95,1,a))
        let g = grad([(0,rgb(1,1,1,a)),(0.6,alpha(ACCENT,a)),(1,alpha(ACCENT,0))])
        ctx.drawRadialGradient(g, startCenter: CGPoint(x:x,y:cy), startRadius: 0,
                               endCenter: CGPoint(x:x,y:cy), endRadius: r, options: [])
        ctx.restoreGState()
    }

    // ── 왼쪽 노드: 속 빈 링 (출발점) ──
    // 배경을 살짝 파내 심볼이 배경 위에 떠 보이도록 어두운 코어 먼저
    let ringPath = CGPath(ellipseIn: CGRect(x: left.x-nodeR, y: left.y-nodeR, width: 2*nodeR, height: 2*nodeR), transform: nil)
    // 링 안쪽을 배경보다 살짝 어둡게
    ctx.saveGState()
    ctx.addPath(ringPath); ctx.clip()
    ctx.setFillColor(rgb(0.04,0.05,0.11,0.6))
    ctx.fill(rect)
    ctx.restoreGState()
    glowStroke(ctx, ringPath, color: ACCENT, width: stroke, glow: BASE*0.025)
    // 링 상단 하이라이트
    let hi = CGMutablePath()
    hi.addArc(center: left, radius: nodeR, startAngle: .pi*0.75, endAngle: .pi*0.15, clockwise: true)
    glowStroke(ctx, hi, color: rgb(1,1,1,0.7), width: stroke*0.34, glow: 0)

    // ── 오른쪽 노드: 채워진 발광 코어 (도착점) ──
    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: BASE*0.04, color: alpha(ACCENT2, 0.95))
    let coreGrad = grad([
        (0.0, rgb(0.95, 0.97, 1.0)),
        (0.35, ACCENT),
        (1.0, ACCENT2),
    ])
    ctx.addPath(CGPath(ellipseIn: CGRect(x: right.x-nodeR, y: right.y-nodeR, width: 2*nodeR, height: 2*nodeR), transform: nil))
    ctx.clip()
    ctx.drawRadialGradient(coreGrad,
        startCenter: CGPoint(x: right.x - nodeR*0.35, y: right.y + nodeR*0.35), startRadius: 0,
        endCenter:   CGPoint(x: right.x, y: right.y), endRadius: nodeR*1.4,
        options: [.drawsAfterEndLocation])
    ctx.restoreGState()
    // 코어 상단 스펙큘러 하이라이트
    let spec = CGRect(x: right.x - nodeR*0.5, y: right.y + nodeR*0.1, width: nodeR*0.6, height: nodeR*0.42)
    ctx.saveGState()
    ctx.addPath(CGPath(ellipseIn: spec, transform: nil)); ctx.clip()
    let sg = grad([(0, rgb(1,1,1,0.85)),(1,rgb(1,1,1,0))])
    ctx.drawRadialGradient(sg, startCenter: CGPoint(x: spec.midX, y: spec.midY), startRadius: 0,
                           endCenter: CGPoint(x: spec.midX, y: spec.midY), endRadius: nodeR*0.5, options: [])
    ctx.restoreGState()
}

func renderPNG(size: Int, to path: String) {
    let ctx = makeContext(size)
    drawBackground(ctx)
    drawSymbol(ctx)
    let img = ctx.makeImage()!
    let rep = NSBitmapImageRep(cgImage: img)
    try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: path))
}

let args = CommandLine.arguments.dropFirst()
var it = args.makeIterator()
while let s = it.next(), let p = it.next() {
    renderPNG(size: Int(s)!, to: p); print("rendered \(s)px -> \(p)")
}
