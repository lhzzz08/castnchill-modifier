// CNCPanel —— Cast n Chill 修改器面板 · 像素游戏机版（方向 A）
// 深夜蓝 #1a1c2c + PICO-8 调色 · 硬边像素框 · 等宽字体
// 功能与 IPC 协议保持不变（README/使用手册不受影响）
import SwiftUI
import AppKit
import Darwin

// MARK: - 像素调色板

enum Pix {
    static let bg     = Color(hex: 0x1a1c2c)   // 深夜蓝
    static let panel  = Color(hex: 0x24283d)   // 卡片
    static let dark   = Color(hex: 0x10121c)   // 最深（阴影/槽）
    static let border = Color(hex: 0x3b4058)   // 描边
    static let text   = Color(hex: 0xf4f4f4)
    static let muted  = Color(hex: 0xa7aab8)
    static let peach  = Color(hex: 0xffccaa)
    static let blue   = Color(hex: 0x29adff)
    static let green  = Color(hex: 0x38b764)
    static let yellow = Color(hex: 0xffcd75)
    static let orange = Color(hex: 0xe8875a)
    static let red    = Color(hex: 0xff5f57)
}

extension Color {
    init(hex: UInt32) {
        self.init(.sRGB,
                  red: Double((hex >> 16) & 0xff) / 255,
                  green: Double((hex >> 8) & 0xff) / 255,
                  blue: Double(hex & 0xff) / 255,
                  opacity: 1)
    }
}

// MARK: - 像素部件

/// 硬边斜面：上/左亮 2px，下/右暗 2px（PICO-8 UI 手法）
struct PixelFrame: View {
    var light: Color
    var dark: Color
    var body: some View {
        ZStack {
            VStack(spacing: 0) {
                Rectangle().fill(light).frame(height: 2)
                Spacer(minLength: 0)
                Rectangle().fill(dark).frame(height: 2)
            }
            HStack(spacing: 0) {
                Rectangle().fill(light).frame(width: 2)
                Spacer(minLength: 0)
                Rectangle().fill(dark).frame(width: 2)
            }
        }
        .allowsHitTesting(false)
    }
}

enum PixelColor {
    case blue, green, yellow, orange, red, gray
    var base: Color {
        switch self {
        case .blue: return Pix.blue
        case .green: return Pix.green
        case .yellow: return Pix.yellow
        case .orange: return Pix.orange
        case .red: return Pix.red
        case .gray: return Pix.border
        }
    }
    var light: Color {
        switch self {
        case .blue: return Color(hex: 0x6cc7ff)
        case .green: return Color(hex: 0x7adaa0)
        case .yellow: return Color(hex: 0xffe49a)
        case .orange: return Color(hex: 0xffa67e)
        case .red: return Color(hex: 0xff9d97)
        case .gray: return Color(hex: 0x565c7a)
        }
    }
    var dark: Color {
        switch self {
        case .blue: return Color(hex: 0x1d7cb8)
        case .green: return Color(hex: 0x26804a)
        case .yellow: return Color(hex: 0xcf9c3d)
        case .orange: return Color(hex: 0xb05f3a)
        case .red: return Color(hex: 0xc23a34)
        case .gray: return Color(hex: 0x22263a)
        }
    }
    var fg: Color {
        self == .yellow ? Pix.dark : Color(hex: 0xf7f9ff)
    }
}

struct PixelButtonStyle: ButtonStyle {
    var tone: PixelColor = .blue
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 11, weight: .medium, design: .monospaced))
            .foregroundColor(tone.fg)
            .lineLimit(1)
            .fixedSize() /* 防止被贪心滑杆挤压换行重叠 */
            .padding(.horizontal, 9)
            .padding(.vertical, 5)
            .background(tone.base)
            .overlay(PixelFrame(light: tone.light, dark: tone.dark))
            /* 硬偏移阴影只垫底板矩形（.shadow 会连文字一起复制，小字发糊） */
            .background(
                Rectangle().fill(Pix.dark)
                    .offset(x: 2, y: 2)
            )
            .offset(x: configuration.isPressed ? 2 : 0, y: configuration.isPressed ? 2 : 0)
    }
}

