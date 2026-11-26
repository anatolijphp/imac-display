// `imd`: command-line test tool for the Mac side (M1 spikes). Not the app yet.
import AppKit
import CoreMedia
import IMDKit

struct Options {
    var values: [String: String] = [:]
    var flags: Set<String> = []
    init(_ args: [String]) {
        var i = 0
        while i < args.count {
            let a = args[i]
            if a.hasPrefix("--") {
                let k = String(a.dropFirst(2))
                if i + 1 < args.count, !args[i + 1].hasPrefix("--") { values[k] = args[i + 1]; i += 2 } else { flags.insert(k); i += 1 }
            } else { i += 1 }
        }
    }
    func int(_ k: String, _ d: Int) -> Int { values[k].flatMap { Int($0) } ?? d }
    func double(_ k: String, _ d: Double) -> Double { values[k].flatMap { Double($0) } ?? d }
    func size(_ k: String, _ d: (Int, Int)) -> (Int, Int) {
        guard let p = values[k]?.split(separator: "x").compactMap({ Int($0) }), p.count == 2 else { return d }
        return (p[0], p[1])
    }
}

struct Failure: Error, CustomStringConvertible { let description: String }

let usage = """
usage: imd <command> [options]

  vdisplay  [--points WxH] [--lodpi] [--seconds N]
      Create a virtual display, print what macOS reports about it, remove it.

  pipeline  [--points WxH] [--lodpi] [--capture WxH] [--cols N --rows N] [--mbps M] [--seconds N]
            [--no-low-latency] [--codec hevc] [--speed] [--serial-tiles] [--capture-fps N] [--queue-depth N]
            [--no-scene] [--out PREFIX] [--host NAME [--port P]]
            [--no-input] [--no-storage] [--no-audio] [--audio] [--audio-source auto|device|screen] [--volume 0..1] [--pointer-speed X] [--quiet]
      Virtual display → test content → screen capture → H.264 encode, with timings.
      --cols/--rows cut the picture into tiles, one encoder each (2x2 for native 5K).
      --out writes PREFIX.tileN.h264. --host also sends the stream to an iMac (PROTOCOL.md);
      the iMac's keyboard and mouse then control this Mac and its USB drives are mounted here.
      --seconds 0 runs until Ctrl-C.

  setmode   --points WxH [--lodpi]
      Change the resolution of the app's extra display, as Display settings would (for testing).

  hello     --host NAME [--port P] [--seconds N]
  find      [--seconds N]     look for the iMac the way the app does; prints where it answers
      Open a session with the iMac without video and print what it sends.

Testing only: --host connects to a named machine (for example imac-display.local over Wi-Fi)
instead of looking for the iMac on the Thunderbolt link.
"""

func describe(_ h: FrameHeader, _ payload: Data) -> String {
    let name = MsgType(rawValue: h.type).map { "\($0)" } ?? String(format: "type 0x%02x", h.type)
    if let s = String(data: payload, encoding: .utf8), s.first == "{" { return "\(name) \(s)" }
    return "\(name) \(payload.count) bytes \(payload.prefix(16).map { String(format: "%02x", $0) }.joined())"
}

