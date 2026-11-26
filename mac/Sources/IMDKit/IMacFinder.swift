// Finds the iMac: the DNS-SD service `_imacdisplay._tcp` (PROTOCOL.md), by default only on the
// Thunderbolt link. NOT YET TESTED against a real iMac: the receiver does not advertise the
// service yet and no Thunderbolt cable has been connected so far.
import Foundation
import Network

public final class IMacFinder {
    /// Called on the main queue with an address to connect to (for example "169.254.7.1" or
    /// "fe80::1%bridge0") whenever an iMac is found, and with nil when it is gone.
    public var onChange: ((String?) -> Void)?
    public var thunderboltOnly = true

    private var browser: NWBrowser?
    private var resolver: NWConnection?
    private var current: String?

    public init() {}

    public func start() {
        let params = NWParameters.tcp
        params.includePeerToPeer = false
        let b = NWBrowser(for: .bonjour(type: "_imacdisplay._tcp", domain: nil), using: params)
        b.browseResultsChangedHandler = { [weak self] results, _ in self?.update(results) }
        b.start(queue: .main)
        browser = b
    }

    public func stop() {
        browser?.cancel(); browser = nil
        resolver?.cancel(); resolver = nil
    }

    /// The Thunderbolt Bridge is `bridge0` on macOS; its member ports show up as plain `enN`.
    private func isThunderbolt(_ i: NWInterface) -> Bool { i.name.hasPrefix("bridge") }

    private func update(_ results: Set<NWBrowser.Result>) {
        let usable = results.first { r in !thunderboltOnly || r.interfaces.contains(where: isThunderbolt) }
        guard let result = usable else {
            if current != nil { current = nil; onChange?(nil) }
            return
        }
        guard current == nil, resolver == nil else { return }
        // Resolve the service to an address by opening (and at once closing) a connection to it.
        let params = NWParameters.tcp
        if thunderboltOnly, let iface = result.interfaces.first(where: isThunderbolt) { params.requiredInterface = iface }
        let c = NWConnection(to: result.endpoint, using: params)
        resolver = c
        c.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready:
                if case let .hostPort(host, _)? = c.currentPath?.remoteEndpoint {
                    var address = "\(host)"
                    if address.hasPrefix("::ffff:") { address.removeFirst(7) }
                    self.current = address
                    self.onChange?(address)
                }
                c.cancel(); self.resolver = nil
            case .failed, .cancelled:
                self.resolver = nil
            default: break
            }
        }
        c.start(queue: .main)
    }
}
