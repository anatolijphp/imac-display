// Hardware H.264 encoder (VideoToolbox) set up for interactive use: no frame reordering,
// one picture in → one access unit out, output as Annex B as PROTOCOL.md asks.
import CoreMedia
import VideoToolbox

public struct EncoderError: Error, CustomStringConvertible {
    public let description: String
    init(_ s: String) { description = s }
}

public final class H264Encoder {
    public struct Config {
        public var width: Int, height: Int, fps: Int
        public var bitrate: Int            // bits per second, average
        public var lowLatency = true       // VideoToolbox's low-latency rate control
        public var highProfile = true
        public var hevc = false            // H.265 instead of H.264 (the iMac would have to decode it on the CPU)
        public var preferSpeed = false     // PrioritizeEncodingSpeedOverQuality
        public var realTime = true
        public var expectedFps: Int? = nil // what the encoder is told to expect; nil = fps
        public var maximizePowerEfficiency: Bool? = nil
        /// Hard cap on the bytes of any `burstWindow` seconds (DataRateLimits). Keeps keyframes small:
        /// a keyframe that takes several frame times to send and decode makes the receiver fall behind.
        public var burstBytes: Int? = nil
        public var burstWindow = 0.05
        public init(width: Int, height: Int, fps: Int, bitrate: Int) {
            self.width = width; self.height = height; self.fps = fps; self.bitrate = bitrate
        }
    }

    public struct Output {
        public let annexB: Data
        public let keyframe: Bool
        public let ptsUs: UInt64
        public let encodeMs: Double   // from handing the picture to the encoder to getting it back
    }

    /// Called on a VideoToolbox thread.
    public var onOutput: ((Output) -> Void)?
    /// Settings the encoder refused, for the log.
    public private(set) var notes: [String] = []
    public private(set) var encoderName = "?"
    public private(set) var failures = 0

    private let session: VTCompressionSession
    private let hevc: Bool

    public init(_ c: Config) throws {
        var spec: [CFString: Any] = [kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: true]
        if c.lowLatency { spec[kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = true }
        var s: VTCompressionSession?
        let st = VTCompressionSessionCreate(
            allocator: nil, width: Int32(c.width), height: Int32(c.height), codecType: c.hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264,
            encoderSpecification: spec as CFDictionary, imageBufferAttributes: nil, compressedDataAllocator: nil,
            outputCallback: nil, refcon: nil, compressionSessionOut: &s)
        guard st == noErr, let s else { throw EncoderError("cannot create a \(c.width)x\(c.height) \(c.hevc ? "HEVC" : "H.264") encoder (status \(st))") }
        session = s
        hevc = c.hevc

        func set(_ key: CFString, _ value: Any, _ name: String) {
            let r = VTSessionSetProperty(s, key: key, value: value as CFTypeRef)
            if r != noErr { notes.append("\(name) refused (\(r))") }
        }
        set(kVTCompressionPropertyKey_RealTime, c.realTime, "RealTime")
        if let m = c.maximizePowerEfficiency { set(kVTCompressionPropertyKey_MaximizePowerEfficiency, m, "MaximizePowerEfficiency") }
        set(kVTCompressionPropertyKey_AllowFrameReordering, false, "AllowFrameReordering=false")
        if c.hevc {
            set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main_AutoLevel, "ProfileLevel")
        } else {
            set(kVTCompressionPropertyKey_ProfileLevel,
                c.highProfile ? kVTProfileLevel_H264_High_AutoLevel : kVTProfileLevel_H264_Main_AutoLevel, "ProfileLevel")
            if c.highProfile { set(kVTCompressionPropertyKey_H264EntropyMode, kVTH264EntropyMode_CABAC, "CABAC") }
        }
        if c.preferSpeed { set(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, true, "PrioritizeEncodingSpeedOverQuality") }
        set(kVTCompressionPropertyKey_AverageBitRate, c.bitrate, "AverageBitRate")
        if let b = c.burstBytes {
            set(kVTCompressionPropertyKey_DataRateLimits, [NSNumber(value: b), NSNumber(value: c.burstWindow)] as CFArray, "DataRateLimits")
        }
        set(kVTCompressionPropertyKey_ExpectedFrameRate, c.expectedFps ?? c.fps, "ExpectedFrameRate")
        // Keyframes only when asked for (first picture, CONFIG change, VIDEO_REQUEST_KEYFRAME).
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, 36000, "MaxKeyFrameInterval")
        set(kVTCompressionPropertyKey_MaxFrameDelayCount, 0, "MaxFrameDelayCount=0")
        set(kVTCompressionPropertyKey_ColorPrimaries, kCMFormatDescriptionColorPrimaries_ITU_R_709_2, "ColorPrimaries")
        set(kVTCompressionPropertyKey_TransferFunction, kCMFormatDescriptionTransferFunction_ITU_R_709_2, "TransferFunction")
        set(kVTCompressionPropertyKey_YCbCrMatrix, kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2, "YCbCrMatrix")
        VTCompressionSessionPrepareToEncodeFrames(s)

