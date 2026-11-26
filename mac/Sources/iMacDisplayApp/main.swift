// iMac Display: the one app on the MacBook. A menu-bar icon; no windows.
// It finds the iMac, adds the extra display, sends picture and sound, takes the iMac's keyboard,
// mouse and drives, and installs its own sound device ("iMac Speakers") when asked.
import AppKit
import AVFoundation
import IMDKit
import ServiceManagement

let testingHostName = "imac-display.local"
let driverInstallPath = "/Library/Audio/Plug-Ins/HAL/iMacDisplayAudio.driver"

@MainActor final class AppController: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let menu = NSMenu()
    private let finder = IMacFinder()
    private let defaults = UserDefaults.standard
    private var session: DisplaySession?
    private var connecting = false
    private var connected = false
    private var foundAddress: String?
    private var stats = DisplaySession.Stats()
    private var lastProblem: String?
    private var installingDriver = false
    private var timer: Timer?

    // Settings
    private var enabled: Bool { get { defaults.object(forKey: "enabled") as? Bool ?? true } set { defaults.set(newValue, forKey: "enabled") } }
    private var switchSound: Bool { get { defaults.object(forKey: "switchSound") as? Bool ?? true } set { defaults.set(newValue, forKey: "switchSound") } }
    private var testingOverWiFi: Bool { get { defaults.bool(forKey: "testingOverWiFi") } set { defaults.set(newValue, forKey: "testingOverWiFi") } }

    func applicationDidFinishLaunching(_ note: Notification) {
        statusItem.menu = menu
        menu.delegate = self
        updateIcon()
        log("started, version \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") ?? "?")")

        checkDriver()
        Task { await checkPermissions() }

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
        guard enabled, session == nil, !connecting, !installingDriver else { return }
        guard DisplayCapture.hasPermission else { return }   // nothing to send without it; the menu says so
        guard let host = testingOverWiFi ? testingHostName : foundAddress else { return }
        connect(to: host)
    }

    private func connect(to host: String) {
        var c = DisplaySession.Config(host: host, macName: Host.current().localizedName ?? "Mac")
        c.selectSpeakers = switchSound
        c.maxBitrate = testingOverWiFi ? 20_000_000 : 40_000_000   // Wi-Fi here carries about 65 Mbit/s
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
                lastProblem = nil
                stats = DisplaySession.Stats()
                log("connected to \(host), sound: \(s.soundSource)")
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

        if connected, let s = session {
            info("Connected to the iMac" + (testingOverWiFi ? " over Wi-Fi (testing)" : ""))
            info("\(s.desktopWidth) × \(s.desktopHeight), \(stats.decoded) pictures per second shown")
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
        _ = item("Display Settings…", #selector(openDisplaySettings))
        _ = item("Play this Mac’s sound on the iMac when connected", #selector(toggleSwitchSound), checked: switchSound)
        menu.addItem(.separator())

        switch driverState {
        case .missing: _ = item("Install the “iMac Speakers” sound device…", #selector(installDriver))
        case .outdated: _ = item("Update the “iMac Speakers” sound device…", #selector(installDriver))
        case .current: break
        }
        menu.addItem(.separator())

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
    @objc private func openDisplaySettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.Displays-Settings.extension") { NSWorkspace.shared.open(url) }
    }
    @objc private func toggleSwitchSound() {
        switchSound.toggle()
        if connected { reconnect("sound setting changed") }
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

SpeakerDevice.runAsHelperIfAsked()   // a second copy of this program, started only to switch the sound output
setvbuf(stdout, nil, _IOLBF, 0)
MainActor.assumeIsolated {
    let app = NSApplication.shared
    let controller = AppController()
    app.delegate = controller
    app.setActivationPolicy(.accessory)
    withExtendedLifetime(controller) { app.run() }
}
