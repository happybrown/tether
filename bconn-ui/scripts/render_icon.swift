import AppKit
import CoreGraphics

// bconn 앱 아이콘 렌더러.
// 컨셉: 두 화면(사각형) 사이를 양방향 화살표로 잇는 단순한 형태.
// 모든 좌표는 1024pt 기준으로 그린 뒤 요청 크기로 스케일한다.
//
// 재생성:
//   swiftc -O render_icon.swift -o /tmp/render_icon
//   ICONSET=../Sources/BconnApp/Assets.xcassets/AppIcon.appiconset
//   /tmp/render_icon 16 "$ICONSET/icon_16.png" 32 "$ICONSET/icon_32.png" \
//     64 "$ICONSET/icon_64.png" 128 "$ICONSET/icon_128.png" \
//     256 "$ICONSET/icon_256.png" 512 "$ICONSET/icon_512.png" \
//     1024 "$ICONSET/icon_1024.png"

let BASE: CGFloat = 1024

func makeContext(_ size: Int) -> CGContext {
    let cs = CGColorSpaceCreateDeviceRGB()
    let ctx = CGContext(data: nil, width: size, height: size,
                        bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    let scale = CGFloat(size) / BASE
    ctx.scaleBy(x: scale, y: scale)
    ctx.setAllowsAntialiasing(true)
    ctx.interpolationQuality = .high
    return ctx
}

// macOS 아이콘 표준: 캔버스의 약 80%를 아트가 차지 (상하좌우 여백).
func iconRect() -> CGRect {
    let inset: CGFloat = BASE * 0.10
    return CGRect(x: inset, y: inset, width: BASE - 2 * inset, height: BASE - 2 * inset)
}

func roundedRectPath(_ rect: CGRect, radius: CGFloat) -> CGPath {
    CGPath(roundedRect: rect, cornerWidth: radius, cornerHeight: radius, transform: nil)
}

func drawBackground(_ ctx: CGContext) {
    let rect = iconRect()
    // Big Sur 스타일 둥근 사각형 (superellipse 근사: 모서리 반경 ≈ 너비의 22.5%)
    let path = roundedRectPath(rect, radius: rect.width * 0.225)
    ctx.saveGState()
    ctx.addPath(path)
    ctx.clip()

    // 파랑 → 보라 대각선 그라디언트 (기존 아이콘 톤 계승)
    let cs = CGColorSpaceCreateDeviceRGB()
    let colors = [
        CGColor(red: 0.30, green: 0.42, blue: 0.92, alpha: 1),  // 밝은 파랑 (상단)
        CGColor(red: 0.46, green: 0.30, blue: 0.82, alpha: 1),  // 보라 (하단)
    ] as CFArray
    let grad = CGGradient(colorsSpace: cs, colors: colors, locations: [0, 1])!
    ctx.drawLinearGradient(grad,
                           start: CGPoint(x: rect.minX, y: rect.maxY),
                           end: CGPoint(x: rect.maxX, y: rect.minY),
                           options: [])
    ctx.restoreGState()

    // 은은한 상단 하이라이트로 입체감
    ctx.saveGState()
    ctx.addPath(path)
    ctx.clip()
    let hl = CGGradient(colorsSpace: cs,
                        colors: [CGColor(red: 1, green: 1, blue: 1, alpha: 0.18),
                                 CGColor(red: 1, green: 1, blue: 1, alpha: 0)] as CFArray,
                        locations: [0, 1])!
    ctx.drawLinearGradient(hl,
                           start: CGPoint(x: rect.midX, y: rect.maxY),
                           end: CGPoint(x: rect.midX, y: rect.midY),
                           options: [])
    ctx.restoreGState()
}

// 한 개의 화면(모니터). 흰색 둥근 사각형 + 안쪽에 살짝 어두운 스크린.
func drawScreen(_ ctx: CGContext, center: CGPoint, w: CGFloat, h: CGFloat) {
    let white = CGColor(red: 1, green: 1, blue: 1, alpha: 0.96)
    let body = CGRect(x: center.x - w/2, y: center.y - h/2, width: w, height: h)
    let r = w * 0.14

    // 부드러운 그림자
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -BASE*0.006),
                  blur: BASE*0.02,
                  color: CGColor(red: 0, green: 0, blue: 0, alpha: 0.22))
    ctx.addPath(roundedRectPath(body, radius: r))
    ctx.setFillColor(white)
    ctx.fillPath()
    ctx.restoreGState()

    // 안쪽 스크린 (배경색이 살짝 비치는 톤)
    let pad = w * 0.11
    let inner = body.insetBy(dx: pad, dy: pad)
    ctx.addPath(roundedRectPath(inner, radius: r * 0.6))
    ctx.setFillColor(CGColor(red: 0.36, green: 0.36, blue: 0.55, alpha: 1))
    ctx.fillPath()
}

