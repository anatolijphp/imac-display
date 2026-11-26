// One complete session with the iMac: extra display, video, sound, keyboard and mouse, drives.
// The app starts one when it finds the iMac and drops it when the session ends.
//
// The extra display behaves like a monitor: its resolution is chosen in macOS's Display settings,
// and the stream follows that choice while the session runs.
import AppKit
import CoreMedia

public final class DisplaySession {
    public struct Config {
        public var host: String
        public var port: UInt16 = Wire.port
        public var macName: String
        /// Largest picture to send; the capture scales the display down to it when the display has more pixels.
        public var maxStreamWidth = 3840
        public var maxStreamHeight = 2160
        /// Upper limit; the actual rate follows the picture size (about 10 Mbit/s at 1920×1080, 40 at 3840×2160).
        public var maxBitrate = 40_000_000
        public var fps = 60
        /// Add the extra display and send its picture.
        public var useDisplay = true
        /// Send this Mac's sound: "iMac Speakers" appears and becomes the Mac's sound output while connected.
        public var useSound = true
        public init(host: String, macName: String) { self.host = host; self.macName = macName }
    }

    public struct Stats {
        public var sent = 0, decoded = 0, dropped = 0
        public var decodeMs = 0.0, toScreenMs = 0.0
        public init() {}
    }

    public enum SoundSource { case none, speakersDevice, everything }

    /// Once a second while pictures flow. Any thread.
    public var onStats: ((Stats) -> Void)?
    /// Once, when the session is over for any reason. Any thread.
    public var onEnd: ((String) -> Void)?
    public var log: (String) -> Void = { print($0) }

    public private(set) var soundSource = SoundSource.none
    public private(set) var streamWidth = 0
    public private(set) var streamHeight = 0
    /// The desktop size picked in Display settings, in points.
    public private(set) var desktopWidth = 0
    public private(set) var desktopHeight = 0

    private let config: Config
    private var session: Session?
    private var display: VirtualDisplay?
    private var capture: DisplayCapture?
    private var encoder: H264Encoder?
    private var injector: InputInjector?
    private var mounter: StorageMounter?
    private var speakers: SpeakerDevice?
    private var screenObserver: NSObjectProtocol?
    private var lockObservers: [NSObjectProtocol] = []
    /// The Mac's screen is locked: macOS lets nobody capture it, so no picture is sent until it is unlocked.
    private var paused = false
    private var limitWidth = 3840, limitHeight = 2160
    private var screenAudio = false
    private var reconfiguring = false
    private let keyframeWanted = OneShot()
    private let sentCounter = Tally()
    private var audioPending = Data()
    private let lock = NSLock()
    private var finished = false
    private var ack: (ok: Bool, error: String)?   // the answer to the last CONFIG

    public init(_ config: Config) { self.config = config }

