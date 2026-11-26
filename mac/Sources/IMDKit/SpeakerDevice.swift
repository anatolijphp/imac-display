// The app's end of the "iMac Speakers" audio driver (mac/AudioDriver): records from the driver's
// hidden tap device whatever was played into "iMac Speakers", and follows that device's volume and
// mute so they can be applied at the iMac.
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
    public private(set) var frames = 0

    private let speakers: AudioDeviceID
    private let tap: AudioDeviceID
    private var procID: AudioDeviceIOProcID?
    private let queue = DispatchQueue(label: "imd.speakers", qos: .userInteractive)
    private var listener: AudioObjectPropertyListenerBlock?

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

    /// True when the driver is installed and loaded.
    public static var isInstalled: Bool { device(uid: speakersUID) != nil && device(uid: tapUID) != nil }

    public static var microphonePermission: AVAuthorizationStatus { AVCaptureDevice.authorizationStatus(for: .audio) }
    public static func requestMicrophonePermission() async -> Bool { await AVCaptureDevice.requestAccess(for: .audio) }

    /// True when "iMac Speakers" is the Mac's current sound output.
    public var isDefaultOutput: Bool {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDefaultOutputDevice,
                                              mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var id = AudioDeviceID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        return AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &id) == noErr && id == speakers
    }

    public init?() {
        guard let s = Self.device(uid: Self.speakersUID), let t = Self.device(uid: Self.tapUID) else { return nil }
        speakers = s; tap = t
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
        procID = id
        rc = AudioDeviceStart(tap, id)
        guard rc == noErr else { throw CaptureError("cannot start the iMac Speakers tap (\(rc))") }

        let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in self?.reportVolume() }
        listener = block
        for selector in [kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyMute] {
            var a = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
            AudioObjectAddPropertyListenerBlock(speakers, &a, queue, block)
        }
        queue.async { [weak self] in self?.reportVolume() }
    }

    public func stop() {
        if let id = procID {
            AudioDeviceStop(tap, id)
            AudioDeviceDestroyIOProcID(tap, id)
            procID = nil
        }
        if let block = listener {
            for selector in [kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyMute] {
                var a = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
                AudioObjectRemovePropertyListenerBlock(speakers, &a, queue, block)
            }
            listener = nil
        }
    }

    public var volume: (level: Float, mute: Bool) {
        var level: Float32 = 0
        var mute: UInt32 = 0
        var size = UInt32(MemoryLayout<Float32>.size)
        var a = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyVolumeScalar, mScope: kAudioObjectPropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
        AudioObjectGetPropertyData(speakers, &a, 0, nil, &size, &level)
        a.mSelector = kAudioDevicePropertyMute
        size = UInt32(MemoryLayout<UInt32>.size)
        AudioObjectGetPropertyData(speakers, &a, 0, nil, &size, &mute)
        return (level, mute != 0)
    }

    private func reportVolume() {
        let v = volume
        onVolume?(v.level, v.mute)
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
