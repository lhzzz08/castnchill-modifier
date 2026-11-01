// make_icon.swift —— 生成像素鱼 App 图标（方向 A · 像素游戏机）
// 用法: swift make_icon.swift <输出 iconset 目录>
// 32 格网格布局：深夜蓝圆角底 + 湖蓝像素内框 + 12x8 像素鱼 + 水花
import AppKit

let outDir = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "AppIcon.iconset"
let fm = FileManager.default
try? fm.createDirectory(atPath: outDir, withIntermediateDirectories: true)

// 16x10 像素鱼（t 背鳍 / b 身体 / e 眼 / f 尾鳍，尾鳍贴合身体）
let fishArt = [
    "................",
    ".....ttt........",
    "....bbbbbb......",
    "..bbbbbbbbbb....",
    ".bbebbbbbbbbff..",
    "bbbbbbbbbbbbff..",
    "bbbbbbbbbbbbff..",
    ".bbbbbbbbbbbbf..",
    "..bbbbbbbbbb....",
    "....bbbbbb......",
]
let fishPal: [Character: (CGFloat, CGFloat, CGFloat)] = [
    "b": (1.00, 0.80, 0.667),  // #ffccaa 桃橙
    "e": (0.063, 0.071, 0.110),// #10121c
    "t": (0.910, 0.529, 0.353),// #e8875a
    "f": (0.910, 0.529, 0.353),
]

func rgba(_ hex: UInt32, _ a: CGFloat = 1) -> CGColor {
    CGColor(red: CGFloat((hex >> 16) & 0xff) / 255,
            green: CGFloat((hex >> 8) & 0xff) / 255,
            blue: CGFloat(hex & 0xff) / 255, alpha: a)
}

func render(_ size: CGFloat) -> NSImage {
    let img = NSImage(size: NSSize(width: size, height: size))
    img.lockFocus()
    guard let ctx = NSGraphicsContext.current?.cgContext else { fatalError() }
    let u = size / 24  // 24 格网格

    // 圆角底（macOS 图标轮廓）
    let radius = size * 0.2237
    let bg = NSBezierPath(roundedRect: NSRect(x: 0, y: 0, width: size, height: size),
                          xRadius: radius, yRadius: radius)
    bg.addClip()
    ctx.setFillColor(rgba(0x1a1c2c))
    ctx.fill(CGRect(x: 0, y: 0, width: size, height: size))

    // 网格坐标（row 0 = 顶部）→ 屏幕矩形；+0.5px 外扩消除抗锯齿接缝
    func cell(_ col: Int, _ row: Int, _ color: CGColor, _ n: Int = 1) {
        ctx.setFillColor(color)
        for dc in 0..<n {
            ctx.fill(CGRect(x: CGFloat(col + dc) * u - 0.25, y: CGFloat(23 - row) * u - 0.25,
                            width: u + 0.5, height: u + 0.5))
        }
    }

    // 湖蓝像素内框（2..21 圈）
    for i in 2..<22 {
        cell(2, i, rgba(0x29adff))
        cell(21, i, rgba(0x29adff))
        cell(i, 2, rgba(0x29adff))
        cell(i, 21, rgba(0x29adff))
    }

    // 气泡（嘴上方）
    cell(6, 4, rgba(0x6cc7ff, 0.9))
    cell(5, 5, rgba(0x6cc7ff, 0.6))
    cell(17, 19, rgba(0x6cc7ff, 0.6))

    // 像素鱼（(4,7) 起，16x10）
    for (row, line) in fishArt.enumerated() {
        for (col, ch) in line.enumerated() where ch != "." {
            if let c = fishPal[ch] {
                cell(4 + col, 7 + row, CGColor(red: c.0, green: c.1, blue: c.2, alpha: 1))
            }
        }
    }

    img.unlockFocus()
    return img
}

let sizes: [(String, CGFloat)] = [
    ("icon_16x16.png", 16), ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32), ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128), ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256), ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512), ("icon_512x512@2x.png", 1024),
]
for (name, px) in sizes {
    let img = render(px)
    guard let tiff = img.tiffRepresentation,
          let rep = NSBitmapImageRep(data: tiff),
          let png = rep.representation(using: .png, properties: [:]) else { continue }
    try! png.write(to: URL(fileURLWithPath: outDir + "/" + name))
    print("✓ \(name) (\(Int(px))px)")
}
print("iconset 完成: \(outDir)")