@MainActor func vdisplay(_ o: Options) async throws {
    let (pw, ph) = o.size("points", (2560, 1440))
    var vd = VirtualDisplay(name: "iMac Display", pointsWide: pw, pointsHigh: ph, hiDPI: !o.flags.contains("lodpi"))
    guard let id = vd?.displayID else { throw Failure(description: "the system refused to create the virtual display") }
    print("created: displayID \(id), asked for \(pw)x\(ph) points, \(vd!.pixelsWide)x\(vd!.pixelsHigh) pixels")
    try await Task.sleep(nanoseconds: 1_500_000_000)
    print("online: \(CGDisplayIsOnline(id) != 0), active: \(CGDisplayIsActive(id) != 0), CGDisplayPixels: \(CGDisplayPixelsWide(id))x\(CGDisplayPixelsHigh(id))")
    if let m = CGDisplayCopyDisplayMode(id) {
        print("current mode: \(m.width)x\(m.height) points, \(m.pixelWidth)x\(m.pixelHeight) pixels, \(m.refreshRate) Hz")
    }
    let all = CGDisplayCopyAllDisplayModes(id, [kCGDisplayShowDuplicateLowResolutionModes: true] as CFDictionary) as? [CGDisplayMode] ?? []
    print("modes offered: " + all.map { "\($0.width)x\($0.height)@\($0.pixelWidth)x\($0.pixelHeight)" }.joined(separator: ", "))
    if let s = vd?.screen {
        print("NSScreen: \"\(s.localizedName)\", frame \(s.frame), scale \(s.backingScaleFactor), max \(s.maximumFramesPerSecond) fps")
    } else {
        print("NSScreen: not found")
    }
    try await Task.sleep(nanoseconds: UInt64(o.double("seconds", 3) * 1e9))
    vd = nil   // releasing the object is what removes the display
    for tenth in 0...50 {   // CGDisplayIsOnline answers nonzero for an ID that no longer exists, so look at the list
        var ids = [CGDirectDisplayID](repeating: 0, count: 32)
        var n: UInt32 = 0
        CGGetOnlineDisplayList(32, &ids, &n)
        if !ids.prefix(Int(n)).contains(id) { print(String(format: "removed %.1f s after release", Double(tenth) / 10)); return }
        try await Task.sleep(nanoseconds: 100_000_000)
    }
    print("still online 5 s after release")
}

/// Changes the resolution of the app's extra display, as picking it in Display settings would.
func setMode(_ o: Options) throws {
    let (w, h) = o.size("points", (1920, 1080))
    var ids = [CGDirectDisplayID](repeating: 0, count: 16)
    var n: UInt32 = 0
    CGGetOnlineDisplayList(16, &ids, &n)
    guard let id = ids.prefix(Int(n)).first(where: { CGDisplayVendorNumber($0) == 0x1D5C }) else {
        throw Failure(description: "the iMac Display extra display is not there")
    }
    let all = CGDisplayCopyAllDisplayModes(id, [kCGDisplayShowDuplicateLowResolutionModes: true] as CFDictionary) as? [CGDisplayMode] ?? []
    print("modes: " + all.map { "\($0.width)x\($0.height)@\($0.pixelWidth)x\($0.pixelHeight)" }.joined(separator: ", "))
    let wantPixels = o.flags.contains("lodpi") ? w : w * 2
    guard let mode = all.first(where: { $0.width == w && $0.height == h && $0.pixelWidth == wantPixels }) else {
        throw Failure(description: "no \(w)x\(h) mode with \(wantPixels) pixels across")
    }
    let rc = CGDisplaySetDisplayMode(id, mode, nil)
    guard rc == .success else { throw Failure(description: "could not change the mode (\(rc.rawValue))") }
    print("display \(id) set to \(w)x\(h) points, \(mode.pixelWidth)x\(mode.pixelHeight) pixels")
}

@MainActor func hello(_ o: Options) async throws {
    guard let host = o.values["host"] else { throw Failure(description: "hello needs --host") }
    let s = try Session(host: host, port: UInt16(o.int("port", Int(Wire.port))), name: Host.current().localizedName ?? "Mac")
    print("connected to \(s.ctrl.peer)")
    print("iMac HELLO: \(String(data: try JSONSerialization.data(withJSONObject: s.imacHello, options: [.sortedKeys]), encoding: .utf8) ?? "")")
    s.onMessage = { h, p in print("← " + describe(h, p)) }
    s.onEnd = { print("session ended: \($0)") }
    try s.configure(videoConfig(width: 3840, height: 2160, tiles: TileRect.grid(width: 3840, height: 2160, cols: 1, rows: 1)))
    print("CONFIG accepted")
    s.start()
    try await Task.sleep(nanoseconds: UInt64(o.double("seconds", 10) * 1e9))
    s.close()
}

@MainActor func find(_ o: Options) async throws {
    let finder = IMacFinder()
    finder.onChange = { found in
        print(found.map { "found: \($0.address) (\($0.thunderbolt ? "Thunderbolt" : "not Thunderbolt"))" } ?? "not found")
    }
    finder.start()
    try await Task.sleep(nanoseconds: UInt64(o.double("seconds", 6) * 1e9))
    finder.stop()
}

