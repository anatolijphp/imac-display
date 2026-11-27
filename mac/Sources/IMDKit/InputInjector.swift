// Turns the iMac's INPUT_* messages (PROTOCOL.md) into keyboard and mouse events on this Mac.
// The keyboard and mouse plugged into the iMac then behave like ones plugged into the MacBook.
// Needs the Accessibility permission (System Settings > Privacy & Security > Accessibility).
import AppKit
import CoreGraphics

public final class InputInjector {
    /// Pointer speed: counts from the mouse are multiplied by this, and more when the mouse moves fast.
    public var pointerSpeed = 1.0
    /// Lines scrolled per wheel notch.
    public var linesPerNotch = 3.0
    public private(set) var keys = 0, moves = 0, clicks = 0
    /// The iMac sends key repeats itself (its "key-repeat" feature): then no repeat timer runs here, and
    /// a release that arrives late (a network stall) cannot make this Mac type extra letters.
    public var remoteRepeat = false
    /// For the app's log: only trouble with modifier keys, never which letter was typed.
    public var log: (String) -> Void = { _ in }

    private let queue = DispatchQueue(label: "imd.input", qos: .userInteractive)
    private let source = CGEventSource(stateID: .hidSystemState)
    private var modifiers: CGEventFlags = []          // from heldModifiers
    private var heldModifiers: Set<UInt16> = []       // HID usages 0xE0–0xE7 held on the iMac (left and right apart)
    private var watch: DispatchSourceTimer?
    private var strayFor: [UInt64: Int] = [:]         // flag → seconds the Mac has shown it down without a key on the iMac
    private var everPressed: Set<UInt64> = []         // flags pressed from the iMac in this session
    private var capsLock = false
    private var buttonsDown: Set<Int> = []
    private var keysDown: Set<CGKeyCode> = []
    private var repeatTimer: DispatchSourceTimer?
    private var repeatingKey: CGKeyCode?
    private var lastClick: (button: Int, time: TimeInterval, at: CGPoint, count: Int64) = (0, 0, .zero, 0)
    private var wheelRemainder = (v: 0.0, h: 0.0)

    public init() {}

    public static var hasPermission: Bool { CGPreflightPostEventAccess() }
    /// Shows the system prompt the first time.
    @discardableResult public static func requestPermission() -> Bool { CGRequestPostEventAccess() }

    /// `payload` is the body of an INPUT_KEY, INPUT_POINTER_REL or INPUT_BUTTON message.
    public func handle(type: MsgType, payload: Data) {
        let b = [UInt8](payload)
        queue.async { [self] in
            switch type {
            case .inputKey where b.count >= 5:
                let page = UInt16(b[0]) | UInt16(b[1]) << 8, usage = UInt16(b[2]) | UInt16(b[3]) << 8
                if b[4] == 2 { repeated(page: page, usage: usage) } else { key(page: page, usage: usage, down: b[4] != 0) }
            case .inputPointerRel where b.count >= 16:
                func i32(_ o: Int) -> Int { Int(Int32(bitPattern: UInt32(b[o]) | UInt32(b[o + 1]) << 8 | UInt32(b[o + 2]) << 16 | UInt32(b[o + 3]) << 24)) }
                pointer(dx: i32(0), dy: i32(4), wheelV: i32(8), wheelH: i32(12))
            case .inputButton where b.count >= 2:
                button(Int(b[0]), down: b[1] != 0)
            default: break
            }
        }
    }

    /// What is held, as the iMac's keyboards say and as this Mac shows it. For the log; names modifier
    /// keys only, other keys are counted.
    public func stateSummary() -> String {
        queue.sync { [self] in
            let all: [CGEventFlags] = [.maskCommand, .maskShift, .maskControl, .maskAlternate]
            func names(_ f: CGEventFlags) -> String { let n = all.filter { f.contains($0) }.map { Self.name($0) }; return n.isEmpty ? "no modifier" : n.joined(separator: "+") }
            let system = CGEventSource.flagsState(.hidSystemState)
            let buttons = [CGMouseButton.left, .right, .center].filter { CGEventSource.buttonState(.hidSystemState, button: $0) }.count
            return "held on the iMac: \(names(modifiers)), \(keysDown.count) other key(s), \(buttonsDown.count) button(s); this Mac shows: \(names(system)), \(buttons) button(s)"
        }
    }

