// A display that exists only in software: macOS lays out windows on it like on a real monitor.
import AppKit
import CVirtualDisplay

public final class VirtualDisplay {
    private let display: CGVirtualDisplay
    public var displayID: CGDirectDisplayID { display.displayID }
    public let pointsWide: Int
    public let pointsHigh: Int
    public let hiDPI: Bool
    public var pixelsWide: Int { pointsWide * (hiDPI ? 2 : 1) }
    public var pixelsHigh: Int { pointsHigh * (hiDPI ? 2 : 1) }

    /// `pointsWide` × `pointsHigh` is the desktop size. With `hiDPI` the picture has twice as many
    /// pixels each way (2560×1440 points → 5120×2880 pixels).
    public init?(name: String, pointsWide: Int, pointsHigh: Int, hiDPI: Bool, hz: Double = 60,
                 onTermination: (() -> Void)? = nil) {
        self.pointsWide = pointsWide; self.pointsHigh = pointsHigh; self.hiDPI = hiDPI
        let d = CGVirtualDisplayDescriptor()
        d.queue = DispatchQueue.main
        d.name = name
        d.maxPixelsWide = UInt32(pointsWide * (hiDPI ? 2 : 1))
        d.maxPixelsHigh = UInt32(pointsHigh * (hiDPI ? 2 : 1))
        d.sizeInMillimeters = CGSize(width: 597, height: 336)  // the iMac's 27-inch panel
        d.vendorID = 0x1D5C   // arbitrary, not a registered vendor
        d.productID = 0x1701
        d.serialNum = 1
        // sRGB primaries and D65 white point.
        d.redPrimary = CGPoint(x: 0.640, y: 0.330)
        d.greenPrimary = CGPoint(x: 0.300, y: 0.600)
        d.bluePrimary = CGPoint(x: 0.150, y: 0.060)
        d.whitePoint = CGPoint(x: 0.3127, y: 0.3290)
        d.terminationHandler = { _, _ in onTermination?() }
        guard let v = CGVirtualDisplay(descriptor: d) else { return nil }
        let s = CGVirtualDisplaySettings()
        s.hiDPI = hiDPI ? 1 : 0
        s.modes = [CGVirtualDisplayMode(width: UInt32(pointsWide), height: UInt32(pointsHigh), refreshRate: hz)]
        guard v.apply(s) else { return nil }
        display = v
    }

    /// The matching `NSScreen`. It appears a moment after the display is created.
    @MainActor public var screen: NSScreen? {
        NSScreen.screens.first {
            ($0.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value == displayID
        }
    }
}
