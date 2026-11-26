// iMac Display: the one app on the MacBook. A menu-bar icon; no windows.
// It finds the iMac, adds the extra display, sends picture and sound, takes the iMac's keyboard,
// mouse and drives, and installs its own sound device ("iMac Speakers") when asked.
import AppKit
import AVFoundation
import IMDKit
import ServiceManagement

let driverInstallPath = "/Library/Audio/Plug-Ins/HAL/iMacDisplayAudio.driver"

@MainActor final class AppController: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let menu = NSMenu()
    private let finder = IMacFinder()
    private let defaults = UserDefaults.standard
    private var session: DisplaySession?
    private var connecting = false
    private var connected = false
    private var found: IMacFinder.Found?        // where the iMac answers right now
    private var overThunderbolt = true          // the link of the current session
    private var lastFoundLogged: IMacFinder.Found?
    private var stats = DisplaySession.Stats()
    private var lastProblem: String?
    private var installingDriver = false
    private var nextAttempt = Date.distantPast   // after a failed start, wait longer before the next one
    private var failures = 0
    private var timer: Timer?

    // Settings
    private var useDisplay: Bool { get { defaults.object(forKey: "useDisplay") as? Bool ?? true } set { defaults.set(newValue, forKey: "useDisplay") } }
    private var useSound: Bool { get { defaults.object(forKey: "useSound") as? Bool ?? true } set { defaults.set(newValue, forKey: "useSound") } }
    private var enabled: Bool { useDisplay || useSound }

    func applicationDidFinishLaunching(_ note: Notification) {
        statusItem.menu = menu
        menu.delegate = self
        updateIcon()
        log("started, version \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") ?? "?")")

        checkDriver()
        Task { await checkPermissions() }

        // Which link is used is chosen on the iMac (the switch on its waiting screen); the app follows.
        finder.onChange = { [weak self] found in
            guard let self else { return }
            if let found, found != self.lastFoundLogged {
                self.log("iMac found at \(found.address) (\(found.thunderbolt ? "Thunderbolt" : "Wi-Fi, for testing"))")
                self.lastFoundLogged = found
            }
            self.found = found
        }
        finder.start()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
        tick()
    }

    func applicationWillTerminate(_ note: Notification) { session?.stop(reason: "quit") }

    // MARK: permissions (checked at every launch, asked for when missing)

    private func checkDriver() {
        if driverState == .outdated {
            offerDriver(update: true)   // an older sound device from an earlier version of this app
        } else if driverState == .missing && !defaults.bool(forKey: "askedAboutDriver") {
            defaults.set(true, forKey: "askedAboutDriver")
            offerDriver(update: false)
        }
    }

    private func checkPermissions() async {
        // Screen Recording: without it there is no picture.
        if !DisplayCapture.hasPermission {
            log("permission missing: Screen Recording")
            let firstTime = !defaults.bool(forKey: "askedScreen")
            defaults.set(true, forKey: "askedScreen")
            DisplayCapture.requestPermission()          // macOS shows its own question only the first time
            if !firstTime {
                askFor("iMac Display is not allowed to record the screen",
                       "Without this there is no picture on the iMac. Switch on “iMac Display” under Screen & System Audio Recording, then open the app again.",
                       pane: "Privacy_ScreenCapture")
            }
        }
        // Accessibility: the iMac's keyboard and mouse.
        if !InputInjector.hasPermission {
            log("permission missing: Accessibility")
            let firstTime = !defaults.bool(forKey: "askedAccessibility")
            defaults.set(true, forKey: "askedAccessibility")
            InputInjector.requestPermission()
            if !firstTime {
                askFor("iMac Display is not allowed to control this Mac",
                       "Without this the keyboard and mouse plugged into the iMac do nothing. Switch on “iMac Display” under Accessibility.",
                       pane: "Privacy_Accessibility")
            }
        }
        // Microphone: only for the "iMac Speakers" device, and only once it is installed.
        if SpeakerDevice.isInstalled {
            switch SpeakerDevice.microphonePermission {
            case .authorized: break
            case .notDetermined:
                if await SpeakerDevice.requestMicrophonePermission() { reconnect("sound permission given") }
            default:
                log("permission missing: Microphone")
                askFor("iMac Display is not allowed to use its sound device",
                       "macOS asks for the Microphone permission because the app records from its own “iMac Speakers” device; it does not use a microphone. Without it the app copies everything this Mac plays instead. Switch on “iMac Display” under Microphone.",
                       pane: "Privacy_Microphone")
            }
        }
    }

    private func askFor(_ title: String, _ text: String, pane: String) {
        let a = NSAlert()
        a.messageText = title
        a.informativeText = text
        a.addButton(withTitle: "Open System Settings")
        a.addButton(withTitle: "Not Now")
        NSApp.activate(ignoringOtherApps: true)
        if a.runModal() == .alertFirstButtonReturn { openSettings(pane) }
    }

    // MARK: connecting

    private func tick() {
        guard enabled, session == nil, !connecting, !installingDriver, Date() >= nextAttempt else { return }
        guard !useDisplay || DisplayCapture.hasPermission else { return }   // no picture without it; the menu says so
        guard let found else { return }
        connect(to: found)
    }

    private func connect(to found: IMacFinder.Found) {
        let host = found.address
        overThunderbolt = found.thunderbolt
        var c = DisplaySession.Config(host: host, macName: Host.current().localizedName ?? "Mac")
        c.useDisplay = useDisplay
        c.useSound = useSound
        c.maxBitrate = found.thunderbolt ? 40_000_000 : 20_000_000   // Wi-Fi here does not carry 40 (docs/mac-spikes.md, section 7)
        let s = DisplaySession(c)
        s.log = { [weak self] line in Task { @MainActor in self?.log(line) } }
        s.onStats = { [weak self] st in Task { @MainActor in self?.stats = st } }
        s.onEnd = { [weak self, weak s] reason in
            Task { @MainActor in
                guard let self, let s, self.session === s else { return }
                self.log("session ended: \(reason)")
                self.session = nil
                self.connected = false
                self.found = nil
                self.finder.refresh()   // the iMac may have been switched to the other link
                self.updateIcon()
            }
        }
        session = s
        connecting = true
        Task {
            do {
                try await s.start()
                connected = true
                failures = 0
                lastProblem = nil
                stats = DisplaySession.Stats()
                log("connected to \(host), sound: \(s.soundSource)")
            } catch {
                let text = "\(error)"
                if lastProblem != text { log("could not connect to \(host): \(text)") }
                lastProblem = text
                // Each failed start may show and remove the extra display: back off instead of repeating every few seconds.
                failures += 1
                nextAttempt = Date().addingTimeInterval(min(60, Double(failures) * 10))
                s.stop(reason: "could not start")
                if session === s { session = nil }
                self.found = nil
                finder.refresh()
            }
            connecting = false
            updateIcon()
        }
    }

    private func reconnect(_ why: String) {
        log(why)
        failures = 0
        nextAttempt = .distantPast
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

        if connected, let s = session {
            info("Connected to the iMac over " + (overThunderbolt ? "Thunderbolt" : "Wi-Fi (testing)"))
            // Only changed pictures are sent: with a still screen the rate is 0, which would read like a fault.
            if useDisplay { info("Display: \(s.desktopWidth) × \(s.desktopHeight)" + (stats.decoded > 0 ? ", \(stats.decoded) pictures per second" : "")) }
            if useSound {
                switch s.soundSource {
                case .speakersDevice: info("Sound: on “iMac Speakers”")
                case .everything: info("Sound: everything this Mac plays")
                case .none: info("Sound: not available")
                }
            }
        } else if !enabled {
            info("Off")
        } else if useDisplay && !DisplayCapture.hasPermission {
            info("Waiting for the Screen Recording permission")
        } else if connecting {
            info("Connecting to the iMac…")
        } else {
            info(found == nil ? "Looking for the iMac…" : "iMac found, connecting…")
        }
        if !connected, let p = lastProblem { info("Last problem: \(p.prefix(90))") }
        menu.addItem(.separator())

        _ = item("Use Display", #selector(toggleDisplay), checked: useDisplay)
        _ = item("Use Sound", #selector(toggleSound), checked: useSound)
        menu.addItem(.separator())

        switch driverState {
        case .missing: _ = item("Install the “iMac Speakers” sound device…", #selector(installDriver)); menu.addItem(.separator())
        case .outdated: _ = item("Update the “iMac Speakers” sound device…", #selector(installDriver)); menu.addItem(.separator())
        case .current: break
        }

        _ = item("Open at Login", #selector(toggleLogin), checked: SMAppService.mainApp.status == .enabled)
        _ = item("Show Log", #selector(showLog))
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

    @objc private func toggleDisplay() {
        useDisplay.toggle()
        reconnect(useDisplay ? "display switched on" : "display switched off")
        if useDisplay { Task { await checkPermissions() } }
        updateIcon()
    }
    @objc private func toggleSound() {
        useSound.toggle()
        reconnect(useSound ? "sound switched on" : "sound switched off")
        updateIcon()
    }
    @objc private func toggleLogin() {
        do {
            if SMAppService.mainApp.status == .enabled { try SMAppService.mainApp.unregister() } else { try SMAppService.mainApp.register() }
        } catch { alert("Open at Login could not be changed", "\(error.localizedDescription)") }
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

    private func offerDriver(update: Bool) {
        let a = NSAlert()
        a.messageText = update ? "Update the “iMac Speakers” sound device?" : "Install the “iMac Speakers” sound device?"
        a.informativeText = (update ? "This version of iMac Display comes with a newer sound device. " : "")
            + "“iMac Speakers” appears in this Mac’s sound outputs while the iMac is connected, and the Mac’s sound moves to it, like the speakers of a monitor. macOS will ask for an administrator password, and sound pauses for a moment.\n\nWithout it, everything this Mac plays is copied to the iMac while the MacBook keeps playing too."
        a.addButton(withTitle: update ? "Update" : "Install")
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

SpeakerDevice.runAsHelperIfAsked()   // a second copy of this program, started only to switch the sound output
setvbuf(stdout, nil, _IOLBF, 0)
MainActor.assumeIsolated {
    let app = NSApplication.shared
    let controller = AppController()
    app.delegate = controller
    app.setActivationPolicy(.accessory)
    withExtendedLifetime(controller) { app.run() }
}