struct PixelCheck: View {
    let title: String
    let on: Bool
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            HStack(spacing: 6) {
                ZStack {
                    Rectangle().fill(Pix.dark).frame(width: 12, height: 12)
                    if on { Rectangle().fill(Pix.yellow).frame(width: 8, height: 8) }
                }
                Text(title)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundColor(on ? Pix.text : Pix.muted)
                    .lineLimit(1)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 7)
            .padding(.vertical, 6)
            .background(on ? Color(hex: 0x2c3a4e) : Pix.panel)
            .overlay(PixelFrame(light: on ? Color(hex: 0x4a6a84) : Pix.border, dark: Pix.dark))
        }
        .buttonStyle(.plain)
    }
}

/// 硬边像素滑杆：数值标签内置，拖动时实时跟随。
/// 拖动期间只写本地 dragValue（父视图保持稳定，手势会话不被重建打断），
/// 松手才提交 Binding 并触发 onEnd 发命令。
struct PixelSlider: View {
    @Binding var value: Double
    let range: ClosedRange<Double>
    var step: Double = 1
    var format: String = "%.1f×"
    var onEnd: (() -> Void)? = nil
    @State private var dragValue: Double? = nil

    init(value: Binding<Double>, in range: ClosedRange<Double>, step: Double = 1,
         format: String = "%.1f×", onEnd: (() -> Void)? = nil) {
        self._value = value
        self.range = range
        self.step = step
        self.format = format
        self.onEnd = onEnd
    }

    private func snap(_ v: Double) -> Double {
        min(range.upperBound, max(range.lowerBound, (v / step).rounded() * step))
    }

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            let trackW = max(60, w - 52) /* 预留右侧数值标签 */
            let shown = dragValue ?? value
            let frac = (shown - range.lowerBound) / (range.upperBound - range.lowerBound)
            HStack(spacing: 8) {
                ZStack(alignment: .leading) {
                    Rectangle().fill(Pix.dark).frame(height: 12)
                        .overlay(PixelFrame(light: Pix.border, dark: Pix.dark))
                    Rectangle().fill(Pix.blue)
                        .frame(width: max(0, (trackW - 4) * frac), height: 8)
                        .padding(.leading, 2)
                    Rectangle().fill(Pix.yellow)
                        .frame(width: 14, height: 14)
                        .overlay(PixelFrame(light: Color(hex: 0xffe49a), dark: Color(hex: 0xcf9c3d)))
                        .offset(x: max(0, min(trackW - 14, (trackW - 14) * frac)))
                }
                .frame(maxWidth: .infinity)
                .contentShape(Rectangle())
                .simultaneousGesture(
                    DragGesture(minimumDistance: 0)
                        .onChanged { g in
                            let f = min(1, max(0, (g.location.x - 7) / max(trackW - 14, 1)))
                            dragValue = snap(range.lowerBound + f * (range.upperBound - range.lowerBound))
                        }
                        .onEnded { _ in
                            if let v = dragValue { value = v }
                            dragValue = nil
                            onEnd?()
                        }
                )
                Text(String(format: format, shown))
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundColor(Pix.text)
                    .frame(width: 44, alignment: .trailing)
            }
        }
        .frame(height: 16)
    }
}

struct PixelSection<Content: View>: View {
    let title: String
    let content: Content
    init(title: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.system(size: 10, weight: .medium, design: .monospaced))
                .foregroundColor(Pix.yellow)
            content
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Pix.panel)
        .overlay(PixelFrame(light: Color(hex: 0x363b55), dark: Pix.dark))
    }
}

// MARK: - Unix socket 客户端（协议不变）

struct ModResponse {
    var raw: String
    var json: [String: Any]?
    var ok: Bool { (json?["ok"] as? Bool) ?? false }
    func string(_ key: String) -> String {
        if let s = json?[key] as? String { return s }
        if let n = json?[key] as? NSNumber { return n.stringValue }
        return "—"
    }
}

enum ModClient {
    static var socketPath: String { "/tmp/cnc-mod-\(getuid()).sock" }

