import AppKit

/// 메뉴바에 표시할 "모니터 2개" 아이콘을 코드로 직접 그려 상태 색을 적용한다.
/// SF Symbol `display.2` 는 palette 렌더링이 불가능해 내부 화면 색만 바꿀 수 없기 때문에
/// NSBezierPath 로 테두리(중립색)와 화면 영역(상태 색)을 분리해 그린다.
enum MenuBarIcon {
    /// 렌더링 크기(pt). 메뉴바 권장은 18pt 내외이나 여유를 두고 그린 뒤 setSize 로 맞춘다.
    private static let size: CGFloat = 18

    /// `tint` 색을 화면 영역에 적용한 NSImage 를 반환한다.
    /// isTemplate = false 로 두어 색이 그대로 표시되도록 한다 (다크/라이트 모드 자동 대응은
    /// 테두리 색을 `.labelColor` 로 두는 것으로 처리).
    static func image(tint: NSColor) -> NSImage {
        let image = NSImage(size: NSSize(width: size, height: size), flipped: false) { rect in
            draw(in: rect, tint: tint)
            return true
        }
        image.isTemplate = false
        return image
    }

    private static func draw(in rect: NSRect, tint: NSColor) {
        let stroke = NSColor.labelColor
        let lineWidth: CGFloat = 1.3

        // 두 모니터 영역을 계산한다. 약간 겹치도록 배치해 "display.2" 느낌을 준다.
        let W = rect.width
        let H = rect.height

        // 뒤쪽 모니터 (오른쪽 위)
        let backScreen = NSRect(x: W * 0.30, y: H * 0.38,
                                width: W * 0.58, height: H * 0.42)
        let backStand  = NSRect(x: W * 0.50, y: H * 0.20,
                                width: W * 0.18, height: H * 0.18)

        // 앞쪽 모니터 (왼쪽 아래)
        let frontScreen = NSRect(x: W * 0.06, y: H * 0.22,
                                 width: W * 0.58, height: H * 0.42)
        let frontStand  = NSRect(x: W * 0.26, y: H * 0.04,
                                 width: W * 0.18, height: H * 0.18)

        let corner: CGFloat = 2.2

        // 뒤쪽 모니터 먼저 그리기
        drawMonitor(screen: backScreen, stand: backStand,
                    tint: tint, stroke: stroke, lineWidth: lineWidth, corner: corner,
                    dim: true)

        // 앞쪽 모니터가 덮을 영역을 clear 로 지워 뒤쪽 테두리/스탠드가 비쳐 보이지 않게 한다.
        // 화면 영역과 스탠드 영역 모두 여유 있게 빼낸다.
        if let ctx = NSGraphicsContext.current?.cgContext {
            ctx.saveGState()
            ctx.setBlendMode(.clear)
            let pad: CGFloat = lineWidth + 0.5
            let clearScreen = frontScreen.insetBy(dx: -pad, dy: -pad)
            ctx.fill(clearScreen)
            let clearStand = NSRect(
                x: frontStand.minX - pad,
                y: frontStand.minY - pad,
                width: frontStand.width + pad * 2,
                height: frontStand.height + pad * 2
            )
            ctx.fill(clearStand)
            ctx.restoreGState()
        }

        drawMonitor(screen: frontScreen, stand: frontStand,
                    tint: tint, stroke: stroke, lineWidth: lineWidth, corner: corner,
                    dim: false)
    }

    private static func drawMonitor(screen: NSRect, stand: NSRect,
                                    tint: NSColor, stroke: NSColor,
                                    lineWidth: CGFloat, corner: CGFloat,
                                    dim: Bool) {
        let fillColor = dim ? tint.withAlphaComponent(0.55) : tint

        // 화면 (둥근 사각형)
        let screenPath = NSBezierPath(roundedRect: screen.insetBy(dx: lineWidth/2, dy: lineWidth/2),
                                      xRadius: corner, yRadius: corner)
        fillColor.setFill()
        screenPath.fill()
        stroke.setStroke()
        screenPath.lineWidth = lineWidth
        screenPath.stroke()

        // 스탠드 (사다리꼴)
        let standPath = NSBezierPath()
        let sTop    = stand.maxY
        let sBottom = stand.minY
        let midX    = stand.midX
        let halfTop    = stand.width * 0.22
        let halfBottom = stand.width * 0.48
        standPath.move(to:    NSPoint(x: midX - halfTop,    y: sTop))
        standPath.line(to:    NSPoint(x: midX + halfTop,    y: sTop))
        standPath.line(to:    NSPoint(x: midX + halfBottom, y: sBottom))
        standPath.line(to:    NSPoint(x: midX - halfBottom, y: sBottom))
        standPath.close()
        stroke.setStroke()
        standPath.lineWidth = lineWidth
        standPath.stroke()
    }
}
