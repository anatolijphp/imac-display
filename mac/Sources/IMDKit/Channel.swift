// One TCP connection of PROTOCOL.md (`ctrl` or `video`), blocking sockets.
import Foundation

public struct ChannelError: Error, CustomStringConvertible {
    public let description: String
    init(_ s: String) { description = s }
}

public final class Channel {
    private var fd: Int32 = -1
    private let sendLock = NSLock()
    public private(set) var peer = ""

    /// Connects to `host` (a name such as `imac-display.local`, or an address; an IPv6 link-local
    /// address needs its `%interface` suffix).
    public init(host: String, port: UInt16 = Wire.port, timeoutSeconds: Int = 5) throws {
        var hints = addrinfo()
        hints.ai_socktype = SOCK_STREAM
        var res: UnsafeMutablePointer<addrinfo>?
        let rc = getaddrinfo(host, String(port), &hints, &res)
        guard rc == 0, let first = res else { throw ChannelError("cannot resolve \(host): \(String(cString: gai_strerror(rc)))") }
        defer { freeaddrinfo(res) }
        var lastErr = "no addresses"
        var ai: UnsafeMutablePointer<addrinfo>? = first
        while let a = ai {
            let s = socket(a.pointee.ai_family, a.pointee.ai_socktype, a.pointee.ai_protocol)
            if s >= 0 {
                var tv = timeval(tv_sec: timeoutSeconds, tv_usec: 0)
                setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
                var one: Int32 = 1
                setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
                if connect(s, a.pointee.ai_addr, a.pointee.ai_addrlen) == 0 {
                    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))
                    var hostBuf = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                    getnameinfo(a.pointee.ai_addr, a.pointee.ai_addrlen, &hostBuf, socklen_t(hostBuf.count), nil, 0, NI_NUMERICHOST)
                    peer = String(cString: hostBuf)
                    fd = s
                    return
                }
                lastErr = String(cString: strerror(errno))
                Darwin.close(s)
            }
            ai = a.pointee.ai_next
        }
        throw ChannelError("cannot connect to \(host):\(port): \(lastErr)")
    }

    deinit { close() }

    public func close() {
        if fd >= 0 { Darwin.close(fd); fd = -1 }
    }

    /// Receive timeout; 0 blocks for ever.
    public func setReceiveTimeout(seconds: Double) {
        var tv = timeval(tv_sec: Int(seconds), tv_usec: Int32((seconds - Double(Int(seconds))) * 1e6))
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
    }

    public func send(_ type: MsgType, flags: UInt8 = 0, tile: UInt16 = 0, ptsUs: UInt64 = Wire.nowUs(), payload: Data = Data()) throws {
        let header = FrameHeader(length: UInt32(payload.count), type: type.rawValue, flags: flags, tile: tile, ptsUs: ptsUs).encoded()
        sendLock.lock(); defer { sendLock.unlock() }
        try writeAll(header)
        if !payload.isEmpty { try writeAll(payload) }
    }

    public func sendJSON(_ type: MsgType, _ object: [String: Any]) throws {
        try send(type, payload: try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]))
    }

    /// Blocks for the next frame. Throws on timeout or a closed connection.
    public func receive() throws -> (header: FrameHeader, payload: Data) {
        guard let h = FrameHeader(try readExactly(Wire.headerSize)) else { throw ChannelError("bad header") }
        guard Int(h.length) <= Wire.maxPayload else { throw ChannelError("payload of \(h.length) bytes is over the limit") }
        return (h, h.length == 0 ? Data() : try readExactly(Int(h.length)))
    }

    private func writeAll(_ d: Data) throws {
        try d.withUnsafeBytes { (p: UnsafeRawBufferPointer) in
            var off = 0
            while off < p.count {
                let n = Darwin.send(fd, p.baseAddress! + off, p.count - off, 0)
                if n <= 0 {
                    if n < 0 && errno == EINTR { continue }
                    throw ChannelError("send failed: \(String(cString: strerror(errno)))")
                }
                off += n
            }
        }
    }

    private func readExactly(_ count: Int) throws -> Data {
        var d = Data(count: count)
        try d.withUnsafeMutableBytes { (p: UnsafeMutableRawBufferPointer) in
            var off = 0
            while off < count {
                let n = recv(fd, p.baseAddress! + off, count - off, 0)
                if n == 0 { throw ChannelError("connection closed by the iMac") }
                if n < 0 {
                    if errno == EINTR { continue }
                    throw ChannelError(errno == EAGAIN ? "receive timed out" : "receive failed: \(String(cString: strerror(errno)))")
                }
                off += n
            }
        }
        return d
    }
}