    /// 阻塞调用，只能在后台线程用（见 request）
    static func send(_ command: String, timeoutSec: Double) -> ModResponse? {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { return nil }
        defer { close(fd) }

        var tv = timeval(tv_sec: Int(timeoutSec),
                         tv_usec: Int32((timeoutSec - Double(Int(timeoutSec))) * 1_000_000))
        let tvLen = socklen_t(MemoryLayout<timeval>.size)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, tvLen)
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, tvLen)
        var one: Int32 = 1 /* mod 先断开时 write 返回错误而不是 SIGPIPE 杀掉面板 */
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))

        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        /* Array 实例方法的 withUnsafeBytes 才是缓冲区内容 */
        let pathBytes = Array(socketPath.utf8CString)
        pathBytes.withUnsafeBytes { buf in
            withUnsafeMutableBytes(of: &addr.sun_path) { dst in
                let n = min(buf.count, dst.count)
                dst.copyMemory(from: UnsafeRawBufferPointer(rebasing: buf.prefix(n)))
            }
        }
        let connectResult = withUnsafePointer(to: &addr) { ptr -> Int32 in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard connectResult == 0 else { return nil }

        let req = (command + "\n").utf8CString
        let sentAll = req.withUnsafeBufferPointer { buf -> Bool in
            var sent = 0
            while sent < buf.count - 1 {
                let n = write(fd, buf.baseAddress! + sent, buf.count - 1 - sent)
                if n <= 0 { return false }
                sent += n
            }
            return true
        }
        guard sentAll else { return nil }

        var data = [UInt8]()
        var chunk = [UInt8](repeating: 0, count: 1024)
        while true {
            let n = read(fd, &chunk, chunk.count)
            guard n > 0 else { break }
            data.append(contentsOf: chunk[0..<n])
            if data.contains(0x0A) { break }
        }
        guard let lineEnd = data.firstIndex(of: 0x0A) else {
            if data.isEmpty { return nil }
            return ModResponse(raw: String(decoding: data, as: UTF8.self), json: nil)
        }
        let line = String(decoding: data[0..<lineEnd], as: UTF8.self)
        var json: [String: Any]? = nil
        if let d = line.data(using: .utf8),
           let obj = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any] {
            json = obj
        }
        return ModResponse(raw: line, json: json)
    }

    private static let queue = DispatchQueue(label: "cnc.panel.ipc", qos: .userInitiated,
                                             attributes: .concurrent)

    /// 在后台队列发命令，不阻塞界面
    static func request(_ command: String, timeoutSec: Double) async -> ModResponse? {
        await withCheckedContinuation { cont in
            queue.async { cont.resume(returning: send(command, timeoutSec: timeoutSec)) }
        }
    }
}

// MARK: - 状态模型

/// 面板上的开关（命令名 → 显示名），顺序即界面顺序
let kToggles: [(cmd: String, title: String)] = [
    ("fish.stamina", "秒赢对决"), ("fish.bite", "即时上钩"), ("line.safe", "不断之线"),
    ("player.stamina", "无限体力"), ("cast.super", "超级投掷"),
]

@MainActor
final class PanelModel: ObservableObject {
    @Published var connected = false
    @Published var statusDetail = ""
    @Published var gameVersion = "—"
    @Published var assemblies = "—"
    @Published var modVersion = "—"
    @Published var cash = "—"
    @Published var speed = 1.0
    @Published var logLines: [String] = []
    @Published var cashInput = ""
    // M4/M5：开关与倍率以 mod 的 state.get 为准（面板重启/命令失败都不会和游戏不同步）
    @Published var toggles: [String: Bool] = [:]
    @Published var boatMult = 1.0

    private var timer: Timer?
    private var refreshing = false
    private var pendingCommands = 0