    /// Lets go of everything that is still held (end of session).
    public func releaseAll() {
        queue.sync { [self] in
            stopRepeat()
            for code in keysDown { postKey(code, down: false, flagsAfter: modifiers) }
            keysDown = []
            for (usage, flag) in Self.modifierFlags where modifiers.contains(flag) {
                if let code = Self.keyCodes[usage] { postKey(code, down: false, flagsAfter: modifiers.subtracting(flag)) }
                modifiers.remove(flag)
            }
            heldModifiers = []
            watch?.cancel()
            watch = nil
            for bn in buttonsDown { postButton(bn, down: false) }
            buttonsDown = []
        }
    }

    // MARK: keyboard

    private func key(page: UInt16, usage: UInt16, down: Bool) {
        keys += 1
        if page == 0x0C { mediaKey(usage, down: down); return }
        guard page == 0x07, let code = Self.keyCodes[usage] else { return }
        if let flag = Self.modifierFlags[usage] {
            // Left and right keys share a flag: it stays while the other one is held.
            if down { heldModifiers.insert(usage); everPressed.insert(flag.rawValue) } else { heldModifiers.remove(usage) }
            modifiers = Self.flags(of: heldModifiers)
            postKey(code, down: down, flagsAfter: modifiers)
            if !down, !modifiers.contains(flag) { verifyReleased(flag, code: code, attempt: 1) }
            startWatch()
            return
        }
        if usage == 0x39 {   // Caps Lock toggles on key down
            if down { capsLock.toggle() }
            postKey(code, down: down, flagsAfter: modifiers)
            return
        }
        if down { keysDown.insert(code) } else { keysDown.remove(code) }
        postKey(code, down: down, flagsAfter: modifiers)
        if remoteRepeat { return }
        if down { startRepeat(code) } else if repeatingKey == code { stopRepeat() }
    }

    /// A repeat made by the iMac while the key is held there. Ignored for a key this Mac does not hold.
    private func repeated(page: UInt16, usage: UInt16) {
        guard remoteRepeat, page == 0x07, let code = Self.keyCodes[usage], keysDown.contains(code) else { return }
        postKey(code, down: true, flagsAfter: modifiers, autorepeat: true)
    }

    private static func flags(of usages: Set<UInt16>) -> CGEventFlags {
        usages.reduce(into: CGEventFlags()) { if let f = modifierFlags[$1] { $0.insert(f) } }
    }

    static func name(_ flag: CGEventFlags) -> String {
        switch flag {
        case .maskCommand: return "Command"
        case .maskShift: return "Shift"
        case .maskControl: return "Control"
        case .maskAlternate: return "Option"
        default: return "a modifier"
        }
    }

    /// A released modifier that the Mac still shows as down would turn every following key into a
    /// shortcut. Check shortly after posting the release, and post it again if it did not take.
    private func verifyReleased(_ flag: CGEventFlags, code: CGKeyCode, attempt: Int) {
        queue.asyncAfter(deadline: .now() + 0.25) { [self] in
            guard !modifiers.contains(flag), CGEventSource.flagsState(.hidSystemState).contains(flag) else { return }
            log("\(Self.name(flag)) was released on the iMac but this Mac still shows it down 0.25 s later; releasing it again (try \(attempt))")
            postKey(code, down: false, flagsAfter: modifiers)
            if attempt < 3 { verifyReleased(flag, code: code, attempt: attempt + 1) }
        }
    }

