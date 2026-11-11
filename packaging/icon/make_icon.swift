// Draws the Metadork app icon (1024 x 1024 PNG): a flat, Material-style film camera with two reels on dark grey.
// swift packaging/icon/make_icon.swift <out.png>
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let S: CGFloat = 1024
let cs = CGColorSpace(name: CGColorSpace.sRGB)!
let ctx = CGContext(data: nil, width: Int(S), height: Int(S), bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
// Work top-down like a design tool
ctx.translateBy(x: 0, y: S)
ctx.scaleBy(x: 1, y: -1)
func rgb(_ h: UInt32, _ a: CGFloat = 1) -> CGColor {
    CGColor(colorSpace: cs, components: [CGFloat((h >> 16) & 255) / 255, CGFloat((h >> 8) & 255) / 255, CGFloat(h & 255) / 255, a])!
}

// Background: macOS icon grid (824 pt rounded square, centred), dark grey with a subtle top-to-bottom shade
let bg = CGRect(x: 100, y: 100, width: 824, height: 824)
let bgPath = CGPath(roundedRect: bg, cornerWidth: 185, cornerHeight: 185, transform: nil)
ctx.saveGState()
ctx.addPath(bgPath)
ctx.clip()
let grad = CGGradient(colorsSpace: cs, colors: [rgb(0x3A3D42), rgb(0x26282C)] as CFArray, locations: [0, 1])!
ctx.drawLinearGradient(grad, start: CGPoint(x: 0, y: bg.minY), end: CGPoint(x: 0, y: bg.maxY), options: [])
ctx.restoreGState()

let light = rgb(0xECEFF1)   // Material blue grey 50
let accent = rgb(0xFFB300)  // Material amber 600

// Reel: disc with five round cut-outs and a hub (even-odd fill)
func reel(_ c: CGPoint, _ r: CGFloat) {
    let p = CGMutablePath()
    p.addEllipse(in: CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r))
    let holeR = r * 0.24, ring = r * 0.56
    for i in 0..<5 {
        let a = CGFloat(i) * 2 * .pi / 5 - .pi / 2
        let hc = CGPoint(x: c.x + cos(a) * ring, y: c.y + sin(a) * ring)
        p.addEllipse(in: CGRect(x: hc.x - holeR, y: hc.y - holeR, width: 2 * holeR, height: 2 * holeR))
    }
    let hub = r * 0.12
    p.addEllipse(in: CGRect(x: c.x - hub, y: c.y - hub, width: 2 * hub, height: 2 * hub))
    ctx.addPath(p)
    ctx.setFillColor(accent)
    ctx.fillPath(using: .evenOdd)
}
ctx.translateBy(x: 0, y: 26)  // centre the glyph optically
reel(CGPoint(x: 342, y: 376), 104)
reel(CGPoint(x: 588, y: 352), 128)

// Body
ctx.setFillColor(light)
ctx.addPath(CGPath(roundedRect: CGRect(x: 236, y: 520, width: 438, height: 236), cornerWidth: 44, cornerHeight: 44, transform: nil))
ctx.fillPath()
// Lens / matte box: a trapezoid opening to the right
let lens = CGMutablePath()
lens.move(to: CGPoint(x: 664, y: 590))
lens.addLine(to: CGPoint(x: 800, y: 532))
lens.addLine(to: CGPoint(x: 800, y: 744))
lens.addLine(to: CGPoint(x: 664, y: 686))
lens.closeSubpath()
ctx.addPath(lens)
ctx.fillPath()
// Record light on the body
ctx.setFillColor(rgb(0xE53935))
ctx.fillEllipse(in: CGRect(x: 280, y: 564, width: 44, height: 44))

let img = ctx.makeImage()!
let out = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "icon.png")
let dest = CGImageDestinationCreateWithURL(out as CFURL, UTType.png.identifier as CFString, 1, nil)!
CGImageDestinationAddImage(dest, img, nil)
CGImageDestinationFinalize(dest)
