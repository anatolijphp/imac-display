// One session with the iMac as in PROTOCOL.md "Session flow": ctrl + video connections,
// HELLO, CONFIG, once-a-second PING, 3-second timeout.
import Foundation

public final class Session {
    public let ctrl: Channel
    public private(set) var video: Channel?
    /// The iMac's HELLO (panel, decode limits, features).
    public let imacHello: [String: Any]
    public let sessionID = UUID().uuidString.lowercased()
    /// Called on the reader thread for every ctrl message except PING/PONG.
    public var onMessage: ((FrameHeader, Data) -> Void)?
    /// Called once when the session ends, with the reason.
    public var onEnd: ((String) -> Void)?

    /// The name or address this session is connected to.
    public let host: String
    private let port: UInt16
    private var early: [(FrameHeader, Data)] = []   // messages that arrived before CONFIG_ACK
    private var ended = false
    private let lock = NSLock()

    public init(host: String, port: UInt16 = Wire.port, name: String) throws {
        self.host = host; self.port = port
        ctrl = try Channel(host: host, port: port)
        try ctrl.sendJSON(.hello, ["proto": Wire.version, "role": "mac", "channel": "ctrl", "session": sessionID, "name": name])
        let (h, payload) = try ctrl.receive()
        let json = (try? JSONSerialization.jsonObject(with: payload)) as? [String: Any] ?? [:]
        if h.type == MsgType.bye.rawValue { throw ChannelError("the iMac refused the session: \(json["reason"] ?? "no reason given")") }
        guard h.type == MsgType.hello.rawValue else { throw ChannelError("expected HELLO, got type 0x\(String(h.type, radix: 16))") }
        guard json["proto"] as? Int == Wire.version else { throw ChannelError("protocol mismatch: the iMac speaks \(json["proto"] ?? "?")") }
        imacHello = json
    }

    /// Sends CONFIG and waits for CONFIG_ACK. Other messages that arrive first (the iMac announces
    /// drives right after its HELLO) are kept and delivered by `start()`.
    public func configure(_ config: [String: Any]) throws {
        try ctrl.sendJSON(.config, config)
        while true {
            let (h, payload) = try ctrl.receive()
            if h.type == MsgType.ping.rawValue { try ctrl.send(.pong, payload: payload); continue }
            if h.type == MsgType.configAck.rawValue {
                let json = (try? JSONSerialization.jsonObject(with: payload)) as? [String: Any] ?? [:]
                guard json["ok"] as? Bool == true else { throw ChannelError("the iMac rejected CONFIG: \(json["error"] ?? "no reason given")") }
                return
            }
            early.append((h, payload))
        }
    }

    public func openVideo() throws {
        let v = try Channel(host: host, port: port)
        try v.sendJSON(.hello, ["proto": Wire.version, "role": "mac", "channel": "video", "session": sessionID])
        let (h, _) = try v.receive()
        guard h.type == MsgType.hello.rawValue else { throw ChannelError("video channel: expected HELLO, got type 0x\(String(h.type, radix: 16))") }
        video = v
    }

    /// Starts the reader and the once-a-second PING.
    public func start() {
        for (h, p) in early { onMessage?(h, p) }
        early = []
        ctrl.setReceiveTimeout(seconds: 3)
        Thread.detachNewThread { [self] in
            while !isEnded {
                do {
                    let (h, payload) = try ctrl.receive()
                    switch MsgType(rawValue: h.type) {
                    case .ping: try ctrl.send(.pong, payload: payload)
                    case .pong: break
                    case .bye:
                        let json = (try? JSONSerialization.jsonObject(with: payload)) as? [String: Any]
                        end("the iMac said BYE: \(json?["reason"] ?? "no reason")", sendBye: false)
                    default: onMessage?(h, payload)   // unknown types are skipped by the caller
                    }
                } catch { end("\(error)", sendBye: false) }
            }
        }
        Thread.detachNewThread { [self] in
            var n: UInt64 = 0
            while !isEnded {
                n += 1
                var v = n.littleEndian
                do { try ctrl.send(.ping, payload: Data(bytes: &v, count: 8)) } catch { end("\(error)", sendBye: false) }
                Thread.sleep(forTimeInterval: 1)
            }
        }
    }

    public func sendVideo(tile: Int, keyframe: Bool, lastTile: Bool, ptsUs: UInt64, data: Data) throws {
        guard let video else { throw ChannelError("video channel is not open") }
        let flags = (keyframe ? VideoFlags.keyframe : 0) | (lastTile ? VideoFlags.lastTile : 0)
        try video.send(.videoFrame, flags: flags, tile: UInt16(tile), ptsUs: ptsUs, payload: data)
    }

    public var isEnded: Bool { lock.lock(); defer { lock.unlock() }; return ended }

    public func close(reason: String = "quit") { end(reason, sendBye: true) }

    private func end(_ reason: String, sendBye: Bool) {
        lock.lock()
        let first = !ended
        ended = true
        lock.unlock()
        guard first else { return }
        if sendBye { try? ctrl.sendJSON(.bye, ["reason": reason]) }
        ctrl.close()
        video?.close()
        onEnd?(reason)
    }
}
