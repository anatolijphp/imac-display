// iMac Display: the one app on the MacBook. A menu-bar icon; no windows.
// It finds the iMac, adds the extra display, sends picture and sound, takes the iMac's keyboard,
// mouse and drives, and installs its own sound device ("iMac Speakers") when asked.
import AppKit
import AVFoundation
import IMDKit
import ServiceManagement

let testingHostName = "imac-display.local"
let driverInstallPath = "/Library/Audio/Plug-Ins/HAL/iMacDisplayAudio.driver"

struct Mode {
    let key: String, title: String, pointsWide: Int, pointsHigh: Int, hiDPI: Bool
    static let all = [
        Mode(key: "1080", title: "1920 × 1080", pointsWide: 1920, pointsHigh: 1080, hiDPI: false),
        Mode(key: "1080r", title: "1920 × 1080 Retina (sent as 4K)", pointsWide: 1920, pointsHigh: 1080, hiDPI: true),
        Mode(key: "1440r", title: "2560 × 1440 Retina (sent as 4K)", pointsWide: 2560, pointsHigh: 1440, hiDPI: true),
    ]
}

@MainActor final class AppController: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let menu = NSMenu()
    private let finder = IMacFinder()
    private let defaults = UserDefaults.standard
    private var session: DisplaySession?
    private var connecting = false
    private var connected = false
    private var foundAddress: String?
    private var connectedMode: Mode?
    private var stats = DisplaySession.Stats()
    private var lastProblem: String?
    private var installingDriver = false
    private var timer: Timer?

    // Settings
    private var enabled: Bool { get { defaults.object(forKey: "enabled") as? Bool ?? true } set { defaults.set(newValue, forKey: "enabled") } }
    private var modeKey: String { get { defaults.string(forKey: "mode") ?? "auto" } set { defaults.set(newValue, forKey: "mode") } }
    private var testingOverWiFi: Bool { get { defaults.bool(forKey: "testingOverWiFi") } set { defaults.set(newValue, forKey: "testingOverWiFi") } }

    func applicationDidFinishLaunching(_ note: Notification) {
        statusItem.menu = menu
        menu.delegate = self
        updateIcon()
        log("started, version \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") ?? "?")")

        // Ask for what the app needs; macOS shows each question once.
        if !DisplayCapture.hasPermission { DisplayCapture.requestPermission() }
        if !InputInjector.hasPermission { InputInjector.requestPermission() }
        if SpeakerDevice.isInstalled && SpeakerDevice.microphonePermission == .notDetermined {
            Task { _ = await SpeakerDevice.requestMicrophonePermission() }
        }
        if driverState != .current && !defaults.bool(forKey: "askedAboutDriver") {
            defaults.set(true, forKey: "askedAboutDriver")
            offerDriver()
        }

        finder.onChange = { [weak self] address in
            self?.foundAddress = address
            self?.log(address.map { "iMac found on the Thunderbolt link at \($0)" } ?? "iMac no longer on the Thunderbolt link")
        }
        finder.start()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
        tick()
    }

    func applicationWillTerminate(_ note: Notification) { session?.stop(reason: "quit") }

    // MARK: connecting

    private func tick() {
        guard enabled, session == nil, !connecting, !installingDriver else { return }
        guard DisplayCapture.hasPermission else { return }   // nothing to send without it; the menu says so
        guard let host = testingOverWiFi ? testingHostName : foundAddress else { return }
        connect(to: host)
    }

    private func connect(to host: String) {
        // Automatic: the full desktop over the cable, a light one over Wi-Fi.
        let mode = Mode.all.first { $0.key == modeKey } ?? (testingOverWiFi ? Mode.all[0] : Mode.all[2])
        var c = DisplaySession.Config(host: host, macName: Host.current().localizedName ?? "Mac")
        c.pointsWide = mode.pointsWide; c.pointsHigh = mode.pointsHigh; c.hiDPI = mode.hiDPI
        let big = mode.hiDPI
        c.bitrate = testingOverWiFi ? (big ? 20_000_000 : 10_000_000) : (big ? 40_000_000 : 20_000_000)
        let s = DisplaySession(c)
        s.log = { [weak self] line in Task { @MainActor in self?.log(line) } }
        s.onStats = { [weak self] st in Task { @MainActor in self?.stats = st } }
        s.onEnd = { [weak self, weak s] reason in
            Task { @MainActor in
                guard let self, let s, self.session === s else { return }
                self.log("session ended: \(reason)")
                self.session = nil
                self.connected = false
                self.updateIcon()
            }
        }
        session = s
        connecting = true
        Task {
            do {
                try await s.start()
                connected = true
                connectedMode = mode
                lastProblem = nil
                stats = DisplaySession.Stats()
                log("connected to \(host): \(mode.title), stream \(s.streamWidth)x\(s.streamHeight), sound: \(s.soundSource)")
            } catch {
                let text = "\(error)"
                if lastProblem != text { log("could not connect to \(host): \(text)") }
                lastProblem = text
                s.stop(reason: "could not start")
                if session === s { session = nil }
            }
            connecting = false
            updateIcon()
        }
    }

    private func reconnect(_ why: String) {
        log(why)
        session?.stop(reason: why)
    }

    // MARK: menu

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        func info(_ text: String) { let i = NSMenuItem(title: text, action: nil, keyEquivalent: ""); i.isEnabled = false; menu.addItem(i) }
        func item(_ text: String, _ action: Selector, checked: Bool = false, tag: Int = 0) -> NSMenuItem {
            let i = NSMenuItem(title: text, action: action, keyEquivalent: "")
            i.target = self; i.state = checked ? .on : .off; i.tag = tag
            menu.addItem(i)
            return i
        }

        if connected, let mode = connectedMode {
            info("Connected to the iMac" + (testingOverWiFi ? " over Wi-Fi (testing)" : ""))
            info("\(mode.title), \(stats.decoded) pictures per second shown")
            switch session?.soundSource {
            case .speakersDevice?: info("Sound: whatever plays on “iMac Speakers”")
            case .everything?: info("Sound: everything this Mac plays")
            default: info("Sound: the iMac cannot play sound")
            }
        } else if !enabled {
            info("Off")
        } else if !DisplayCapture.hasPermission {
            info("Waiting for the Screen Recording permission")
        } else if connecting {
            info("Connecting to the iMac…")
        } else if testingOverWiFi {
            info("Looking for \(testingHostName) on Wi-Fi…")
        } else {
            info(foundAddress == nil ? "Looking for the iMac on the Thunderbolt cable…" : "iMac found, connecting…")
        }
        if !connected, let p = lastProblem { info("Last problem: \(p.prefix(90))") }
        menu.addItem(.separator())

        _ = item("Use the iMac as a display", #selector(toggleEnabled), checked: enabled)
        let res = NSMenuItem(title: "Resolution", action: nil, keyEquivalent: "")
        let sub = NSMenu()
        let auto = NSMenuItem(title: "Automatic", action: #selector(pickMode(_:)), keyEquivalent: "")
        auto.target = self; auto.representedObject = "auto"; auto.state = modeKey == "auto" ? .on : .off
        sub.addItem(auto)
        for m in Mode.all {
            let i = NSMenuItem(title: m.title, action: #selector(pickMode(_:)), keyEquivalent: "")
            i.target = self; i.representedObject = m.key; i.state = modeKey == m.key ? .on : .off
            sub.addItem(i)
        }
        res.submenu = sub
        menu.addItem(res)
        menu.addItem(.separator())

        // What still needs the user.
        var needs = false
        if !DisplayCapture.hasPermission { _ = item("Allow Screen Recording…", #selector(openScreenSettings)); needs = true }
        if !InputInjector.hasPermission { _ = item("Allow the iMac’s keyboard and mouse (Accessibility)…", #selector(openAccessibilitySettings)); needs = true }
        switch driverState {
        case .missing: _ = item("Install the “iMac Speakers” sound device…", #selector(installDriver)); needs = true
        case .outdated: _ = item("Update the “iMac Speakers” sound device…", #selector(installDriver)); needs = true
        case .current:
            if SpeakerDevice.microphonePermission != .authorized {
                _ = item("Allow sound for “iMac Speakers” (Microphone)…", #selector(openMicrophoneSettings)); needs = true
            }
        }
        if needs { menu.addItem(.separator()) }

        _ = item("Open at Login", #selector(toggleLogin), checked: SMAppService.mainApp.status == .enabled)
        _ = item("Testing: connect over Wi-Fi", #selector(toggleTesting), checked: testingOverWiFi)
        _ = item("Show Log", #selector(showLog))
        if driverState != .missing { _ = item("Remove the “iMac Speakers” sound device…", #selector(removeDriver)) }
        menu.addItem(.separator())
        _ = item("Quit iMac Display", #selector(quit))
    }

    private func updateIcon() {
        let name = connected ? "desktopcomputer" : "desktopcomputer.trianglebadge.exclamationmark"
        let image = NSImage(systemSymbolName: connected || !enabled ? "desktopcomputer" : name, accessibilityDescription: "iMac Display")
            ?? NSImage(systemSymbolName: "desktopcomputer", accessibilityDescription: "iMac Display")
        image?.isTemplate = true
        statusItem.button?.image = image
        statusItem.button?.appearsDisabled = !connected
    }

    @objc private func toggleEnabled() {
        enabled.toggle()
        if !enabled { reconnect("switched off") }
        updateIcon()
    }
    @objc private func pickMode(_ sender: NSMenuItem) {
        guard let key = sender.representedObject as? String, key != modeKey else { return }
        modeKey = key
        reconnect("resolution changed")
    }
    @objc private func toggleTesting() {
        testingOverWiFi.toggle()
        lastProblem = nil
        reconnect("link changed")
    }
    @objc private func toggleLogin() {
        do {
            if SMAppService.mainApp.status == .enabled { try SMAppService.mainApp.unregister() } else { try SMAppService.mainApp.register() }
        } catch { alert("Open at Login could not be changed", "\(error.localizedDescription)") }
    }
    @objc private func openScreenSettings() { openSettings("Privacy_ScreenCapture") }
    @objc private func openAccessibilitySettings() { InputInjector.requestPermission(); openSettings("Privacy_Accessibility") }
    @objc private func openMicrophoneSettings() {
        if SpeakerDevice.microphonePermission == .notDetermined {
            Task { if await SpeakerDevice.requestMicrophonePermission() { reconnect("sound permission given") } }
        } else { openSettings("Privacy_Microphone") }
    }
    private func openSettings(_ pane: String) {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(pane)") { NSWorkspace.shared.open(url) }
    }
    @objc private func showLog() { NSWorkspace.shared.open(logURL) }
    @objc private func quit() { NSApp.terminate(nil) }

    // MARK: sound device (the audio driver inside this app)

    private enum DriverState { case missing, outdated, current }
    private var bundledDriver: URL? { Bundle.main.url(forResource: "iMacDisplayAudio", withExtension: "driver") }
    private func driverVersion(at path: String) -> String? {
        (NSDictionary(contentsOfFile: path + "/Contents/Info.plist")?["CFBundleVersion"] as? String)
    }
    private var driverState: DriverState {
        guard let installed = driverVersion(at: driverInstallPath) else { return .missing }
        guard let bundled = bundledDriver.flatMap({ driverVersion(at: $0.path) }) else { return .current }
        return installed == bundled ? .current : .outdated
    }

    private func offerDriver() {
        let a = NSAlert()
        a.messageText = "Install the “iMac Speakers” sound device?"
        a.informativeText = "It adds “iMac Speakers” to this Mac’s sound outputs, so you can choose which sound plays on the iMac. macOS will ask for an administrator password, and sound pauses for a moment.\n\nWithout it, everything this Mac plays is sent to the iMac."
        a.addButton(withTitle: "Install")
        a.addButton(withTitle: "Not Now")
        NSApp.activate(ignoringOtherApps: true)
        if a.runModal() == .alertFirstButtonReturn { installDriver() }
    }

    @objc private func installDriver() {
        guard let source = bundledDriver else { alert("The sound device is missing from this copy of the app", "Build the app again with mac/scripts/build-app.sh."); return }
        let cmd = "mkdir -p /Library/Audio/Plug-Ins/HAL && rm -rf '\(driverInstallPath)' && cp -R '\(source.path)' '\(driverInstallPath)' && chown -R root:wheel '\(driverInstallPath)' && { launchctl kickstart -k system/com.apple.audio.coreaudiod || killall coreaudiod; }"
        runAsAdministrator(cmd, prompt: "iMac Display wants to install its sound device, iMac Speakers.") { [weak self] ok, message in
            guard let self else { return }
            if !ok { if !message.contains("-128") { self.alert("The sound device was not installed", message) }; return }   // -128: the user cancelled
            self.log("sound device installed")
            Task { @MainActor in
                for _ in 0..<20 where !SpeakerDevice.isInstalled { try? await Task.sleep(nanoseconds: 500_000_000) }
                guard SpeakerDevice.isInstalled else { self.alert("“iMac Speakers” did not appear", "The files are installed, but macOS did not load the device. Restarting the Mac usually fixes that."); return }
                if SpeakerDevice.microphonePermission != .authorized { _ = await SpeakerDevice.requestMicrophonePermission() }
                self.reconnect("sound device installed")
            }
        }
    }

    @objc private func removeDriver() {
        let cmd = "rm -rf '\(driverInstallPath)'; launchctl kickstart -k system/com.apple.audio.coreaudiod || killall coreaudiod"
        runAsAdministrator(cmd, prompt: "iMac Display wants to remove its sound device, iMac Speakers.") { [weak self] ok, message in
            if ok { self?.log("sound device removed"); self?.reconnect("sound device removed") }
            else if !message.contains("-128") { self?.alert("The sound device was not removed", message) }
        }
    }

    /// Runs a shell command as root after macOS's own password dialog. Never blocks the menu.
    private func runAsAdministrator(_ command: String, prompt: String, done: @escaping @MainActor (Bool, String) -> Void) {
        installingDriver = true
        session?.stop(reason: "sound device change")
        func quoted(_ s: String) -> String { s.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") }
        let script = "do shell script \"\(quoted(command))\" with prompt \"\(quoted(prompt))\" with administrator privileges"
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        p.arguments = ["-e", script]
        let err = Pipe()
        p.standardError = err
        p.standardOutput = Pipe()
        p.terminationHandler = { proc in
            let message = String(data: err.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
            Task { @MainActor in
                self.installingDriver = false
                done(proc.terminationStatus == 0, message.trimmingCharacters(in: .whitespacesAndNewlines))
            }
        }
        do { try p.run() } catch { installingDriver = false; done(false, "\(error)") }
    }

    // MARK: small things

    private func alert(_ title: String, _ text: String) {
        log("\(title): \(text)")
        let a = NSAlert()
        a.messageText = title
        a.informativeText = text
        NSApp.activate(ignoringOtherApps: true)
        a.runModal()
    }

    private let logURL: URL = {
        let dir = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0].appendingPathComponent("Logs")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("iMac Display.log")
    }()

    private func log(_ line: String) {
        let stamp = ISO8601DateFormatter().string(from: Date())
        let text = "\(stamp) \(line)\n"
        print(line)
        if let h = try? FileHandle(forWritingTo: logURL) {
            h.seekToEndOfFile(); h.write(text.data(using: .utf8)!); try? h.close()
        } else {
            try? text.write(to: logURL, atomically: true, encoding: .utf8)
        }
    }
}

setvbuf(stdout, nil, _IOLBF, 0)
MainActor.assumeIsolated {
    let app = NSApplication.shared
    let controller = AppController()
    app.delegate = controller
    app.setActivationPolicy(.accessory)
    withExtendedLifetime(controller) { app.run() }
}
