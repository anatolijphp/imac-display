// Wire format from PROTOCOL.md, version 0. PROTOCOL.md is the authority; this file follows it.
import Foundation

public enum Wire {
    public static let version = 0
    public static let port: UInt16 = 7500
    public static let maxPayload = 64 << 20
    public static let headerSize = 16

    /// The sender's monotonic clock in microseconds (`pts_us`).
    public static func nowUs() -> UInt64 { clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000 }
}

public enum MsgType: UInt8 {
    case hello = 0x01, config = 0x02, configAck = 0x03, ping = 0x04, pong = 0x05, bye = 0x06
    case videoFrame = 0x10, videoRequestKeyframe = 0x11, videoStats = 0x12
    case audioOut = 0x20, audioIn = 0x21, audioVolume = 0x22
    case inputKey = 0x30, inputPointerRel = 0x31, inputButton = 0x32
    case storageAttached = 0x40, storageEject = 0x41, storageDetached = 0x42
    case displayBrightness = 0x50, displayPower = 0x51
}

public struct VideoFlags {
    public static let keyframe: UInt8 = 1 << 0
    public static let lastTile: UInt8 = 1 << 1
}

public struct FrameHeader {
    public var length: UInt32
    public var type: UInt8
    public var flags: UInt8
    public var tile: UInt16
    public var ptsUs: UInt64

    public init(length: UInt32, type: UInt8, flags: UInt8 = 0, tile: UInt16 = 0, ptsUs: UInt64) {
        self.length = length; self.type = type; self.flags = flags; self.tile = tile; self.ptsUs = ptsUs
    }

    public func encoded() -> Data {
        var d = Data(count: Wire.headerSize)
        d.withUnsafeMutableBytes { p in
            p.storeBytes(of: length.littleEndian, toByteOffset: 0, as: UInt32.self)
            p.storeBytes(of: type, toByteOffset: 4, as: UInt8.self)
            p.storeBytes(of: flags, toByteOffset: 5, as: UInt8.self)
            p.storeBytes(of: tile.littleEndian, toByteOffset: 6, as: UInt16.self)
            p.storeBytes(of: ptsUs.littleEndian, toByteOffset: 8, as: UInt64.self)
        }
        return d
    }

    public init?(_ d: Data) {
        guard d.count == Wire.headerSize else { return nil }
        let b = [UInt8](d)
        length = UInt32(b[0]) | UInt32(b[1]) << 8 | UInt32(b[2]) << 16 | UInt32(b[3]) << 24
        type = b[4]; flags = b[5]
        tile = UInt16(b[6]) | UInt16(b[7]) << 8
        var v: UInt64 = 0
        for i in (8..<16).reversed() { v = v << 8 | UInt64(b[i]) }
        ptsUs = v
    }
}