    /// Connects and starts everything. Throws when the iMac cannot be reached or refuses.
    @MainActor public func start() async throws {
        let c = config
        // Connect first: an extra display must not flash up for an iMac that is not there.
        let s = try await Task.detached { try Session(host: c.host, port: c.port, name: c.macName) }.value
        session = s
        let hello = s.imacHello
        let decode = hello["decode"] as? [String: Any] ?? [:]
        limitWidth = min(c.maxStreamWidth, decode["max_w"] as? Int ?? 3840)
        limitHeight = min(c.maxStreamHeight, decode["max_h"] as? Int ?? 2160)
        let features = hello["features"] as? [String] ?? []
        let audio = hello["audio"] as? [String: Any] ?? [:]

        if features.contains("input") { injector = InputInjector() }
        if features.contains("storage") {
            mounter = StorageMounter(host: s.host, sendEject: { [weak s] id in try? s?.ctrl.sendJSON(.storageEject, ["id": id]) },
                                     log: { [weak self] in self?.log("drives: \($0)") })
        }
        s.onMessage = { [weak self] h, p in self?.received(h, p) }
        s.onEnd = { [weak self] reason in self?.finish(reason) }
        s.start()   // from here on the once-a-second PING keeps the session alive

        if c.useDisplay {
            guard let vd = VirtualDisplay(name: "iMac Display", modes: VirtualDisplay.imacModes, hiDPI: true) else {
                throw CaptureError("macOS refused to create the extra display")
            }
            display = vd
            for _ in 0..<40 where vd.currentPixelSize == nil { try await Task.sleep(nanoseconds: 50_000_000) }
        }

        // Sound. With our driver installed, only what is played into "iMac Speakers"; otherwise everything.
        if c.useSound, audio["out"] as? Bool == true {
            if let dev = SpeakerDevice(), SpeakerDevice.microphonePermission == .authorized {
                dev.onAudio = { [weak self] in self?.pcm($0) }
                dev.log = { [weak self] in self?.log("sound: \($0)") }
                dev.onVolume = { [weak s] level, mute in try? s?.ctrl.sendJSON(.audioVolume, ["volume": Double(level), "mute": mute]) }
                speakers = dev
                soundSource = .speakersDevice
            } else if c.useDisplay {
                screenAudio = true            // comes with the screen capture, so only together with the display
                soundSource = .everything
            } else {
                log("sound: not sent. Without the display, sound needs the “iMac Speakers” device and its permission.")
            }
        }

        if c.useDisplay && Self.screenIsLocked {
            // Connected while the screen is locked (for example right after waking up): everything
            // but the picture starts now, the picture when the user unlocks.
            paused = true
            log("the screen is locked: the picture starts when it is unlocked")
            if let px = display?.currentPixelSize { (streamWidth, streamHeight) = streamSize(for: px) }
            try await configure(width: max(streamWidth, 1280), height: max(streamHeight, 720))
        } else if c.useDisplay {
            try await startStream()
        } else {
            // Sound, keyboard, mouse and drives only. The receiver still wants a video size in CONFIG
            // (it starts its sound player on CONFIG); no picture is sent.
            try await configure(width: 1280, height: 720)
        }
        try speakers?.start()   // after the first CONFIG: the iMac starts its player on CONFIG

        if c.useDisplay {
            let center = DistributedNotificationCenter.default()
            lockObservers = [
                center.addObserver(forName: Notification.Name("com.apple.screenIsLocked"), object: nil, queue: .main) { [weak self] _ in
                    Task { @MainActor in await self?.pause() }
                },
                center.addObserver(forName: Notification.Name("com.apple.screenIsUnlocked"), object: nil, queue: .main) { [weak self] _ in
                    Task { @MainActor in await self?.resume() }
                },
            ]
        }

        // The user changed the resolution in Display settings (or rearranged displays): follow.
        screenObserver = NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main
        ) { [weak self] _ in Task { @MainActor in await self?.displayChanged() } }
    }

    /// Ends the session from our side (quit, settings changed).
    public func stop(reason: String = "quit") { finish(reason) }

    // MARK: video

    /// The picture to send for a display of this many pixels: the same shape, at most the iMac's limit, even numbers.
    private func streamSize(for pixels: (width: Int, height: Int)) -> (Int, Int) {
        let scale = min(1.0, Double(limitWidth) / Double(pixels.width), Double(limitHeight) / Double(pixels.height))
        return (Int(Double(pixels.width) * scale) / 2 * 2, Int(Double(pixels.height) * scale) / 2 * 2)
    }

    /// Tells the iMac the picture size, then starts encoder and capture for the display's current resolution.
    @MainActor private func startStream() async throws {
        guard let vd = display, let s = session, let pixels = vd.currentPixelSize else { throw CaptureError("the extra display did not appear") }
        let c = config
        let (w, h) = streamSize(for: pixels)
        streamWidth = w; streamHeight = h
        if let pt = vd.currentPointSize { desktopWidth = pt.width; desktopHeight = pt.height }

        try await configure(width: w, height: h)
        if s.video == nil { try await Task.detached { try s.openVideo() }.value }

        var enc = H264Encoder.Config(width: w, height: h, fps: c.fps, bitrate: min(c.maxBitrate, max(6_000_000, Int(Double(w * h) * 4.8))))
        enc.lowLatency = true
        let encoder = try H264Encoder(enc)
        self.encoder = encoder
        encoder.onOutput = { [weak self] out in
            guard let self, let session = self.session else { return }
            do {
                try session.sendVideo(tile: 0, keyframe: out.keyframe, lastTile: true, ptsUs: out.ptsUs, data: out.annexB)
                self.sentCounter.add()
            } catch { self.finish("\(error)") }
        }

        let capture = DisplayCapture()
        var first = true
        capture.onFrame = { [weak self] pixelBuffer, _ in
            guard let self else { return }
            let key = first || self.keyframeWanted.take()
            first = false
            encoder.encode(pixelBuffer, ptsUs: Wire.nowUs(), forceKeyframe: key)
        }
        if screenAudio { capture.onAudio = { [weak self] in self?.pcm($0) } }
        capture.onStop = { [weak self, weak capture] error in
            // macOS ended the capture by itself (it does when the screen locks, and sometimes after sleep).
            Task { @MainActor in
                guard let self, let capture, self.capture === capture else { return }
                self.log("the screen capture stopped (\(error.localizedDescription))")
                await self.restartStream()
            }
        }
        // A new display takes a moment to become capturable.
        var lastError: Error = CaptureError("the extra display did not appear")
        for _ in 0..<20 {
            do {
                // Twice the frame rate as the minimum interval: asking for exactly 1/60 s loses pictures.
                try await capture.start(displayID: vd.displayID, width: w, height: h, fps: c.fps * 2, audio: screenAudio)
                self.capture = capture
                log("display \(desktopWidth)x\(desktopHeight), sending \(w)x\(h) at \(enc.bitrate / 1_000_000) Mbit/s")
                return
            } catch {
                lastError = error
                try await Task.sleep(nanoseconds: 250_000_000)
            }
        }
        throw lastError
    }

    /// Sends CONFIG and waits for the iMac's answer.
    @MainActor private func configure(width w: Int, height h: Int) async throws {
        guard let s = session else { throw CaptureError("no session") }
        setAck(nil)
        try s.sendConfig([
            "video": ["codec": "h264", "fps": config.fps, "w": w, "h": h, "scale": "fit",
                      "tiles": [["id": 0, "x": 0, "y": 0, "w": w, "h": h]]] as [String: Any],
            "audio_out": ["rate": 48000, "ch": 2, "fmt": "s16le"],
            "audio_in": ["enabled": false, "rate": 48000, "ch": 1, "fmt": "s16le"],
        ])
        var answer: (ok: Bool, error: String)?
        for _ in 0..<150 {   // 3 s
            answer = currentAck
            if answer != nil || isFinished { break }
            try await Task.sleep(nanoseconds: 20_000_000)
        }
        guard let answer else { throw CaptureError("the iMac did not answer CONFIG") }
        guard answer.ok else { throw CaptureError("the iMac refused \(w)x\(h): \(answer.error)") }
    }

    @MainActor private func displayChanged() async {
        guard !isFinished, !reconfiguring, !paused, !Self.screenIsLocked,
              let vd = display, let pixels = vd.currentPixelSize else { return }
        let (w, h) = streamSize(for: pixels)
        if let pt = vd.currentPointSize { desktopWidth = pt.width; desktopHeight = pt.height }
        guard w != streamWidth || h != streamHeight else { return }
        reconfiguring = true
        log("resolution changed in Display settings")
        await stopStream()
        try? await Task.sleep(nanoseconds: 150_000_000)   // let the last pictures of the old size arrive first
        await startStreamOrGiveUp()
        reconfiguring = false
        await displayChanged()   // in case it changed again meanwhile
    }

    /// Starts the picture; if that fails while the screen is locked it simply waits for the unlock,
    /// otherwise it tries once more before ending the session.
    @MainActor private func startStreamOrGiveUp() async {
        for attempt in 1...2 {
            if isFinished { return }
            if Self.screenIsLocked { paused = true; return }
            do { try await startStream(); return } catch {
                if Self.screenIsLocked { paused = true; return }
                if attempt == 2 { finish("\(error)"); return }
                log("could not start the picture (\(error)); trying again")
                try? await Task.sleep(nanoseconds: 1_000_000_000)
            }
        }
    }

    @MainActor private func stopStream() async {
        let old = capture
        capture = nil            // first, so that its stop handler does not restart it
        await old?.stop()
        encoder?.flush()
        encoder = nil
    }

    /// The capture ended by itself: start it again (or wait for the unlock).
    @MainActor private func restartStream() async {
        guard !isFinished, !reconfiguring, !paused else { return }
        reconfiguring = true
        await stopStream()
        await startStreamOrGiveUp()
        reconfiguring = false
    }

    /// The screen was locked. macOS does not let the lock screen be captured, and the last picture must
    /// not stay on the iMac for everyone to read: send nothing, and make the iMac drop the picture.
    @MainActor private func pause() async {
        guard !isFinished, !paused, config.useDisplay else { return }
        paused = true
        while reconfiguring { try? await Task.sleep(nanoseconds: 50_000_000) }
        guard !isFinished else { return }
        log("screen locked: picture paused")
        await stopStream()
        // A CONFIG with no picture after it makes the receiver go black and then back to its waiting screen.
        try? await configure(width: max(streamWidth, 1280), height: max(streamHeight, 720))
    }

    @MainActor private func resume() async {
        guard !isFinished, paused else { return }
        paused = false
        while reconfiguring { try? await Task.sleep(nanoseconds: 50_000_000) }
        guard !isFinished, capture == nil else { return }
        log("screen unlocked: picture back on")
        reconfiguring = true
        try? await Task.sleep(nanoseconds: 300_000_000)   // the displays settle for a moment after the unlock
        await startStreamOrGiveUp()
        reconfiguring = false
        await displayChanged()
    }

    /// True while the Mac shows its lock screen.
    public static var screenIsLocked: Bool {
        (CGSessionCopyCurrentDictionary() as? [String: Any])?["CGSSessionScreenIsLocked"] as? Bool ?? false
    }

    // MARK: messages from the iMac

    private func received(_ h: FrameHeader, _ p: Data) {
        guard let type = MsgType(rawValue: h.type) else { return }
        switch type {
        case .inputKey, .inputPointerRel, .inputButton:
            injector?.handle(type: type, payload: p)
        case .storageAttached, .storageDetached:
            guard let j = (try? JSONSerialization.jsonObject(with: p)) as? [String: Any] else { return }
            if type == .storageAttached { mounter?.attached(j) } else { mounter?.detached(j) }
        case .configAck:
            let j = (try? JSONSerialization.jsonObject(with: p)) as? [String: Any] ?? [:]
            setAck((j["ok"] as? Bool == true, j["error"] as? String ?? "no reason given"))
        case .videoRequestKeyframe:
            keyframeWanted.set()
        case .videoStats:
            guard let j = (try? JSONSerialization.jsonObject(with: p)) as? [String: Any] else { return }
            var st = Stats()
            st.sent = sentCounter.take()
            st.decoded = j["decoded"] as? Int ?? 0
            st.dropped = j["dropped"] as? Int ?? 0
            st.decodeMs = j["decode_ms"] as? Double ?? 0
            st.toScreenMs = j["present_ms"] as? Double ?? 0
            if st.decoded == 0 && st.sent > 0 { keyframeWanted.set() }   // the iMac's decoder lost its place
            onStats?(st)
        default: break
        }
    }

    /// 10 ms per AUDIO_OUT message. Called on one queue at a time.
    private func pcm(_ data: Data) {
        audioPending.append(data)
        while audioPending.count >= 1920 {
            try? session?.ctrl.send(.audioOut, payload: audioPending.prefix(1920))
            audioPending.removeFirst(1920)
        }
    }

    private func setAck(_ value: (ok: Bool, error: String)?) { lock.lock(); ack = value; lock.unlock() }
    private var currentAck: (ok: Bool, error: String)? { lock.lock(); defer { lock.unlock() }; return ack }

    private var isFinished: Bool { lock.lock(); defer { lock.unlock() }; return finished }

    private func finish(_ reason: String) {
        lock.lock()
        let firstTime = !finished
        finished = true
        lock.unlock()
        guard firstTime else { return }
        let capture = self.capture, display = self.display, observer = screenObserver
        for o in lockObservers { DistributedNotificationCenter.default().removeObserver(o) }
        lockObservers = []
        speakers?.stop()          // puts the Mac's sound output back and makes "iMac Speakers" disappear
        injector?.releaseAll()
        mounter?.unmountAll()
        session?.close(reason: reason)   // sends BYE if the connection is still up
        Task { @MainActor in
            if let observer { NotificationCenter.default.removeObserver(observer) }
            await capture?.stop()
            withExtendedLifetime(display) {}   // let go of the extra display only after the capture has stopped
        }
        self.capture = nil
        self.display = nil
        self.encoder = nil
        self.screenObserver = nil
        onEnd?(reason)
    }
}

/// Thread-safe one-shot flag.
final class OneShot {
    private var value = false
    private let lock = NSLock()
    func set() { lock.lock(); value = true; lock.unlock() }
    func take() -> Bool { lock.lock(); defer { value = false; lock.unlock() }; return value }
}

/// Thread-safe counter, read and reset together.
final class Tally {
    private var value = 0
    private let lock = NSLock()
    func add() { lock.lock(); value += 1; lock.unlock() }
    func take() -> Int { lock.lock(); defer { value = 0; lock.unlock() }; return value }
}
