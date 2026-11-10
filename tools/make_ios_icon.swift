// Draws the iOS app icon: 同級生２ as a 2x2 grid of bold characters, white on dark blue.
//   swift tools/make_ios_icon.swift OUT.png [SIZE=1024]
// Output: opaque (no alpha), sRGB, square, no pre-rounded corners, no metadata chunks.
// Font: PingFang TC Semibold (macOS system font); the title uses a full-width ２ (U+FF12).
// The 2x2 grid keeps the characters large enough to read at 40 px; it sits in the central ~68%.
import CoreGraphics
import CoreText
import Foundation
import ImageIO

let args = CommandLine.arguments
guard args.count >= 2 else { print("usage: make_ios_icon.swift OUT.png [SIZE]"); exit(2) }
let size = args.count > 2 ? Int(args[2])! : 1024
let s = CGFloat(size)
let cs = CGColorSpace(name: CGColorSpace.sRGB)!
let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                    bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
ctx.setFillColor(CGColor(colorSpace: cs, components: [0.06, 0.13, 0.33, 1])!)
ctx.fill(CGRect(x: 0, y: 0, width: s, height: s))

let font = CTFontCreateWithName("PingFangTC-Semibold" as CFString, s * 0.36, nil)
let chars = ["同", "級", "生", "２"]
let cell = s * 0.34 // grid pitch
let origin = CGPoint(x: s / 2 - cell / 2, y: s / 2 + cell / 2) // centre of the top-left cell
for (i, ch) in chars.enumerated() {
    let attr = [kCTFontAttributeName: font, kCTForegroundColorFromContextAttributeName: true] as CFDictionary
    let line = CTLineCreateWithAttributedString(CFAttributedStringCreate(nil, ch as CFString, attr)!)
    let b = CTLineGetBoundsWithOptions(line, .useGlyphPathBounds)
    let cx = origin.x + CGFloat(i % 2) * cell, cy = origin.y - CGFloat(i / 2) * cell
    ctx.setFillColor(CGColor(colorSpace: cs, components: [1, 1, 1, 1])!)
    ctx.textPosition = CGPoint(x: cx - b.midX, y: cy - b.midY)
    CTLineDraw(line, ctx)
}
let img = ctx.makeImage()!
let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: args[1]) as CFURL, "public.png" as CFString, 1, nil)!
CGImageDestinationAddImage(dest, img, nil)
guard CGImageDestinationFinalize(dest) else { print("write failed"); exit(1) }

// ImageIO adds an eXIf chunk (pixel size only); drop every chunk except IHDR, sRGB, IDAT, IEND.
var png = [UInt8](try! Data(contentsOf: URL(fileURLWithPath: args[1])))
var out = Array(png[0..<8])
var pos = 8
while pos < png.count {
    let n = Int(png[pos]) << 24 | Int(png[pos + 1]) << 16 | Int(png[pos + 2]) << 8 | Int(png[pos + 3])
    let type = String(bytes: png[(pos + 4)..<(pos + 8)], encoding: .ascii)!
    if ["IHDR", "sRGB", "IDAT", "IEND"].contains(type) { out += png[pos..<(pos + 12 + n)] }
    pos += 12 + n
}
try! Data(out).write(to: URL(fileURLWithPath: args[1]))
