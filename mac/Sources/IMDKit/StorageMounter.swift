// Mounts the drives the iMac announces (STORAGE_ATTACHED) as network volumes in Finder,
// tells the iMac when the user ejects one, and unmounts when the iMac says a drive is gone.
import AppKit
import NetFS

public final class StorageMounter {
    private struct Volume { var share: String; var label: String; var mountPath: String? }
    private var volumes: [String: Volume] = [:]   // by the iMac's drive id
    private let lock = NSLock()
    private let host: String
    private let sendEject: (String) -> Void
    private let log: (String) -> Void
    private var observer: NSObjectProtocol?

    /// `host` is the name or address the session is connected to. `sendEject(id)` sends STORAGE_EJECT.
    public init(host: String, sendEject: @escaping (String) -> Void, log: @escaping (String) -> Void) {
        self.host = host; self.sendEject = sendEject; self.log = log
        observer = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didUnmountNotification, object: nil, queue: nil
        ) { [weak self] note in
            guard let self, let url = note.userInfo?[NSWorkspace.volumeURLUserInfoKey] as? URL else { return }
            self.lock.lock()
            let hit = self.volumes.first { $0.value.mountPath == url.path }
            if let hit { self.volumes[hit.key]?.mountPath = nil }
            self.lock.unlock()
            if let hit {   // the user ejected it in Finder: the iMac can now unmount and power the drive off
                self.log("drive \"\(hit.value.label)\" ejected on the MacBook; telling the iMac")
                self.sendEject(hit.key)
            }
        }
    }

    deinit { if let observer { NSWorkspace.shared.notificationCenter.removeObserver(observer) } }

    public func attached(_ json: [String: Any]) {
        guard let id = json["id"] as? String, let share = json["share"] as? String,
              let user = json["user"] as? String, let password = json["password"] as? String else {
            log("STORAGE_ATTACHED without id, share, user or password; ignored")
            return
        }
        let label = json["label"] as? String ?? share
        let readOnly = json["read_only"] as? Bool ?? false
        var address = host
        if address.contains(":") { address = "[" + address.replacingOccurrences(of: "%", with: "%25") + "]" }   // IPv6 literal
        // Share names come from drive labels and may contain spaces and other characters a URL cannot hold.
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        let encodedShare = share.addingPercentEncoding(withAllowedCharacters: allowed) ?? share
        guard let url = URL(string: "smb://\(address)/\(encodedShare)") else { log("cannot build a URL for share \(share)"); return }
        lock.lock(); volumes[id] = Volume(share: share, label: label, mountPath: nil); lock.unlock()

        let open = NSMutableDictionary(); open[kNAUIOptionKey] = kNAUIOptionNoUI
        let mount = NSMutableDictionary()
        mount[kNetFSSoftMountKey] = true                       // a vanished iMac must not hang Finder
        if readOnly { mount[kNetFSMountFlagsKey] = Int(MNT_RDONLY) }
        var request: AsyncRequestID?
        let rc = NetFSMountURLAsync(url as CFURL, nil, user as CFString, password as CFString,
                                    open as CFMutableDictionary, mount as CFMutableDictionary, &request, DispatchQueue.global()) { [weak self] status, _, mountpoints in
            guard let self else { return }
            guard status == 0, let path = (mountpoints as? [String])?.first else {
                self.log("drive \"\(label)\": mounting \(url.absoluteString) failed (\(status): \(String(cString: strerror(status))))")
                return
            }
            self.lock.lock(); self.volumes[id]?.mountPath = path; self.lock.unlock()
            self.log("drive \"\(label)\" (\(json["fs"] ?? "?"), \(readOnly ? "read-only" : "read-write")) mounted at \(path)")
        }
        if rc != 0 { log("drive \"\(label)\": mount request refused (\(rc))") }
    }

    public func detached(_ json: [String: Any]) {
        guard let id = json["id"] as? String else { return }
        lock.lock(); let v = volumes.removeValue(forKey: id); lock.unlock()
        guard let v else { return }
        if let path = v.mountPath { unmount(path, label: v.label) } else { log("drive \"\(v.label)\" is safe to remove") }
    }

    /// End of session: nothing of ours stays mounted.
    public func unmountAll() {
        lock.lock(); let all = volumes; volumes = [:]; lock.unlock()
        for v in all.values { if let path = v.mountPath { unmount(path, label: v.label) } }
    }

    private func unmount(_ path: String, label: String) {
        // The drive is already gone on the iMac, so do not wait for it.
        let rc = Darwin.unmount(path, MNT_FORCE)
        log(rc == 0 ? "drive \"\(label)\" removed" : "drive \"\(label)\": unmounting \(path) failed (\(String(cString: strerror(errno))))")
    }
}
