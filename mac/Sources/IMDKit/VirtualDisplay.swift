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

    /// The resolutions a 27-inch 5K display offers in macOS's Display settings ("Larger Text" … "More Space").
    public static let imacModes = [(2560, 1440), (2304, 1296), (2048, 1152), (1920, 1080), (1600, 900), (1280, 720)]

    /// One resolution only (the test tool).
    public convenience init?(name: String, pointsWide: Int, pointsHigh: Int, hiDPI: Bool, hz: Double = 60,
                             onTermination: (() -> Void)? = nil) {
        self.init(name: name, modes: [(pointsWide, pointsHigh)], hiDPI: hiDPI, hz: hz, onTermination: onTermination)
    }

    /// The size of the picture right now, in pixels, as chosen in macOS's Display settings.
    public var currentPixelSize: (width: Int, height: Int)? {
        guard let m = CGDisplayCopyDisplayMode(displayID), m.pixelWidth > 0 else { return nil }
        return (m.pixelWidth, m.pixelHeight)
    }
    /// The size of the desktop right now, in points.
    public var currentPointSize: (width: Int, height: Int)? {
        guard let m = CGDisplayCopyDisplayMode(displayID), m.width > 0 else { return nil }
        return (m.width, m.height)
    }

    /// `modes` are desktop sizes in points; the user picks one in macOS's Display settings and macOS
    /// remembers the choice for this display. The first is the largest. With `hiDPI` each has twice as
    /// many pixels each way (2560×1440 points → 5120×2880 pixels).
    public init?(name: String, modes: [(Int, Int)], hiDPI: Bool, hz: Double = 60,
                 onTermination: (() -> Void)? = nil) {
        guard let (pointsWide, pointsHigh) = modes.first else { return nil }
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
        s.modes = modes.map { CGVirtualDisplayMode(width: UInt32($0.0), height: UInt32($0.1), refreshRate: hz) }
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
