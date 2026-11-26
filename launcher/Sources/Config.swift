import Foundation

enum WindowMode: String, Codable, CaseIterable, Identifiable {
    case windowed
    case borderless
    case fullscreen

    var id: String { rawValue }

    var title: String {
        switch self {
        case .windowed: return "Windowed"
        case .borderless: return "Borderless full screen"
        case .fullscreen: return "Native full screen"
        }
    }
}

struct LauncherConfig: Codable, Equatable {
    var gamePath: String = ""
    var width: Int = 0
    var height: Int = 0
    var windowMode: WindowMode = .borderless
    var display: Int = 0
    var skipIntro: Bool = false

    static func load() -> LauncherConfig {
        guard let data = try? Data(contentsOf: Paths.config),
              let config = try? JSONDecoder().decode(LauncherConfig.self, from: data) else {
            return LauncherConfig()
        }
        return config
    }

    func save() {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        guard let data = try? encoder.encode(self) else { return }
        try? FileManager.default.createDirectory(at: Paths.support, withIntermediateDirectories: true)
        try? data.write(to: Paths.config, options: .atomic)
    }

    var resolution: String { "\(width)x\(height)" }
}
