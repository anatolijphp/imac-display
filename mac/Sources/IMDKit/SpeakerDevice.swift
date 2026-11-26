// The app's end of the "iMac Speakers" audio driver (mac/AudioDriver).
//
// While this object is started it records from the driver's hidden tap device. That is what makes
// the driver show "iMac Speakers" in the Mac's sound outputs, so the device appears when an iMac is
// connected and disappears when it is gone, like the speakers of a real monitor. On start the Mac's
// sound output is switched to it (unless told not to) and on stop it is switched back.
// Volume and mute of "iMac Speakers" are followed so they can be applied at the iMac.
// Recording from any audio input, even this virtual one, needs the Microphone permission.
import AVFoundation
import CoreAudio

public final class SpeakerDevice {
    public static let speakersUID = "iMacDisplay:speakers"
    public static let tapUID = "iMacDisplay:speakers-tap"

    /// 48 kHz stereo signed 16-bit interleaved PCM, any number of frames per call. Called on one queue.
    public var onAudio: ((Data) -> Void)?
    /// Volume 0…1 and mute of "iMac Speakers", at start and whenever the user changes them.
    public var onVolume: ((Float, Bool) -> Void)?
    /// Make "iMac Speakers" the Mac's sound output while connected, and put the old output back afterwards.
    public var selectWhileRunning = true
    public var log: (String) -> Void = { _ in }
    public private(set) var frames = 0

    private let tap: AudioDeviceID
    private var speakers: AudioDeviceID?
    private var procID: AudioDeviceIOProcID?
    private let queue = DispatchQueue(label: "imd.speakers", qos: .userInteractive)
    private let control = DispatchQueue(label: "imd.speakers.control")
    private var listener: AudioObjectPropertyListenerBlock?
    private var previousOutputUID: String?
    private var running = false
    private let stopLock = NSLock()
    private var stopRequested = false
    private var stopping: Bool { stopLock.lock(); defer { stopLock.unlock() }; return stopRequested }