    private static let timeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f
    }()

    init() {
        /* 状态轮询常开（本地 socket 查询，在后台队列执行，不卡界面）；
         * 断开时同样靠它自动重连 */
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    func isOn(_ cmd: String) -> Bool { toggles[cmd] ?? false }

    func log(_ s: String) {
        logLines.insert("\(Self.timeFormatter.string(from: Date()))  \(s)", at: 0)
        if logLines.count > 200 { logLines.removeLast(logLines.count - 200) }
    }

    func refresh() {
        guard !refreshing else { return }
        refreshing = true
        Task {
            let resp = await ModClient.request("state.get", timeoutSec: 1.5)
            refreshing = false
            apply(resp)
        }
    }

    private func apply(_ resp: ModResponse?) {
        guard let resp else {
            connected = false
            statusDetail = "游戏未启动或未注入"
            return
        }
        guard resp.ok else {
            connected = false
            let err = resp.string("error")
            switch err {
            case "booting": statusDetail = "游戏启动中，自动重试…"
            case "main-thread timeout", "main-thread unavailable":
                statusDetail = "主线程未就绪，自动重试…"
            default: statusDetail = err == "—" ? "未知错误" : err
            }
            return
        }
        connected = true
        statusDetail = ""
        gameVersion = resp.string("game_version")
        assemblies = resp.string("assemblies")
        modVersion = resp.string("mod_version")
        if let c = resp.json?["cash"] as? NSNumber { cash = c.intValue < 0 ? "未进档" : c.stringValue }
        /* 有命令在途时不覆盖可操作控件，避免开关/滑杆闪回旧值 */
        guard pendingCommands == 0 else { return }
        if let s = resp.json?["speed"] as? NSNumber { speed = min(16, max(1, s.doubleValue)) }
        if let t = resp.json?["toggles"] as? [String: Bool] { toggles = t }
        if let m = resp.json?["mults"] as? [String: NSNumber], let b = m["boat.fast"] {
            boatMult = b.doubleValue
        }
    }

    /* 操作命令可能较慢（卖鱼 + 落盘），超时放宽到 10s（mod 侧主线程等待上限 8s） */
    private func run(_ cmd: String, label: String) {
        pendingCommands += 1
        Task {
            let resp = await ModClient.request(cmd, timeoutSec: 10)
            pendingCommands -= 1
            if let resp {
                log("\(label) \(resp.ok ? "→" : "✘") \(resp.raw)")
            } else {
                log("\(label) ✘ 连接失败")
            }
            refresh()
        }
    }

    func cashAdd(_ n: Int) { run("cash.add \(n)", label: "加钱 \(n)") }
    func cashSet() {
        let text = cashInput.trimmingCharacters(in: .whitespaces)
        guard let n = Int(text), n >= 0 else {
            log("设钱 ✘ 金额无效：「\(cashInput)」（请输入非负整数）")
            return
        }
        run("cash.set \(n)", label: "设钱 \(n)")
    }
    func speedApply() { run(String(format: "speed.set %.2f", speed), label: String(format: "速度 %.1f×", speed)) }
    func speedReset() { speed = 1; run("speed.set 1", label: "速度还原") }
    func sellAll() { run("sell.all", label: "一键卖鱼") }
    func m4Toggle(_ name: String) {
        let on = !isOn(name)
        toggles[name] = on /* 先显示目标状态；结果以下一次 state.get 为准 */
        run("\(name) \(on ? "on" : "off")", label: "\(name) \(on ? "开" : "关")")
    }
    func m4Mult(_ name: String, _ v: Double) { run(String(format: "%@ %.1f", name, v), label: String(format: "%@ %.1f×", name, v)) }
}

// MARK: - 界面

struct ContentView: View {
    @StateObject private var model = PanelModel()

    var body: some View {
        VStack(spacing: 0) {
            header
            VStack(spacing: 8) {
                PixelSection(title: "▚ 现金") {
                    HStack(spacing: 8) {
                        Text(model.cash)
                            .font(.system(size: 20, weight: .medium, design: .monospaced))
                            .foregroundColor(Pix.peach)
                        Spacer()
                        TextField("金额", text: $model.cashInput)
                            .textFieldStyle(.plain)
                            .font(.system(size: 12, design: .monospaced))
                            .foregroundColor(Pix.text)
                            .padding(.horizontal, 6).padding(.vertical, 5)
                            .frame(width: 78)
                            .background(Pix.dark)
                            .overlay(PixelFrame(light: Pix.border, dark: Pix.dark))
                        Button("+1万") { model.cashAdd(10000) }.buttonStyle(PixelButtonStyle(tone: .blue))
                        Button("+10万") { model.cashAdd(100000) }.buttonStyle(PixelButtonStyle(tone: .blue))
                        Button("设定") { model.cashSet() }.buttonStyle(PixelButtonStyle(tone: .green))
                    }
                }
                PixelSection(title: "▚ 卖鱼") {
                    Button("一键卖出") { model.sellAll() }.buttonStyle(PixelButtonStyle(tone: .yellow))
                }
                PixelSection(title: "▚ 钓鱼增强") {
                    VStack(spacing: 6) {
                        HStack(spacing: 6) {
                            ForEach(kToggles.prefix(3), id: \.cmd) { t in
                                PixelCheck(title: t.title, on: model.isOn(t.cmd)) { model.m4Toggle(t.cmd) }
                            }
                        }
                        HStack(spacing: 6) {
                            ForEach(kToggles.dropFirst(3), id: \.cmd) { t in
                                PixelCheck(title: t.title, on: model.isOn(t.cmd)) { model.m4Toggle(t.cmd) }
                            }
                        }
                    }
                }
                PixelSection(title: "▚ 速度") {
                    HStack(spacing: 8) {
                        Text("加速").font(.system(size: 11, design: .monospaced)).foregroundColor(Pix.muted).frame(width: 30, alignment: .leading)
                        PixelSlider(value: $model.speed, in: 1...16, step: 0.5, format: "%.1f×") { model.speedApply() }
                        Button("还原") { model.speedReset() }.buttonStyle(PixelButtonStyle(tone: .gray))
                    }
                    HStack(spacing: 8) {
                        Text("船速").font(.system(size: 11, design: .monospaced)).foregroundColor(Pix.muted).frame(width: 30, alignment: .leading)
                        PixelSlider(value: $model.boatMult, in: 1...10, step: 1, format: "%.0f×") { model.m4Mult("boat.fast", model.boatMult) }
                    }
                }
            }
            .padding(8)
            console
        }
        .background(Pix.bg)
        .preferredColorScheme(.dark)
        .frame(width: 470) /* 宽度锁定，高度交给窗口自动伸缩 */
        .onAppear { model.refresh() }
    }

