// Finds the iMac: the DNS-SD service `_imacdisplay._tcp` (PROTOCOL.md).
//
// The iMac advertises the service on the Thunderbolt link and on Wi-Fi, but answers on only one
// of them: the one chosen with the Thunderbolt / Wi-Fi switch on its waiting screen. So the app
// has no switch of its own. This class tries port 7500 on every link the service is seen on and
// reports the one that answers, Thunderbolt first.
//
// Tested over Wi-Fi. NOT YET TESTED over Thunderbolt: no cable has been connected so far.
import Foundation
import Network

public final class IMacFinder {
    public struct Found: Equatable, Sendable {
        /// An address to connect to, for example "169.254.7.1" or "fe80::1%bridge0".
        public let address: String
        /// True on the Thunderbolt link, false on anything else (Wi-Fi: for testing).
        public let thunderbolt: Bool
    }

    /// Called on the main queue when an iMac that answers is found, and with nil when it is gone.
    public var onChange: ((Found?) -> Void)?

    private final class Probe {
        let connection: NWConnection
        let thunderbolt: Bool
        var done = false
        var address: String?
        init(_ c: NWConnection, thunderbolt: Bool) { connection = c; self.thunderbolt = thunderbolt }
    }

    private var browser: NWBrowser?
    private var results: Set<NWBrowser.Result> = []
    private var probes: [Probe] = []   // one round of attempts, Thunderbolt first
    private var round = 0
    private var current: Found?
    private var timer: DispatchSourceTimer?

    public init() {}

    public func start() {
        let params = NWParameters.tcp
        params.includePeerToPeer = false
        let b = NWBrowser(for: .bonjour(type: "_imacdisplay._tcp", domain: nil), using: params)
        b.browseResultsChangedHandler = { [weak self] results, _ in
            guard let self else { return }
            self.results = results
            if results.isEmpty { self.set(nil) } else if self.current == nil { self.probe() }
        }
        b.start(queue: .main)
        browser = b
        // The iMac's switch can change without the advertisement changing: while nothing answers, keep trying.
        let t = DispatchSource.makeTimerSource(queue: .main)
        t.schedule(deadline: .now() + 3, repeating: 3)
        t.setEventHandler { [weak self] in if self?.current == nil { self?.probe() } }
        t.resume()
        timer = t
    }

    public func stop() {
        browser?.cancel(); browser = nil
        timer?.cancel(); timer = nil
        endRound()
    }

    /// The address did not work, or the session over it ended: forget it and look again.
    public func refresh() {
        current = nil
        probe()
    }

    /// The Thunderbolt Bridge is `bridge0` on macOS; its member ports show up as plain `enN`.
    private func isThunderbolt(_ i: NWInterface) -> Bool { i.name.hasPrefix("bridge") }

    private func set(_ found: Found?) {
        guard found != current else { return }
        current = found
        onChange?(found)
    }

    private func endRound() {
        round += 1   // answers from connections of the old round are ignored from here on
        probes.forEach { $0.connection.cancel() }
        probes = []
    }

    private func probe() {
        guard probes.isEmpty else { return }
        var seen = Set<String>()
        var targets: [(NWEndpoint, NWInterface)] = []
        for r in results { for i in r.interfaces where seen.insert(i.name).inserted { targets.append((r.endpoint, i)) } }
        guard !targets.isEmpty else { return }
        targets.sort { isThunderbolt($0.1) && !isThunderbolt($1.1) }
        round += 1
        let thisRound = round
        // On each link first with IPv4 only, then with whatever there is: an IPv4 address is the
        // easier one for mounting the iMac's drives (smb://) than a link-local IPv6 one.
        for (endpoint, interface) in targets { for v4Only in [true, false] {
            let params = NWParameters.tcp
            params.requiredInterface = interface
            if v4Only, let ip = params.defaultProtocolStack.internetProtocol as? NWProtocolIP.Options { ip.version = .v4 }
            let c = NWConnection(to: endpoint, using: params)
            let p = Probe(c, thunderbolt: isThunderbolt(interface))
            probes.append(p)
            c.stateUpdateHandler = { [weak self] state in
                guard let self, self.round == thisRound else { return }
                switch state {
                case .ready:
                    if case let .hostPort(host, _)? = c.currentPath?.remoteEndpoint { p.address = Self.clean("\(host)") }
                    p.done = true
                case .waiting, .failed, .cancelled:
                    p.done = true   // nothing listens on this link (the iMac's switch points at the other one)
                default:
                    return
                }
                self.decide()
            }
            c.start(queue: .main)
        } }
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
            guard let self, self.round == thisRound else { return }
            self.probes.forEach { $0.done = true }
            self.decide()
        }
    }

    /// Takes the first link that answered, in order of preference; waits while a better one is still out.
    private func decide() {
        guard !probes.isEmpty else { return }
        var winner: Probe?
        for p in probes {
            if !p.done { return }
            if p.address != nil { winner = p; break }
        }
        let found = winner.flatMap { p in p.address.map { Found(address: $0, thunderbolt: p.thunderbolt) } }
        endRound()
        if let found { set(found) } else { set(nil) }
    }

    /// "::ffff:192.168.1.5" and "192.168.1.5%en0" → "192.168.1.5"; a link-local IPv6 address keeps its "%en0".
    static func clean(_ address: String) -> String {
        var a = address
        if a.hasPrefix("::ffff:") { a.removeFirst(7) }
        if !a.contains(":"), let percent = a.firstIndex(of: "%") { a = String(a[..<percent]) }
        return a
    }
}