    public static func device(uid: String) -> AudioDeviceID? {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyTranslateUIDToDevice,
                                              mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var cf = uid as CFString
        var id = AudioDeviceID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let rc = withUnsafePointer(to: &cf) {
            AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, UInt32(MemoryLayout<CFString>.size), $0, &size, &id)
        }
        return rc == noErr && id != kAudioObjectUnknown ? id : nil
    }

    /// True when the driver is installed and loaded (its tap device is always there).
    public static var isInstalled: Bool { device(uid: tapUID) != nil }

    public static var microphonePermission: AVAuthorizationStatus { AVCaptureDevice.authorizationStatus(for: .audio) }
    public static func requestMicrophonePermission() async -> Bool { await AVCaptureDevice.requestAccess(for: .audio) }

    private static let defaultOutputAddress = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyDefaultOutputDevice, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)

    private static var defaultOutput: AudioDeviceID? {
        get {
            var addr = defaultOutputAddress
            var id = AudioDeviceID(kAudioObjectUnknown)
            var size = UInt32(MemoryLayout<AudioDeviceID>.size)
            return AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &id) == noErr
                && id != kAudioObjectUnknown ? id : nil
        }
    }

    @discardableResult private static func setDefaultOutput(_ id: AudioDeviceID) -> Bool {
        var addr = defaultOutputAddress
        var value = id
        return AudioObjectSetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, UInt32(MemoryLayout<AudioDeviceID>.size), &value) == noErr
    }

    private static func uid(of device: AudioDeviceID) -> String? {
        var a = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyDeviceUID, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var s: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(device, &a, 0, nil, &size, &s) == noErr else { return nil }
        return s?.takeRetainedValue() as String?
    }

    // Switching the Mac's sound output to "iMac Speakers" has to be done by another process.
    // Measured on macOS 27.2: a process that is recording from the tap can ask for the switch, gets
    // "no error" and nothing happens; a process that is not recording gets the switch. A process also
    // keeps seeing the old default for a while. So the app runs a second copy of itself for a moment,
    // once to switch and once to look.

    /// Call first thing in `main`: does the job and exits when this process was started as that helper.
    public static func runAsHelperIfAsked() {
        let args = CommandLine.arguments
        if let i = args.firstIndex(of: "--set-default-output"), i + 1 < args.count {
            guard let dev = device(uid: args[i + 1]) else { exit(2) }
            let ok = setDefaultOutput(dev)
            Thread.sleep(forTimeInterval: 0.25)   // the request is carried out a moment later; do not vanish before that
            exit(ok ? 0 : 1)
        }
        if args.contains("--print-default-output") {
            print(defaultOutput.flatMap { uid(of: $0) } ?? "")
            exit(0)
        }
    }

    @discardableResult private static func helper(_ arguments: [String]) -> String {
        guard let exe = Bundle.main.executableURL else { return "" }
        let p = Process()
        p.executableURL = exe
        p.arguments = arguments
        let out = Pipe()
        p.standardOutput = out
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return "" }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return (String(data: data, encoding: .utf8) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
    }

    public init?() {
        guard let t = Self.device(uid: Self.tapUID) else { return nil }
        tap = t
    }

    deinit { stop() }

    public func start() throws {
        // Short cycles keep the delay low: 10 ms, the size of one AUDIO_OUT message.
        var bufferFrames: UInt32 = 480
        var bufAddr = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyBufferFrameSize,
                                                 mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        AudioObjectSetPropertyData(tap, &bufAddr, 0, nil, UInt32(MemoryLayout<UInt32>.size), &bufferFrames)

        var id: AudioDeviceIOProcID?
        var rc = AudioDeviceCreateIOProcIDWithBlock(&id, tap, queue) { [weak self] _, input, _, _, _ in self?.received(input) }
        guard rc == noErr, let id else { throw CaptureError("cannot record from the iMac Speakers tap (\(rc))") }
        rc = AudioDeviceStart(tap, id)
        guard rc == noErr else {
            AudioDeviceDestroyIOProcID(tap, id)
            throw CaptureError("cannot start the iMac Speakers tap (\(rc))")
        }
        stopLock.lock(); stopRequested = false; stopLock.unlock()
        control.sync { procID = id; running = true }
        // Recording from the tap makes the driver publish "iMac Speakers". Wait for macOS to list it.
        control.async { [weak self] in self?.attachSpeakers() }
    }

    private func attachSpeakers() {
        var found: AudioDeviceID?
        for _ in 0..<60 {
            guard !stopping else { return }
            found = Self.device(uid: Self.speakersUID)
            if found != nil { break }
            Thread.sleep(forTimeInterval: 0.05)
        }
        guard let dev = found, !stopping else { log("\"iMac Speakers\" did not appear in the sound outputs"); return }
        speakers = dev
        let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in self?.control.async { self?.reportVolume() } }
        listener = block
        for selector in [kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyMute] {
            var a = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
            AudioObjectAddPropertyListenerBlock(dev, &a, control, block)
        }
        reportVolume()
        guard selectWhileRunning, let old = Self.defaultOutput, old != dev, let oldUID = Self.uid(of: old) else { return }
        // macOS needs a moment after a device appears before it can become the output. Switch, look, repeat.
        for attempt in 1...6 {
            for _ in 0..<(attempt == 1 ? 12 : 8) { if stopping { return }; Thread.sleep(forTimeInterval: 0.05) }
            Self.helper(["--set-default-output", Self.speakersUID])
            for _ in 0..<6 { if stopping { return }; Thread.sleep(forTimeInterval: 0.05) }
            let now = Self.helper(["--print-default-output"])
            if ProcessInfo.processInfo.environment["IMD_DEBUG"] != nil { log(String(format: "DEBUG %.2f attempt %d → default is %@", Date().timeIntervalSince1970.truncatingRemainder(dividingBy: 100), attempt, now)) }
            if now == Self.speakersUID {
                previousOutputUID = oldUID
                log("sound output switched to \"iMac Speakers\"" + (attempt > 1 ? " (attempt \(attempt))" : ""))
                return
            }
        }
        log("could not switch the sound output to \"iMac Speakers\"")
    }

    public func stop() {
        stopLock.lock(); stopRequested = true; stopLock.unlock()
        control.sync {
            guard running else { return }
            running = false
            // Put the Mac's sound back where it was, unless the user has picked something else meanwhile.
            let restore = previousOutputUID.flatMap { Self.helper(["--print-default-output"]) == Self.speakersUID ? $0 : nil }
            previousOutputUID = nil
            if let block = listener, let dev = speakers {
                for selector in [kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyMute] {
                    var a = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
                    AudioObjectRemovePropertyListenerBlock(dev, &a, control, block)
                }
            }
            listener = nil
            speakers = nil
            if let id = procID {   // stopping the recording makes the driver take "iMac Speakers" away
                AudioDeviceStop(tap, id)
                AudioDeviceDestroyIOProcID(tap, id)
                procID = nil
            }
            if let restore {       // macOS picks some output by itself now; make sure it is the one from before
                Thread.sleep(forTimeInterval: 0.3)
                if Self.helper(["--print-default-output"]) != restore { Self.helper(["--set-default-output", restore]) }
                log("sound output switched back")
            }
        }
    }

    private func reportVolume() {
        guard let dev = speakers else { return }
        var level: Float32 = 0
        var mute: UInt32 = 0
        var size = UInt32(MemoryLayout<Float32>.size)
        var a = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyVolumeScalar, mScope: kAudioObjectPropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
        guard AudioObjectGetPropertyData(dev, &a, 0, nil, &size, &level) == noErr else { return }
        a.mSelector = kAudioDevicePropertyMute
        size = UInt32(MemoryLayout<UInt32>.size)
        AudioObjectGetPropertyData(dev, &a, 0, nil, &size, &mute)
        onVolume?(level, mute != 0)
    }

    /// The tap delivers interleaved stereo 32-bit float. The wire wants s16le.
    private func received(_ list: UnsafePointer<AudioBufferList>) {
        let buffers = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: list))
        guard let onAudio, let b = buffers.first, let p = b.mData?.assumingMemoryBound(to: Float32.self) else { return }
        let samples = Int(b.mDataByteSize) / MemoryLayout<Float32>.size
        guard samples > 0 else { return }
        var out = [Int16](repeating: 0, count: samples)
        for i in 0..<samples { out[i] = Int16(max(-1, min(1, p[i])) * 32767) }
        frames += samples / 2
        onAudio(out.withUnsafeBufferPointer { Data(buffer: $0) })
    }
}