    /// Once a second: a modifier that this Mac shows as down while no key on the iMac holds it. That is
    /// either a key held on the MacBook's own keyboard or a stuck one; it is logged once, after 3 s.
    private func startWatch() {
        guard watch == nil else { return }
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 1, repeating: 1)
        t.setEventHandler { [weak self] in
            guard let self else { return }
            let system = CGEventSource.flagsState(.hidSystemState)
            for flag in [CGEventFlags.maskCommand, .maskShift, .maskControl, .maskAlternate] {
                if system.contains(flag) && !self.modifiers.contains(flag) {
                    self.strayFor[flag.rawValue, default: 0] += 1
                    if self.strayFor[flag.rawValue] == 3 {
                        // Only one this session pressed itself: a key held on the MacBook's own keyboard is left alone.
                        let ours = self.everPressed.contains(flag.rawValue)
                        self.log("this Mac has shown \(Self.name(flag)) down for 3 s while no key on the iMac holds it" + (ours ? "; releasing it" : " (not pressed from the iMac in this session: left alone)"))
                        guard ours else { continue }
                        for (usage, f) in Self.modifierFlags where f == flag {
                            if let code = Self.keyCodes[usage] { self.postKey(code, down: false, flagsAfter: self.modifiers) }
                        }
                        self.queue.asyncAfter(deadline: .now() + 0.25) {
                            let still = CGEventSource.flagsState(.hidSystemState).contains(flag)
                            self.log(still ? "\(Self.name(flag)) is still down after that release" : "\(Self.name(flag)) is released now")
                        }
                    }
                } else if let n = self.strayFor.removeValue(forKey: flag.rawValue), n >= 3 {
                    self.log("\(Self.name(flag)) is no longer down on this Mac (after \(n) s)")
                }
            }
        }
        t.resume()
        watch = t
    }

    private func postKey(_ code: CGKeyCode, down: Bool, flagsAfter: CGEventFlags, autorepeat: Bool = false) {
        guard let e = CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: down) else {
            log("could not create a key event (\(down ? "press" : "release"))")
            return
        }
        var flags = e.flags.subtracting([.maskShift, .maskControl, .maskAlternate, .maskCommand, .maskAlphaShift]).union(flagsAfter)
        if capsLock { flags.insert(.maskAlphaShift) }
        e.flags = flags
        if autorepeat { e.setIntegerValueField(.keyboardEventAutorepeat, value: 1) }
        e.post(tap: .cghidEventTap)
    }

    private func startRepeat(_ code: CGKeyCode) {
        stopRepeat()
        repeatingKey = code
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + NSEvent.keyRepeatDelay, repeating: max(NSEvent.keyRepeatInterval, 0.015))
        t.setEventHandler { [weak self] in
            guard let self, self.repeatingKey == code else { return }
            self.postKey(code, down: true, flagsAfter: self.modifiers, autorepeat: true)
        }
        t.resume()
        repeatTimer = t
    }

    private func stopRepeat() {
        repeatTimer?.cancel()
        repeatTimer = nil
        repeatingKey = nil
    }

    /// Volume, mute, play/pause, next, previous, brightness, eject: macOS "system defined" key events.
    private func mediaKey(_ usage: UInt16, down: Bool) {
        guard let nx = Self.mediaKeys[usage] else { return }
        let state = down ? 0xA : 0xB
        let e = NSEvent.otherEvent(with: .systemDefined, location: .zero,
                                   modifierFlags: NSEvent.ModifierFlags(rawValue: UInt(state << 8)),
                                   timestamp: 0, windowNumber: 0, context: nil, subtype: 8,
                                   data1: (nx << 16) | (state << 8), data2: -1)
        e?.cgEvent?.post(tap: .cghidEventTap)
    }

    // MARK: pointer

    private func pointer(dx: Int, dy: Int, wheelV: Int, wheelH: Int) {
        if dx != 0 || dy != 0 {
            moves += 1
            // Simple acceleration: slow movements are 1:1, fast ones travel further.
            let speed = (Double(dx * dx + dy * dy)).squareRoot()
            let gain = pointerSpeed * min(1.0 + speed * 0.06, 3.0)
            let here = CGEvent(source: nil)?.location ?? .zero
            let target = clamp(CGPoint(x: here.x + Double(dx) * gain, y: here.y + Double(dy) * gain), from: here)
            let (type, btn): (CGEventType, CGMouseButton) =
                buttonsDown.contains(1) ? (.leftMouseDragged, .left) :
                buttonsDown.contains(2) ? (.rightMouseDragged, .right) :
                buttonsDown.isEmpty ? (.mouseMoved, .left) : (.otherMouseDragged, .center)
            if let e = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: target, mouseButton: btn) {
                e.setIntegerValueField(.mouseEventDeltaX, value: Int64((Double(dx) * gain).rounded()))
                e.setIntegerValueField(.mouseEventDeltaY, value: Int64((Double(dy) * gain).rounded()))
                e.flags = e.flags.union(modifiers)
                e.post(tap: .cghidEventTap)
            }
        }
        if wheelV != 0 || wheelH != 0 {
            // 120 units per notch (PROTOCOL.md). Positive v = wheel away from the user = scroll up.
            // Positive h = right; Quartz counts horizontal scrolling the other way round.
            wheelRemainder.v += Double(wheelV) / 120 * linesPerNotch
            wheelRemainder.h += Double(-wheelH) / 120 * linesPerNotch
            let v = Int32(wheelRemainder.v.rounded(.towardZero)), h = Int32(wheelRemainder.h.rounded(.towardZero))
            wheelRemainder.v -= Double(v); wheelRemainder.h -= Double(h)
            if v != 0 || h != 0, let e = CGEvent(scrollWheelEvent2Source: source, units: .line, wheelCount: 2, wheel1: v, wheel2: h, wheel3: 0) {
                e.flags = e.flags.union(modifiers)
                e.post(tap: .cghidEventTap)
            }
        }
    }

    /// Keeps the pointer on a screen: if the new point is on no display, stay inside the one it came from.
    private func clamp(_ p: CGPoint, from old: CGPoint) -> CGPoint {
        var ids = [CGDirectDisplayID](repeating: 0, count: 16)
        var n: UInt32 = 0
        CGGetActiveDisplayList(16, &ids, &n)
        let bounds = ids.prefix(Int(n)).map { CGDisplayBounds($0) }
        if bounds.contains(where: { $0.contains(p) }) { return p }
        guard let home = bounds.first(where: { $0.contains(old) }) ?? bounds.first else { return p }
        return CGPoint(x: min(max(p.x, home.minX), home.maxX - 1), y: min(max(p.y, home.minY), home.maxY - 1))
    }

    private func button(_ n: Int, down: Bool) {
        clicks += 1
        if down { buttonsDown.insert(n) } else { buttonsDown.remove(n) }
        postButton(n, down: down)
    }

    private func postButton(_ n: Int, down: Bool) {
        let at = CGEvent(source: nil)?.location ?? .zero
        let (type, btn): (CGEventType, CGMouseButton)
        switch n {
        case 1: (type, btn) = (down ? .leftMouseDown : .leftMouseUp, .left)
        case 2: (type, btn) = (down ? .rightMouseDown : .rightMouseUp, .right)
        default: (type, btn) = (down ? .otherMouseDown : .otherMouseUp, .center)
        }
        if down {   // double and triple clicks: same button, soon enough, close enough
            let now = ProcessInfo.processInfo.systemUptime
            let again = lastClick.button == n && now - lastClick.time < NSEvent.doubleClickInterval
                && abs(at.x - lastClick.at.x) < 5 && abs(at.y - lastClick.at.y) < 5
            lastClick = (n, now, at, again ? lastClick.count + 1 : 1)
        }
        guard let e = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: at, mouseButton: btn) else { return }
        if n > 3 { e.setIntegerValueField(.mouseEventButtonNumber, value: Int64(n - 1)) }   // 4, 5 → back, forward
        e.setIntegerValueField(.mouseEventClickState, value: lastClick.button == n ? lastClick.count : 1)
        e.flags = e.flags.union(modifiers)
        e.post(tap: .cghidEventTap)
    }

    // MARK: tables

    /// USB HID keyboard usage (page 0x07) → macOS virtual key code.
    static let keyCodes: [UInt16: CGKeyCode] = {
        var m: [UInt16: CGKeyCode] = [:]
        let letters: [CGKeyCode] = [0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2E, 0x2D, 0x1F, 0x23,
                                    0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06]          // a … z
        for (i, c) in letters.enumerated() { m[UInt16(0x04 + i)] = c }
        let digits: [CGKeyCode] = [0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19, 0x1D]            // 1 … 9, 0
        for (i, c) in digits.enumerated() { m[UInt16(0x1E + i)] = c }
        let fkeys: [CGKeyCode] = [0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F] // F1 … F12
        for (i, c) in fkeys.enumerated() { m[UInt16(0x3A + i)] = c }
        let keypad: [CGKeyCode] = [0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B, 0x5C, 0x52]            // keypad 1 … 9, 0
        for (i, c) in keypad.enumerated() { m[UInt16(0x59 + i)] = c }
        let rest: [UInt16: CGKeyCode] = [
            0x28: 0x24, 0x29: 0x35, 0x2A: 0x33, 0x2B: 0x30, 0x2C: 0x31,   // return, escape, backspace, tab, space
            0x2D: 0x1B, 0x2E: 0x18, 0x2F: 0x21, 0x30: 0x1E, 0x31: 0x2A, 0x32: 0x2A,   // - = [ ] \ (and non-US #)
            0x33: 0x29, 0x34: 0x27, 0x35: 0x32, 0x36: 0x2B, 0x37: 0x2F, 0x38: 0x2C,   // ; ' ` , . /
            0x39: 0x39,                                                   // caps lock
            0x46: 0x69, 0x47: 0x6B, 0x48: 0x71,                           // print screen, scroll lock, pause → F13, F14, F15
            0x49: 0x72, 0x4A: 0x73, 0x4B: 0x74, 0x4C: 0x75, 0x4D: 0x77, 0x4E: 0x79,   // insert(help), home, page up, delete, end, page down
            0x4F: 0x7C, 0x50: 0x7B, 0x51: 0x7D, 0x52: 0x7E,               // right, left, down, up
            0x53: 0x47, 0x54: 0x4B, 0x55: 0x43, 0x56: 0x4E, 0x57: 0x45, 0x58: 0x4C, 0x63: 0x41, 0x67: 0x51,   // keypad
            0x64: 0x0A, 0x65: 0x6E,                                       // ISO key next to left shift, menu key
            0xE0: 0x3B, 0xE1: 0x38, 0xE2: 0x3A, 0xE3: 0x37,               // left control, shift, option (Alt), command (Windows key)
            0xE4: 0x3E, 0xE5: 0x3C, 0xE6: 0x3D, 0xE7: 0x36,               // right control, shift, option, command
        ]
        m.merge(rest) { a, _ in a }
        return m
    }()

    static let modifierFlags: [UInt16: CGEventFlags] = [
        0xE0: .maskControl, 0xE1: .maskShift, 0xE2: .maskAlternate, 0xE3: .maskCommand,
        0xE4: .maskControl, 0xE5: .maskShift, 0xE6: .maskAlternate, 0xE7: .maskCommand,
    ]

    /// USB HID consumer usage (page 0x0C) → macOS media key number (NX_KEYTYPE_*).
    static let mediaKeys: [UInt16: Int] = [
        0xE9: 0, 0xEA: 1, 0xE2: 7,        // volume up, volume down, mute
        0x6F: 2, 0x70: 3,                 // brightness up, down
        0xCD: 16, 0xB5: 17, 0xB6: 18,     // play/pause, next, previous
        0xB8: 14,                         // eject
    ]
}
