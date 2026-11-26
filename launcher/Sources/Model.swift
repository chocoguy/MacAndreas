import AppKit
import Foundation

@MainActor
final class LauncherModel: ObservableObject {
    @Published var config = LauncherConfig.load()
    @Published var verify: VerifyResult?
    @Published var verifyError: String?
    @Published var conversion: ConversionState = .needed("Choose the game folder first.")
    @Published var converting = false
    @Published var phase = ""
    @Published var fraction: Double?
    @Published var lastLine = ""
    @Published var running = false
    @Published var status = ""
    @Published var displays: [DisplayChoice] = []

    private var host: Process?

    init(config initial: LauncherConfig = LauncherConfig.load()) {
        config = initial
        displays = availableDisplays()
        if config.display >= displays.count { config.display = 0 }
        if config.width == 0 || config.height == 0 {
            let r = defaultResolution(for: selectedDisplay)
            config.width = r.width
            config.height = r.height
        }
    }

    func start() {
        if !config.gamePath.isEmpty { refresh() }
    }

    var selectedDisplay: DisplayChoice? { displays.first { $0.id == config.display } }
    var canConvert: Bool {
        guard verify?.supported == true, !converting, !running else { return false }
        if case .needed = conversion { return true }
        return false
    }
    var canPlay: Bool { verify?.supported == true && conversion == .upToDate && !converting && !running }

    func chooseGame() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.message = "Choose the GTA San Andreas folder that contains gta_sa.exe (1.0 US)."
        panel.prompt = "Choose"
        if !config.gamePath.isEmpty { panel.directoryURL = URL(fileURLWithPath: config.gamePath) }
        guard panel.runModal() == .OK, let url = panel.url else { return }
        config.gamePath = url.path
        save()
        refresh()
    }

    func save() { config.save() }

    func refresh() {
        verify = nil
        verifyError = nil
        let path = config.gamePath
        Task.detached {
            let result = Result { try verifyGame(path) }
            let state: ConversionState?
            if case .success(let v) = result, v.supported {
                state = conversionState(exeSha256: v.sha256)
            } else {
                state = nil
            }
            await MainActor.run {
                switch result {
                case .success(let v):
                    self.verify = v
                    self.conversion = state ?? .unavailable(v.message)
                case .failure(let e):
                    self.verifyError = String(describing: e)
                    self.conversion = .unavailable("Fix the game folder first.")
                }
            }
        }
    }

    func convert() {
        guard let verify, canConvert else { return }
        converting = true
        phase = "Lifting"
        fraction = nil
        lastLine = ""
        let exe = verify.path
        let sha = verify.sha256
        Task.detached {
            let result = Result {
                try convertGame(exe: exe) { progress in
                    Task { @MainActor in
                        self.phase = progress.phase
                        if let f = progress.fraction { self.fraction = f }
                        self.lastLine = progress.line
                    }
                }
                try Stamp.current(exeSha256: sha).save()
            }
            await MainActor.run {
                self.converting = false
                switch result {
                case .success:
                    self.phase = "Done"
                    self.fraction = 1
                    self.conversion = .upToDate
                case .failure(let e):
                    self.phase = "Failed"
                    self.conversion = .needed(String(describing: e))
                }
            }
        }
    }

    func play() {
        guard canPlay else { return }
        save()
        do {
            host = try launchHost(config) { code in
                Task { @MainActor in
                    self.running = false
                    self.host = nil
                    self.status = code == 0 ? "The game exited normally." : "The game exited with status \(code). See \(Paths.hostLog.path)."
                }
            }
            running = true
            status = "Running. The log is in \(Paths.hostLog.path)."
        } catch {
            status = "Cannot start the game: \(error)"
        }
    }

    func revealLogs() {
        try? FileManager.default.createDirectory(at: Paths.logs, withIntermediateDirectories: true)
        NSWorkspace.shared.open(Paths.logs)
    }
}
