// Cuts one NV12 picture into tiles, each a separate pixel buffer for its own encoder.
import CoreVideo
import Foundation

public struct TileRect {
    public let id: Int, x: Int, y: Int, w: Int, h: Int
    public init(id: Int, x: Int, y: Int, w: Int, h: Int) { self.id = id; self.x = x; self.y = y; self.w = w; self.h = h }

    /// `cols` × `rows` equal tiles over a `width` × `height` picture (sizes must divide evenly and be even).
    public static func grid(width: Int, height: Int, cols: Int, rows: Int) -> [TileRect] {
        let w = width / cols, h = height / rows
        return (0..<rows * cols).map { TileRect(id: $0, x: ($0 % cols) * w, y: ($0 / cols) * h, w: w, h: h) }
    }
}

public final class TileSplitter {
    public let tiles: [TileRect]
    private var pools: [CVPixelBufferPool] = []

    public init?(tiles: [TileRect]) {
        self.tiles = tiles
        for t in tiles {
            let attrs: [CFString: Any] = [
                kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                kCVPixelBufferWidthKey: t.w, kCVPixelBufferHeightKey: t.h,
                kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary,
            ]
            var pool: CVPixelBufferPool?
            guard CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pool) == kCVReturnSuccess, let pool else { return nil }
            pools.append(pool)
        }
    }

    /// Copies each tile out of `src` (NV12). The copies run in parallel.
    public func split(_ src: CVPixelBuffer) -> [CVPixelBuffer]? {
        guard CVPixelBufferGetPixelFormatType(src) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
              CVPixelBufferGetPlaneCount(src) == 2 else { return nil }
        var outs = [CVPixelBuffer?](repeating: nil, count: tiles.count)
        for i in tiles.indices {
            guard CVPixelBufferPoolCreatePixelBuffer(nil, pools[i], &outs[i]) == kCVReturnSuccess else { return nil }
        }
        let dsts = outs.compactMap { $0 }
        guard dsts.count == tiles.count else { return nil }
        CVPixelBufferLockBaseAddress(src, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(src, .readOnly) }
        let attachments = CVBufferCopyAttachments(src, .shouldPropagate)
        DispatchQueue.concurrentPerform(iterations: tiles.count) { i in
            let t = tiles[i], dst = dsts[i]
            CVPixelBufferLockBaseAddress(dst, [])
            for plane in 0..<2 {
                // Plane 0 is luma, one byte per pixel. Plane 1 is CbCr pairs at half height: w bytes per row too.
                let rows = plane == 0 ? t.h : t.h / 2
                let firstRow = plane == 0 ? t.y : t.y / 2
                let sStride = CVPixelBufferGetBytesPerRowOfPlane(src, plane)
                let dStride = CVPixelBufferGetBytesPerRowOfPlane(dst, plane)
                guard let s = CVPixelBufferGetBaseAddressOfPlane(src, plane),
                      let d = CVPixelBufferGetBaseAddressOfPlane(dst, plane) else { continue }
                for r in 0..<rows {
                    memcpy(d + r * dStride, s + (firstRow + r) * sStride + t.x, t.w)
                }
            }
            CVPixelBufferUnlockBaseAddress(dst, [])
            if let attachments { CVBufferSetAttachments(dst, attachments, .shouldPropagate) }
        }
        return dsts
    }
}
