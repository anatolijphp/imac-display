// Moving test content for a display: two columns of scrolling text (the hard case for a video
// encoder) and a bouncing box. Animated by the window server, not by this process.
import AppKit
import QuartzCore

@MainActor public final class TestScene {
    private let window: NSWindow

    public init(screen: NSScreen) {
        let size = screen.frame.size
        window = NSWindow(contentRect: screen.frame, styleMask: .borderless, backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.setFrame(screen.frame, display: false)
        let view = NSView(frame: NSRect(origin: .zero, size: size))
        view.wantsLayer = true
        let root = view.layer!
        root.backgroundColor = NSColor(white: 0.98, alpha: 1).cgColor
        root.masksToBounds = true
        let scale = screen.backingScaleFactor

        let font = NSFont.monospacedSystemFont(ofSize: 13, weight: .regular)
        let lineHeight: CGFloat = 17
        let lines = Int(size.height * 2 / lineHeight)
        let words = ["let", "frame", "= encoder.encode(", "tile", "pts:", "guard", "return", "// scrolls", "for i in 0..<", "{", "}",
                     "display", "thunderbolt0", "bridge0", "0x10", "VIDEO_FRAME", "keyframe", "if", "else", "nil", "5120", "2880"]
        var seed: UInt64 = 42
        func next() -> Int { seed = seed &* 6364136223846793005 &+ 1442695040888963407; return Int(seed >> 33) }
        for column in 0..<2 {
            var text = ""
            for n in 0..<lines {
                text += String(format: "%4d  ", n + 1) + String(repeating: "  ", count: next() % 4)
                for _ in 0..<(4 + next() % 12) { text += words[next() % words.count] + " " }
                text += "\n"
            }
            let para = NSMutableParagraphStyle()
            para.minimumLineHeight = lineHeight; para.maximumLineHeight = lineHeight
            let layer = CATextLayer()
            layer.string = NSAttributedString(string: text, attributes: [
                .font: font, .foregroundColor: NSColor(white: 0.1, alpha: 1), .paragraphStyle: para])
            layer.contentsScale = scale
            layer.anchorPoint = .zero
            let w = size.width / 2 - 30
            layer.frame = CGRect(x: 20 + CGFloat(column) * size.width / 2, y: -size.height, width: w, height: size.height * 2)
            root.addSublayer(layer)
            let scroll = CABasicAnimation(keyPath: "position.y")
            scroll.fromValue = -size.height
            scroll.toValue = 0
            scroll.duration = Double(size.height) / (column == 0 ? 120 : 400)   // points per second: reading, flicking
            scroll.repeatCount = .infinity
            layer.add(scroll, forKey: "scroll")
        }

        let box = CALayer()
        box.backgroundColor = NSColor.systemBlue.cgColor
        box.cornerRadius = 12
        box.frame = CGRect(x: 0, y: size.height * 0.45, width: 320, height: 200)
        root.addSublayer(box)
        let bounce = CABasicAnimation(keyPath: "position.x")
        bounce.fromValue = 160
        bounce.toValue = size.width - 160
        bounce.duration = 3
        bounce.autoreverses = true
        bounce.repeatCount = .infinity
        box.add(bounce, forKey: "bounce")

        window.contentView = view
        window.orderFrontRegardless()
    }

    public func close() { window.orderOut(nil) }
}