    private var header: some View {
        HStack(spacing: 8) {
            Rectangle()
                .fill(model.connected ? Pix.green : Pix.red)
                .frame(width: 10, height: 10)
                .overlay(PixelFrame(light: .white.opacity(0.25), dark: Pix.dark))
            Text(model.connected ? "已连接游戏" : "未连接：\(model.statusDetail)")
                .font(.system(size: 12, weight: .medium, design: .monospaced))
                .foregroundColor(model.connected ? Pix.text : Pix.red)
            Spacer()
            Text("v\(model.modVersion)")
                .font(.system(size: 10, design: .monospaced))
                .foregroundColor(Pix.dark)
                .padding(.horizontal, 6).padding(.vertical, 2)
                .background(Pix.green)
            if !model.connected {
                Text("socket: \(ModClient.socketPath)")
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundColor(Pix.muted)
                    .lineLimit(1)
                    .truncationMode(.head)
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .background(Color(hex: 0x24283d))
        .overlay(Rectangle().fill(Pix.dark).frame(height: 2), alignment: .bottom)
    }

    @State private var logExpanded = false

    private var console: some View {
        VStack(alignment: .leading, spacing: 6) {
            /* 折叠头：点击展开/收起；中间显示最近一条操作记录 */
            Button {
                logExpanded.toggle()
                /* 直接发通知（两参数 onChange 需要 macOS 14，本面板支持 macOS 12+） */
                NotificationCenter.default.post(
                    name: PanelAppDelegate.cncToggleLog, object: logExpanded
                )
            } label: {
                HStack(spacing: 8) {
                    Text(logExpanded ? "运行日志 ▾" : "运行日志 ▸")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(Pix.muted)
                    Text(model.logLines.first ?? "暂无操作记录")
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundColor(logLinesHasError ? Pix.red : Pix.muted)
                        .lineLimit(1)
                        .truncationMode(.tail)
                    Spacer()
                    Text("游戏 \(model.gameVersion) · 程序集 \(model.assemblies)")
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundColor(Pix.muted)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if logExpanded {
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 2) {
                        ForEach(model.logLines.indices, id: \.self) { i in
                            Text(model.logLines[i])
                                .font(.system(size: 10, design: .monospaced))
                                .foregroundColor(Pix.muted)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(height: 64)
                .padding(6)
                .background(Pix.dark)
                .overlay(PixelFrame(light: Pix.border, dark: Pix.dark))
            }
        }
        .padding(10)
        .background(Color(hex: 0x1d2033))
        .overlay(Rectangle().fill(Pix.dark).frame(height: 2), alignment: .top)
    }

    /* 最近一条记录是否为失败（红色提示） */
    private var logLinesHasError: Bool {
        model.logLines.first?.contains("✘") ?? false
    }
}

// MARK: - AppKit 引导

final class PanelAppDelegate: NSObject, NSApplicationDelegate {
    private var window: NSWindow?

    static let cncToggleLog = Notification.Name("cncToggleLog")

    /* 最小菜单栏：Cmd+Q 退出 / Cmd+W 关窗 */
    func setupMenu() {
        let main = NSMenu()
        let appItem = NSMenuItem()
        let appMenu = NSMenu(title: "Cast n Chill 修改器")
        appMenu.addItem(NSMenuItem(title: "退出修改器", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
        appItem.submenu = appMenu
        main.addItem(appItem)
        let winItem = NSMenuItem()
        let winMenu = NSMenu(title: "窗口")
        winMenu.addItem(NSMenuItem(title: "最小化窗口", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m"))
        winMenu.addItem(NSMenuItem(title: "关闭窗口", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w"))
        winItem.submenu = winMenu
        main.addItem(winItem)
        NSApp.mainMenu = main
    }

    private func saveOrigin() {
        guard let w = window else { return }
        /* 存左上角（top-left）：窗口高度随日志展开变化时位置也不漂 */
        let tl = NSPoint(x: w.frame.origin.x, y: w.frame.origin.y + w.frame.height)
        UserDefaults.standard.set("\(Int(tl.x)),\(Int(tl.y))", forKey: "WinOrigin")
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        let w = NSWindow(
            contentRect: NSRect(x: 160, y: 160, width: 470, height: 530),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered, defer: false
        )
        w.minSize = NSSize(width: 470, height: 400)
        w.title = "Cast n Chill 修改器"
        w.titlebarAppearsTransparent = true
        w.titleVisibility = .hidden
        w.isReleasedWhenClosed = false /* 关闭时保留窗口对象，重开不崩（2026-11-01 闪退根因） */
        w.appearance = NSAppearance(named: .darkAqua)
        w.backgroundColor = NSColor(red: 0x1a / 255, green: 0x1c / 255, blue: 0x2c / 255, alpha: 1)
        /* 注意：不能开 isMovableByWindowBackground —— AppKit 窗口拖动会
         * 在 SwiftUI 手势之前吞掉鼠标拖动事件，导致滑杆只能点击不能拖 */
        /* 窗口高度由 AppKit 手动驱动（见 cncToggleLog 通知）：
         * NSHostingSizingOptions.intrinsicContentSize 实测不生效（内容溢出） */
        w.contentView = NSHostingView(rootView: ContentView())
        w.center()
        w.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        window = w

        /* 恢复上次退出前的窗口位置；那块屏幕已断开（外接显示器拔掉）时保持居中，
         * 否则窗口会落在看不见的地方 */
        if let s = UserDefaults.standard.string(forKey: "WinOrigin") {
            let p = s.split(separator: ",").compactMap { Double($0) }
            if p.count == 2 {
                let tl = NSPoint(x: p[0], y: p[1])
                let titleBar = NSRect(x: tl.x, y: tl.y - 30, width: w.frame.width, height: 30)
                if NSScreen.screens.contains(where: { $0.visibleFrame.intersects(titleBar) }) {
                    w.setFrameTopLeftPoint(tl)
                }
            }
        }
        NotificationCenter.default.addObserver(
            forName: NSWindow.didMoveNotification, object: w, queue: .main
        ) { [weak self] _ in self?.saveOrigin() }
        NotificationCenter.default.addObserver(
            forName: NSWindow.willCloseNotification, object: w, queue: .main
        ) { [weak self] _ in self?.saveOrigin() }

        /* 日志展开/收起 → 窗口高度随之增减（锚定左上角）。
         * 展开增量 = 日志框 64 + 内边距 12 + 间距 6 ≈ 82pt */
        NotificationCenter.default.addObserver(
            forName: PanelAppDelegate.cncToggleLog, object: nil, queue: .main
        ) { [weak self] note in
            guard let w = self?.window, let expanded = note.object as? Bool else { return }
            var f = w.frame
            let delta: CGFloat = expanded ? 82 : -82
            f.origin.y -= delta
            f.size.height += delta
            w.setFrame(f, display: true, animate: true)
        }
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        saveOrigin()
        return .terminateNow
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows: Bool) -> Bool {
        guard let w = window else { return true }
        if w.isMiniaturized { w.deminiaturize(nil) } /* 最小化的窗口要先展开 */
        w.makeKeyAndOrderFront(nil)
        return true
    }
}

@main
enum PanelMain {
    static func main() {
        let app = NSApplication.shared
        let delegate = PanelAppDelegate()
        app.delegate = delegate
        app.setActivationPolicy(.regular)
        delegate.setupMenu()
        app.run()
    }
}
