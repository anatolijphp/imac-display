// One complete session with the iMac: extra display, video, sound, keyboard and mouse, drives.
// The app starts one when it finds the iMac and drops it when the session ends.
import AppKit
import CoreMedia

public final class DisplaySession {
    public struct Config {
        public var host: String
        public var port: UInt16 = Wire.port
        public var macName: String
        public var pointsWide = 1920
        public var pointsHigh = 1080
        public var hiDPI = false
        /// Largest picture to send; the capture scales the display down to it when the display has more pixels.
        public var maxStreamWidth = 3840
        public var maxStreamHeight = 2160
        public var bitrate = 10_000_000
        public var fps = 60
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

    private let config: Config
    private var session: Session?
    private var display: VirtualDisplay?
    private var capture: DisplayCapture?
    private var encoder: H264Encoder?
    private var injector: InputInjector?
    private var mounter: StorageMounter?
    private var speakers: SpeakerDevice?
    private let keyframeWanted = OneShot()
    private let sentCounter = Tally()
    private var audioPending = Data()
    private let lock = NSLock()
    private var finished = false

    public init(_ config: Config) { self.config = config }

    /// Connects and starts everything. Throws when the iMac cannot be reached or refuses.
    @MainActor public func start() async throws {
        let c = config
        // Connect first: an extra display must not flash up for an iMac that is not there.
        let s = try await Task.detached { try Session(host: c.host, port: c.port, name: c.macName) }.value
        session = s
        let hello = s.imacHello
        let decode = hello["decode"] as? [String: Any] ?? [:]
        let maxW = min(c.maxStreamWidth, decode["max_w"] as? Int ?? 3840)
        let maxH = min(c.maxStreamHeight, decode["max_h"] as? Int ?? 2160)
        let pixelsW = c.pointsWide * (c.hiDPI ? 2 : 1), pixelsH = c.pointsHigh * (c.hiDPI ? 2 : 1)
        let scale = min(1.0, Double(maxW) / Double(pixelsW), Double(maxH) / Double(pixelsH))
        streamWidth = Int(Double(pixelsW) * scale) / 2 * 2
        streamHeight = Int(Double(pixelsH) * scale) / 2 * 2
        let features = hello["features"] as? [String] ?? []
        let audio = hello["audio"] as? [String: Any] ?? [:]

        s.onMessage = { [weak self] h, p in self?.received(h, p) }
        s.onEnd = { [weak self] reason in self?.finish(reason, sendBye: false) }
        try s.configure([
            "video": ["codec": "h264", "fps": c.fps, "w": streamWidth, "h": streamHeight, "scale": "fit",
                      "tiles": [["id": 0, "x": 0, "y": 0, "w": streamWidth, "h": streamHeight]]] as [String: Any],
            "audio_out": ["rate": 48000, "ch": 2, "fmt": "s16le"],
            "audio_in": ["enabled": false, "rate": 48000, "ch": 1, "fmt": "s16le"],
        ])
        try s.openVideo()

        if features.contains("input") { injector = InputInjector() }
        if features.contains("storage") {
            mounter = StorageMounter(host: s.host, sendEject: { [weak s] id in try? s?.ctrl.sendJSON(.storageEject, ["id": id]) },
                                     log: { [weak self] in self?.log("drives: \($0)") })
        }
        s.start()   // from here on the once-a-second PING keeps the session alive

        var enc = H264Encoder.Config(width: streamWidth, height: streamHeight, fps: c.fps, bitrate: c.bitrate)
        enc.lowLatency = true
        let encoder = try H264Encoder(enc)
        self.encoder = encoder
        encoder.onOutput = { [weak self] out in
            guard let self, let session = self.session else { return }
            do {
                try session.sendVideo(tile: 0, keyframe: out.keyframe, lastTile: true, ptsUs: out.ptsUs, data: out.annexB)
                self.sentCounter.add()
            } catch { self.finish("\(error)", sendBye: false) }
        }

        guard let vd = VirtualDisplay(name: "iMac Display", pointsWide: c.pointsWide, pointsHigh: c.pointsHigh, hiDPI: c.hiDPI) else {
            throw CaptureError("macOS refused to create the extra display")
        }
        display = vd

        // Sound. With our driver installed, only what is played into "iMac Speakers"; otherwise everything.
        var screenAudio = false
        if audio["out"] as? Bool == true {
            if let dev = SpeakerDevice(), SpeakerDevice.microphonePermission == .authorized {
                dev.onAudio = { [weak self] in self?.pcm($0) }
                dev.onVolume = { [weak s] level, mute in try? s?.ctrl.sendJSON(.audioVolume, ["volume": Double(level), "mute": mute]) }
                try dev.start()
                speakers = dev
                soundSource = .speakersDevice
            } else {
                screenAudio = true
                soundSource = .everything
            }
        }

        let capture = DisplayCapture()
        self.capture = capture
        var first = true
        capture.onFrame = { [weak self] pixelBuffer, _ in
            guard let self else { return }
            let key = first || self.keyframeWanted.take()
            first = false
            encoder.encode(pixelBuffer, ptsUs: Wire.nowUs(), forceKeyframe: key)
        }
        if screenAudio { capture.onAudio = { [weak self] in self?.pcm($0) } }
        // The new display takes a moment to become capturable.
        var lastError: Error = CaptureError("the extra display did not appear")
        for _ in 0..<20 {
            do {
                // Twice the frame rate as the minimum interval: asking for exactly 1/60 s loses pictures.
                try await capture.start(displayID: vd.displayID, width: streamWidth, height: streamHeight, fps: c.fps * 2, audio: screenAudio)
                return
            } catch {
                lastError = error
                try await Task.sleep(nanoseconds: 250_000_000)
            }
        }
        throw lastError
    }

    /// Ends the session from our side (quit, settings changed).
    public func stop(reason: String = "quit") { finish(reason, sendBye: true) }

    private func received(_ h: FrameHeader, _ p: Data) {
        guard let type = MsgType(rawValue: h.type) else { return }
        switch type {
        case .inputKey, .inputPointerRel, .inputButton:
            injector?.handle(type: type, payload: p)
        case .storageAttached, .storageDetached:
            guard let j = (try? JSONSerialization.jsonObject(with: p)) as? [String: Any] else { return }
            if type == .storageAttached { mounter?.attached(j) } else { mounter?.detached(j) }
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

    /// 10 ms per AUDIO_OUT message. Always called on one queue per sound source.
    private func pcm(_ data: Data) {
        audioPending.append(data)
        while audioPending.count >= 1920 {
            try? session?.ctrl.send(.audioOut, payload: audioPending.prefix(1920))
            audioPending.removeFirst(1920)
        }
    }

    private func finish(_ reason: String, sendBye: Bool) {
        lock.lock()
        let firstTime = !finished
        finished = true
        lock.unlock()
        guard firstTime else { return }
        let capture = self.capture, display = self.display
        speakers?.stop()
        injector?.releaseAll()
        mounter?.unmountAll()
        _ = sendBye             // close() sends BYE only while the connection is still up
        session?.close(reason: reason)
        Task { @MainActor in
            await capture?.stop()
            withExtendedLifetime(display) {}   // let go of the extra display only after the capture has stopped
        }
        self.capture = nil
        self.display = nil
        self.encoder = nil
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
