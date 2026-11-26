import Foundation

/// Collects samples from any thread and reports percentiles.
public final class Samples {
    private var values: [Double] = []
    private let lock = NSLock()
    public init() {}
    public func add(_ v: Double) { lock.lock(); values.append(v); lock.unlock() }
    public var count: Int { lock.lock(); defer { lock.unlock() }; return values.count }
    public var sum: Double { lock.lock(); defer { lock.unlock() }; return values.reduce(0, +) }
    public func percentile(_ p: Double) -> Double {
        lock.lock(); defer { lock.unlock() }
        guard !values.isEmpty else { return .nan }
        let s = values.sorted()
        return s[min(s.count - 1, Int((Double(s.count) * p / 100).rounded(.down)))]
    }
    /// "p50 / p95 / max" with one decimal.
    public var summary: String {
        guard count > 0 else { return "no samples" }
        return String(format: "%.1f / %.1f / %.1f", percentile(50), percentile(95), percentile(100))
    }
}