// 두 화면 사이 양방향 화살표. tip 은 양 끝(화면 안쪽 가장자리 근처).
func drawArrows(_ ctx: CGContext, y: CGFloat, leftX: CGFloat, rightX: CGFloat) {
    let white = CGColor(red: 1, green: 1, blue: 1, alpha: 0.98)
    let gap = rightX - leftX
    let lineW = BASE * 0.034
    let head = gap * 0.26             // 화살촉 길이
    let headHalfH = head * 0.85       // 화살촉 절반 높이 (통통하게)

    // 양 끝 tip 을 화면 가장자리에서 살짝 띄운다.
    let margin = gap * 0.06
    let x0 = leftX + margin           // 왼쪽 tip
    let x1 = rightX - margin          // 오른쪽 tip

    // 가로 축: 두 화살촉 밑변 사이만 잇는다 (촉과 겹치지 않게)
    ctx.setStrokeColor(white)
    ctx.setLineWidth(lineW)
    ctx.setLineCap(.round)
    ctx.move(to: CGPoint(x: x0 + head, y: y))
    ctx.addLine(to: CGPoint(x: x1 - head, y: y))
    ctx.strokePath()

    // 화살촉 (양쪽) — 채워진 삼각형
    func arrowHead(tipX: CGFloat, pointingLeft: Bool) {
        let dir: CGFloat = pointingLeft ? 1 : -1  // 밑변이 tip 반대쪽
        ctx.beginPath()
        ctx.move(to: CGPoint(x: tipX, y: y))
        ctx.addLine(to: CGPoint(x: tipX + dir*head, y: y + headHalfH))
        ctx.addLine(to: CGPoint(x: tipX + dir*head, y: y - headHalfH))
        ctx.closePath()
        ctx.setFillColor(white)
        ctx.fillPath()
    }
    arrowHead(tipX: x0, pointingLeft: true)
    arrowHead(tipX: x1, pointingLeft: false)
}

func renderPNG(size: Int, to path: String) {
    let ctx = makeContext(size)
    drawBackground(ctx)

    let rect = iconRect()
    // 두 화면을 좌우로 배치. 사이에 화살표가 들어갈 공간을 넉넉히 둔다.
    let screenW = rect.width * 0.30
    let screenH = screenW * 0.74
    let cy = rect.midY
    let offset = rect.width * 0.265
    let leftC  = CGPoint(x: rect.midX - offset, y: cy)
    let rightC = CGPoint(x: rect.midX + offset, y: cy)

    drawScreen(ctx, center: leftC,  w: screenW, h: screenH)
    drawScreen(ctx, center: rightC, w: screenW, h: screenH)

    // 화살표는 두 화면 안쪽 가장자리 사이에 위치
    drawArrows(ctx,
               y: cy,
               leftX: leftC.x + screenW/2,
               rightX: rightC.x - screenW/2)

    let img = ctx.makeImage()!
    let rep = NSBitmapImageRep(cgImage: img)
    let data = rep.representation(using: .png, properties: [:])!
    try! data.write(to: URL(fileURLWithPath: path))
}

// 실행: 인자로 (크기 경로) 쌍들을 받는다.
let args = CommandLine.arguments.dropFirst()
var it = args.makeIterator()
while let s = it.next(), let p = it.next() {
    renderPNG(size: Int(s)!, to: p)
    print("rendered \(s)px -> \(p)")
}