func videoConfig(width: Int, height: Int, tiles: [TileRect]) -> [String: Any] {
    ["video": ["codec": "h264", "fps": 60, "w": width, "h": height, "scale": "fit",
               "tiles": tiles.map { ["id": $0.id, "x": $0.x, "y": $0.y, "w": $0.w, "h": $0.h] }] as [String: Any],
     "audio_out": ["rate": 48000, "ch": 2, "fmt": "s16le"],
     "audio_in": ["enabled": false, "rate": 48000, "ch": 1, "fmt": "s16le"]]
}

@MainActor func pipeline(_ o: Options) async throws {
    let (pw, ph) = o.size("points", (2560, 1440))
    let hiDPI = !o.flags.contains("lodpi")
    let (cw, ch) = o.size("capture", (pw * (hiDPI ? 2 : 1), ph * (hiDPI ? 2 : 1)))
    let cols = o.int("cols", 1), rows = o.int("rows", 1)
    let seconds = o.double("seconds", 8)
    let totalBitrate = Int(o.double("mbps", 40) * 1e6)
    let fps = 60

    guard DisplayCapture.hasPermission else {
        DisplayCapture.requestPermission()
        throw Failure(description: "screen capture is not allowed for this program yet. Allow it in System Settings > Privacy & Security > Screen & System Audio Recording, then run again.")
    }
    guard let vd = VirtualDisplay(name: "iMac Display", pointsWide: pw, pointsHigh: ph, hiDPI: hiDPI) else {
        throw Failure(description: "the system refused to create the virtual display")
    }
    try await Task.sleep(nanoseconds: 1_500_000_000)
    guard let screen = vd.screen else { throw Failure(description: "the virtual display did not show up as a screen") }
    print("virtual display \(vd.displayID): \(pw)x\(ph) points, \(vd.pixelsWide)x\(vd.pixelsHigh) pixels, scale \(screen.backingScaleFactor)")
    let scene = o.flags.contains("no-scene") ? nil : TestScene(screen: screen)

    let tiles = TileRect.grid(width: cw, height: ch, cols: cols, rows: rows)
    let splitter = tiles.count > 1 ? TileSplitter(tiles: tiles) : nil
    var encoders: [H264Encoder] = []
    for t in tiles {
        var c = H264Encoder.Config(width: t.w, height: t.h, fps: fps, bitrate: totalBitrate / tiles.count)
        c.lowLatency = !o.flags.contains("no-low-latency")
        c.hevc = o.values["codec"] == "hevc"
        c.preferSpeed = o.flags.contains("speed")
        c.realTime = !o.flags.contains("no-realtime")
        if let kb = o.values["burst-kb"].flatMap({ Double($0) }) { c.burstBytes = Int(kb * 1000) / tiles.count }
        c.burstWindow = o.double("burst-window", 0.05)
        c.expectedFps = o.values["expected-fps"].flatMap { Int($0) }
        if let m = o.values["power-efficiency"] { c.maximizePowerEfficiency = m == "on" }
        encoders.append(try H264Encoder(c))
    }
    print("capture \(cw)x\(ch) at \(fps) fps, \(tiles.count) tile(s) of \(tiles[0].w)x\(tiles[0].h), \(totalBitrate / 1_000_000) Mbit/s total, encoder \(encoders[0].encoderName)")
    for n in Set(encoders.flatMap { $0.notes }).sorted() { print("  encoder note: \(n)") }

    var session: Session?
    let keyframeWanted = Flag()
    let sentSinceStats = Counter()
    let quiet = o.flags.contains("quiet")
    var injector: InputInjector?
    var mounter: StorageMounter?
    let ended = Flag()
    if let host = o.values["host"] {
        let s = try Session(host: host, port: UInt16(o.int("port", Int(Wire.port))), name: Host.current().localizedName ?? "Mac")
        print("iMac at \(s.ctrl.peer): \(String(data: try JSONSerialization.data(withJSONObject: s.imacHello, options: [.sortedKeys]), encoding: .utf8) ?? "")")
        if !o.flags.contains("no-input") {
            if !InputInjector.hasPermission {
                InputInjector.requestPermission()
                print("WARNING: no Accessibility permission yet, so the iMac's keyboard and mouse will do nothing. Allow this program (or the terminal it runs in) in System Settings > Privacy & Security > Accessibility, then start again.")
            }
            injector = InputInjector()
            injector?.pointerSpeed = o.double("pointer-speed", 1.0)
        }
        if !o.flags.contains("no-storage") {
            mounter = StorageMounter(host: s.host, sendEject: { [weak s] id in try? s?.ctrl.sendJSON(.storageEject, ["id": id]) },
                                     log: { print("storage: \($0)") })
        }
        s.onMessage = { h, p in
            let type = MsgType(rawValue: h.type)
            if type == .inputKey || type == .inputPointerRel || type == .inputButton, let injector {
                injector.handle(type: type!, payload: p)
                return
            }
            if type == .storageAttached || type == .storageDetached, let mounter,
               let j = (try? JSONSerialization.jsonObject(with: p)) as? [String: Any] {
                if type == .storageAttached { mounter.attached(j) } else { mounter.detached(j) }
                return
            }
            if h.type == MsgType.videoRequestKeyframe.rawValue { keyframeWanted.set() }
            if h.type == MsgType.videoStats.rawValue, let j = (try? JSONSerialization.jsonObject(with: p)) as? [String: Any] {
                // Once a second: what the iMac did with what we sent.
                let sent = sentSinceStats.take()
                let decoded = (j["decoded"] as? Int) ?? 0
                var line = "iMac: decoded \(decoded), dropped \(j["dropped"] ?? "?"), decode \(j["decode_ms"] ?? "?") ms, to screen \(j["present_ms"] ?? "?") ms | sent \(sent)"
                // Safety net: we are sending and the iMac shows nothing, so its decoder has lost its place.
                if decoded == 0 && sent > 0 { keyframeWanted.set(); line += "  → sending a keyframe unasked" }
                if !quiet || (decoded == 0 && sent > 0) { print(line) }
                return
            }
            print("← " + describe(h, p))
        }
        s.onEnd = { print("session ended: \($0)"); ended.set() }
        try s.configure(videoConfig(width: cw, height: ch, tiles: tiles))
        try s.openVideo()
        s.start()
        session = s
    }

    var files: [FileHandle] = []
    if let prefix = o.values["out"] {
        for t in tiles {
            let path = "\(prefix).tile\(t.id).h264"
            FileManager.default.createFile(atPath: path, contents: nil)
            guard let f = FileHandle(forWritingAtPath: path) else { throw Failure(description: "cannot write \(path)") }
            files.append(f)
        }
    }

    let encodeMs = Samples(), keyBytes = Samples(), deltaBytes = Samples(), intervalMs = Samples()
    let lagMs = Samples(), splitMs = Samples(), totalMs = Samples(), sendMs = Samples()
    let collector = TileCollector(count: tiles.count)
    let ioQueue = DispatchQueue(label: "imd.sink")
    for (i, e) in encoders.enumerated() {
        e.onOutput = { out in
            encodeMs.add(out.encodeMs)
            (out.keyframe ? keyBytes : deltaBytes).add(Double(out.annexB.count))
            guard let picture = collector.add(tile: i, out) else { return }
            // All tiles of this picture are encoded: capture-to-encoded time, then hand over in tile order.
            totalMs.add(Double(Wire.nowUs() - out.ptsUs) / 1000)
            ioQueue.async {
                let t0 = Wire.nowUs()
                for (n, p) in picture.enumerated() {
                    if !files.isEmpty { files[n].write(p.annexB) }
                    try? session?.sendVideo(tile: n, keyframe: p.keyframe, lastTile: n == picture.count - 1, ptsUs: p.ptsUs, data: p.annexB)
                }
                if session != nil { sendMs.add(Double(Wire.nowUs() - t0) / 1000); sentSinceStats.add() }
            }
        }
    }

    let capture = DisplayCapture()
    let serialTiles = o.flags.contains("serial-tiles")
    let callMs = Samples()
    var lastUs: UInt64 = 0
    var first = true
    capture.onFrame = { pb, displayTime in
        let now = Wire.nowUs()
        if lastUs != 0 { intervalMs.add(Double(now - lastUs) / 1000) }
        lastUs = now
        lagMs.add(CMTimeGetSeconds(CMTimeSubtract(CMClockGetTime(CMClockGetHostTimeClock()), displayTime)) * 1000)
        let key = first || keyframeWanted.take()
        first = false
        defer { callMs.add(Double(Wire.nowUs() - now) / 1000) }
        if let splitter {
            guard let parts = splitter.split(pb) else { return }
            splitMs.add(Double(Wire.nowUs() - now) / 1000)
            if serialTiles {
                for (i, p) in parts.enumerated() { encoders[i].encode(p, ptsUs: now, forceKeyframe: key) }
            } else {   // the low-latency encoder blocks the caller while it works: run the tiles side by side
                DispatchQueue.concurrentPerform(iterations: parts.count) { i in encoders[i].encode(parts[i], ptsUs: now, forceKeyframe: key) }
            }
        } else {
            encoders[0].encode(pb, ptsUs: now, forceKeyframe: key)
        }
    }
    // Sound goes to the iMac as AUDIO_OUT, 10 ms (480 frames, 1920 bytes) per message. Two sources:
    //   device: the "iMac Speakers" output device of our audio driver. The user picks it like any output.
    //   screen: everything this Mac plays, copied by ScreenCaptureKit (no driver, no choice).
    let imacPlaysSound = ((session?.imacHello["audio"] as? [String: Any])?["out"] as? Bool) == true
    let wantAudio = session != nil && !o.flags.contains("no-audio") && (imacPlaysSound || o.flags.contains("audio"))
    var audioSource = o.values["audio-source"] ?? "auto"
    if audioSource == "auto" { audioSource = SpeakerDevice.isInstalled ? "device" : "screen" }
    var speakers: SpeakerDevice?
    var audioPending = Data()
    let audioMessages = Counter()
    let onPCM: (Data) -> Void = { pcm in   // always called on the same queue
        audioPending.append(pcm)
        while audioPending.count >= 1920 {
            try? session?.ctrl.send(.audioOut, payload: audioPending.prefix(1920))
            audioPending.removeFirst(1920)
            audioMessages.add()
        }
    }
    if wantAudio && audioSource == "device" {
        guard let dev = SpeakerDevice() else { throw Failure(description: "the iMac Speakers audio driver is not installed (mac/scripts/install-audio-driver.sh)") }
        if SpeakerDevice.microphonePermission != .authorized {
            let granted = await SpeakerDevice.requestMicrophonePermission()
            if !granted { print("WARNING: no Microphone permission, so the iMac will stay silent. macOS asks for it because the app records from the iMac Speakers device. Allow it in System Settings > Privacy & Security > Microphone, then start again.") }
        }
        dev.onAudio = onPCM
        dev.onVolume = { level, mute in
            try? session?.ctrl.sendJSON(.audioVolume, ["volume": Double(level), "mute": mute])
            print(String(format: "sound: iMac Speakers volume %.0f %%%@", level * 100, mute ? ", muted" : ""))
        }
        dev.selectWhileRunning = !o.flags.contains("no-select-speakers")
        dev.log = { print("sound: \($0)") }
        try dev.start()
        speakers = dev
        print("sound: sending what is played into \"iMac Speakers\"")
    } else if wantAudio {
        capture.onAudio = onPCM
        print("sound: sending everything this Mac plays (the iMac Speakers driver is not installed)")
    } else if session != nil && !imacPlaysSound {
        print("sound: the iMac says it cannot play sound yet (HELLO audio.out is false), so none is sent")
    }
    let sendAudio = wantAudio
    let screenAudio = wantAudio && audioSource != "device"
    if screenAudio, let v = o.values["volume"].flatMap({ Double($0) }) {   // the iMac's speaker level, 0 to 1
        try? session?.ctrl.sendJSON(.audioVolume, ["volume": max(0, min(1, v)), "mute": false])
    }
    try await capture.start(displayID: vd.displayID, width: cw, height: ch, fps: o.int("capture-fps", fps), queueDepth: o.int("queue-depth", 3),
                            audio: screenAudio)
    // Runs for --seconds (0 = until Ctrl-C, or until the session ends).
    signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN)
    let sources = [SIGINT, SIGTERM].map { sig -> DispatchSourceSignal in
        let src = DispatchSource.makeSignalSource(signal: sig, queue: .main)
        src.setEventHandler { ended.set() }
        src.resume()
        return src
    }
    let started = Date()
    while !ended.peek() && (seconds <= 0 || Date().timeIntervalSince(started) < seconds) {
        try await Task.sleep(nanoseconds: 100_000_000)
    }
    sources.forEach { $0.cancel() }
    let ran = Date().timeIntervalSince(started)
    await capture.stop()
    speakers?.stop()
    encoders.forEach { $0.flush() }
    ioQueue.sync {}
    injector?.releaseAll()
    mounter?.unmountAll()
    if sendAudio { print(String(format: "sound sent to the iMac: %d messages of 10 ms = %.1f s (captured %.1f s)", audioMessages.take(), Double(capture.audioFrames + (speakers?.frames ?? 0)) / 48000 - Double(audioPending.count) / 4 / 48000, Double(capture.audioFrames + (speakers?.frames ?? 0)) / 48000)) }
    if let injector { print("input from the iMac: \(injector.keys) key events, \(injector.moves) pointer moves, \(injector.clicks) button events") }
    session?.close()
    files.forEach { try? $0.close() }
    scene?.close()

    let bytes = keyBytes.sum + deltaBytes.sum
    print(String(format: "pictures: %d new in %.1f s = %.1f per second (%d idle, %d other)", capture.completeFrames, ran,
                 Double(capture.completeFrames) / ran, capture.idleFrames, capture.otherFrames))
    print("time between pictures, ms (p50 / p95 / max): \(intervalMs.summary)")
    print("capture delay, screen refresh to our callback, ms: \(lagMs.summary)")
    if splitter != nil { print("cutting into tiles, ms: \(splitMs.summary)") }
    print("encode time per tile, ms: \(encodeMs.summary)")
    print("time our capture callback is busy per picture, ms: \(callMs.summary)")
    print("callback to all tiles encoded, ms: \(totalMs.summary)")
    if session != nil { print("writing one picture to the socket, ms: \(sendMs.summary)") }
    print(String(format: "bitrate: %.1f Mbit/s; keyframes %d (largest %.0f kB), other frames %d (median %.1f kB, largest %.0f kB); encoder failures %d",
                 bytes * 8 / ran / 1e6, keyBytes.count, keyBytes.percentile(100) / 1000, deltaBytes.count,
                 deltaBytes.percentile(50) / 1000, deltaBytes.percentile(100) / 1000, encoders.reduce(0) { $0 + $1.failures }))
}

