// Screen capture of one display with ScreenCaptureKit, delivered as NV12 pixel buffers.
import CoreMedia
import ScreenCaptureKit

public struct CaptureError: Error, CustomStringConvertible {
    public let description: String
    init(_ s: String) { description = s }
}

public final class DisplayCapture: NSObject, SCStreamOutput, SCStreamDelegate {
    /// Called on the capture queue for every new picture. `displayTime` is on the host clock.
    public var onFrame: ((CVPixelBuffer, CMTime) -> Void)?
    public var onStop: ((Error) -> Void)?
    /// Everything this Mac is playing, as 48 kHz stereo signed 16-bit interleaved PCM (any number of frames per call).
    public var onAudio: ((Data) -> Void)?
    public private(set) var audioFrames = 0
    public private(set) var completeFrames = 0
    public private(set) var idleFrames = 0
    public private(set) var otherFrames = 0

    private var stream: SCStream?
    private let queue = DispatchQueue(label: "imd.capture", qos: .userInteractive)
    private let audioQueue = DispatchQueue(label: "imd.capture.audio", qos: .userInteractive)

    public static var hasPermission: Bool { CGPreflightScreenCaptureAccess() }
    /// Shows the system prompt the first time; afterwards it only reports the state.
    @discardableResult public static func requestPermission() -> Bool { CGRequestScreenCaptureAccess() }

    public func start(displayID: CGDirectDisplayID, width: Int, height: Int, fps: Int, showCursor: Bool = true, queueDepth: Int = 3,
                      audio: Bool = false) async throws {
        let content: SCShareableContent
        do {
            content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
        } catch {
            throw CaptureError("screen capture is not allowed (\(error.localizedDescription)). Allow it in System Settings > Privacy & Security > Screen & System Audio Recording.")
        }
        guard let display = content.displays.first(where: { $0.displayID == displayID }) else {
            throw CaptureError("display \(displayID) is not in the list of capturable displays")
        }
        let cfg = SCStreamConfiguration()
        cfg.width = width
        cfg.height = height
        cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        cfg.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange  // NV12: what the encoder takes directly
        cfg.colorMatrix = CGDisplayStream.yCbCrMatrix_ITU_R_709_2
        cfg.colorSpaceName = CGColorSpace.sRGB
        cfg.queueDepth = queueDepth
        cfg.showsCursor = showCursor
        if audio {
            cfg.capturesAudio = true
            cfg.sampleRate = 48000
            cfg.channelCount = 2
            cfg.excludesCurrentProcessAudio = true
        }
        let s = SCStream(filter: SCContentFilter(display: display, excludingWindows: []), configuration: cfg, delegate: self)
        try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        if audio { try s.addStreamOutput(self, type: .audio, sampleHandlerQueue: audioQueue) }
        try await s.startCapture()
        stream = s
    }

    public func stop() async {
        try? await stream?.stopCapture()
        stream = nil
    }

    public func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard sb.isValid else { return }
        if type == .audio { audio(sb); return }
        guard type == .screen else { return }
        guard let atts = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let raw = atts.first?[.status] as? Int, let status = SCFrameStatus(rawValue: raw) else { return }
        switch status {
        case .complete:
            guard let pb = sb.imageBuffer else { return }
            completeFrames += 1
            onFrame?(pb, sb.presentationTimeStamp)
        case .idle: idleFrames += 1     // nothing changed on screen: no new picture
        default: otherFrames += 1
        }
    }

    /// ScreenCaptureKit delivers 32-bit float, usually one buffer per channel. The wire wants s16le interleaved stereo.
    private func audio(_ sb: CMSampleBuffer) {
        guard let onAudio, let asbd = sb.formatDescription?.audioStreamBasicDescription,
              asbd.mFormatFlags & kAudioFormatFlagIsFloat != 0, asbd.mBitsPerChannel == 32 else { return }
        let frames = sb.numSamples
        guard frames > 0 else { return }
        var out = [Int16](repeating: 0, count: frames * 2)
        func s16(_ x: Float32) -> Int16 { Int16(max(-1, min(1, x)) * 32767) }
        try? sb.withAudioBufferList { list, _ in
            if asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0 {
                for ch in 0..<2 {
                    let buf = list[min(ch, list.count - 1)]   // mono source: same samples on both sides
                    guard let p = buf.mData?.assumingMemoryBound(to: Float32.self), Int(buf.mDataByteSize) >= frames * 4 else { return }
                    for i in 0..<frames { out[i * 2 + ch] = s16(p[i]) }
                }
            } else {
                let chans = Int(asbd.mChannelsPerFrame)
                guard chans > 0, let p = list[0].mData?.assumingMemoryBound(to: Float32.self),
                      Int(list[0].mDataByteSize) >= frames * chans * 4 else { return }
                for i in 0..<frames {
                    out[i * 2] = s16(p[i * chans])
                    out[i * 2 + 1] = s16(p[i * chans + (chans > 1 ? 1 : 0)])
                }
            }
        }
        audioFrames += frames
        onAudio(out.withUnsafeBufferPointer { Data(buffer: $0) })
    }

    public func stream(_ stream: SCStream, didStopWithError error: Error) { onStop?(error) }
}
