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
    public private(set) var completeFrames = 0
    public private(set) var idleFrames = 0
    public private(set) var otherFrames = 0

    private var stream: SCStream?
    private let queue = DispatchQueue(label: "imd.capture", qos: .userInteractive)

    public static var hasPermission: Bool { CGPreflightScreenCaptureAccess() }
    /// Shows the system prompt the first time; afterwards it only reports the state.
    @discardableResult public static func requestPermission() -> Bool { CGRequestScreenCaptureAccess() }

    public func start(displayID: CGDirectDisplayID, width: Int, height: Int, fps: Int, showCursor: Bool = true, queueDepth: Int = 3) async throws {
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
        let s = SCStream(filter: SCContentFilter(display: display, excludingWindows: []), configuration: cfg, delegate: self)
        try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        try await s.startCapture()
        stream = s
    }

    public func stop() async {
        try? await stream?.stopCapture()
        stream = nil
    }

    public func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, sb.isValid else { return }
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

    public func stream(_ stream: SCStream, didStopWithError error: Error) { onStop?(error) }
}