        var name: CFTypeRef?
        if VTSessionCopyProperty(s, key: kVTCompressionPropertyKey_EncoderID, allocator: nil, valueOut: &name) == noErr,
           let n = name as? String { encoderName = n }
    }

    deinit { VTCompressionSessionInvalidate(session) }

    public func encode(_ pixelBuffer: CVPixelBuffer, ptsUs: UInt64, forceKeyframe: Bool = false) {
        let t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC)
        let props: CFDictionary? = forceKeyframe ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil
        let st = VTCompressionSessionEncodeFrame(
            session, imageBuffer: pixelBuffer,
            presentationTimeStamp: CMTime(value: CMTimeValue(ptsUs), timescale: 1_000_000),
            duration: .invalid, frameProperties: props, infoFlagsOut: nil
        ) { [weak self] status, _, sb in
            guard let self else { return }
            guard status == noErr, let sb, CMSampleBufferDataIsReady(sb) else { self.failures += 1; return }
            let ms = Double(clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0) / 1e6
            var key = true
            if let atts = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[CFString: Any]],
               let notSync = atts.first?[kCMSampleAttachmentKey_NotSync] as? Bool { key = !notSync }
            guard let data = H264Encoder.annexB(sb, withParameterSets: key, hevc: self.hevc) else { self.failures += 1; return }
            self.onOutput?(Output(annexB: data, keyframe: key, ptsUs: ptsUs, encodeMs: ms))
        }
        if st != noErr { failures += 1 }
    }

    /// Waits until every picture handed in so far has come out.
    public func flush() { VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid) }

    /// VideoToolbox gives length-prefixed NAL units and keeps SPS/PPS aside; the wire wants start codes
    /// with SPS and PPS in front of every keyframe.
    static func annexB(_ sb: CMSampleBuffer, withParameterSets: Bool, hevc: Bool) -> Data? {
        func parameterSet(_ fd: CMFormatDescription, _ i: Int, _ p: UnsafeMutablePointer<UnsafePointer<UInt8>?>?,
                          _ size: UnsafeMutablePointer<Int>?, _ count: UnsafeMutablePointer<Int>?) -> OSStatus {
            hevc ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, parameterSetIndex: i, parameterSetPointerOut: p,
                                                                      parameterSetSizeOut: size, parameterSetCountOut: count, nalUnitHeaderLengthOut: nil)
                 : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, parameterSetIndex: i, parameterSetPointerOut: p,
                                                                      parameterSetSizeOut: size, parameterSetCountOut: count, nalUnitHeaderLengthOut: nil)
        }
        guard let bb = CMSampleBufferGetDataBuffer(sb) else { return nil }
        let startCode: [UInt8] = [0, 0, 0, 1]
        var out = Data()
        if withParameterSets, let fd = CMSampleBufferGetFormatDescription(sb) {
            var count = 0
            _ = parameterSet(fd, 0, nil, nil, &count)
            for i in 0..<count {
                var p: UnsafePointer<UInt8>?
                var size = 0
                guard parameterSet(fd, i, &p, &size, nil) == noErr, let p else { return nil }
                out.append(contentsOf: startCode)
                out.append(p, count: size)
            }
        }
        let total = CMBlockBufferGetDataLength(bb)
        let head = out.count
        out.count = head + total
        let ok: Bool = out.withUnsafeMutableBytes { raw in
            let base = raw.baseAddress! + head
            guard CMBlockBufferCopyDataBytes(bb, atOffset: 0, dataLength: total, destination: base) == noErr else { return false }
            let p = base.assumingMemoryBound(to: UInt8.self)
            var off = 0
            while off + 4 <= total {   // 4-byte big-endian length → 4-byte start code, in place
                let len = Int(p[off]) << 24 | Int(p[off + 1]) << 16 | Int(p[off + 2]) << 8 | Int(p[off + 3])
                p[off] = 0; p[off + 1] = 0; p[off + 2] = 0; p[off + 3] = 1
                off += 4 + len
            }
            return off == total
        }
        return ok ? out : nil
    }
}