/// Thread-safe counter that is read and reset together.
final class Counter {
    private var value = 0
    private let lock = NSLock()
    func add() { lock.lock(); value += 1; lock.unlock() }
    func take() -> Int { lock.lock(); defer { value = 0; lock.unlock() }; return value }
}

/// Thread-safe one-shot flag.
final class Flag {
    private var value = false
    private let lock = NSLock()
    func set() { lock.lock(); value = true; lock.unlock() }
    func take() -> Bool { lock.lock(); defer { value = false; lock.unlock() }; return value }
    func peek() -> Bool { lock.lock(); defer { lock.unlock() }; return value }
}

/// Gathers the encoded tiles of one picture (same pts) and returns them in tile order once all are there.
final class TileCollector {
    private let count: Int
    private var pending: [UInt64: [H264Encoder.Output?]] = [:]
    private let lock = NSLock()
    init(count: Int) { self.count = count }
    func add(tile: Int, _ out: H264Encoder.Output) -> [H264Encoder.Output]? {
        lock.lock(); defer { lock.unlock() }
        var slots = pending[out.ptsUs] ?? [H264Encoder.Output?](repeating: nil, count: count)
        slots[tile] = out
        let done = slots.compactMap { $0 }
        if done.count == count { pending[out.ptsUs] = nil; return done }
        pending[out.ptsUs] = slots
        return nil
    }
}

SpeakerDevice.runAsHelperIfAsked()   // a second copy of this program, started only to switch the sound output
setvbuf(stdout, nil, _IOLBF, 0)
let arguments = Array(CommandLine.arguments.dropFirst())
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
Task { @MainActor in
    do {
        let o = Options(Array(arguments.dropFirst()))
        switch arguments.first {
        case "vdisplay": try await vdisplay(o)
        case "pipeline": try await pipeline(o)
        case "hello": try await hello(o)
        case "find": try await find(o)
        case "setmode": try setMode(o)
        default: print(usage); exit(arguments.isEmpty ? 0 : 2)
        }
        exit(0)
    } catch {
        FileHandle.standardError.write("error: \(error)\n".data(using: .utf8)!)
        exit(1)
    }
}
app.run()
