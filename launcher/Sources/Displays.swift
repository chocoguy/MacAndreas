import AppKit

struct DisplayChoice: Identifiable, Equatable {
    var id: Int
    var name: String
    var width: Int
    var height: Int
}

struct Resolution: Identifiable, Hashable {
    var width: Int
    var height: Int
    var id: String { "\(width)x\(height)" }
    var aspect: String { aspectName(width, height) }
}

func aspectName(_ width: Int, _ height: Int) -> String {
    guard height > 0 else { return "Other" }
    let ratio = Double(width) / Double(height)
    let known: [(String, Double)] = [("4:3", 4.0 / 3), ("5:4", 1.25), ("16:10", 1.6), ("16:9", 16.0 / 9),
                                     ("21:9", 64.0 / 27), ("32:9", 32.0 / 9)]
    for (name, value) in known where abs(ratio - value) < 0.02 { return name }
    return "Other"
}

@MainActor
func availableDisplays() -> [DisplayChoice] {
    NSScreen.screens.enumerated().map { index, screen in
        var frame = screen.frame
        let inset = screen.safeAreaInsets
        frame.size.width -= inset.left + inset.right
        frame.size.height -= inset.top + inset.bottom
        let scale = screen.backingScaleFactor
        return DisplayChoice(id: index, name: screen.localizedName,
                             width: Int((frame.width * scale).rounded()), height: Int((frame.height * scale).rounded()))
    }
}

let standardResolutions: [Resolution] = [
    (800, 600), (1024, 768), (1280, 720), (1280, 800), (1280, 960), (1366, 768), (1440, 900), (1600, 900),
    (1680, 1050), (1920, 1080), (1920, 1200), (2560, 1080), (2560, 1440), (2560, 1600), (2880, 1800),
    (3440, 1440), (3840, 2160), (5120, 2880),
].map { Resolution(width: $0.0, height: $0.1) }

func resolutions(for display: DisplayChoice?) -> [Resolution] {
    guard let display else { return standardResolutions }
    var list = standardResolutions.filter { $0.width <= display.width && $0.height <= display.height }
    let native = Resolution(width: display.width, height: display.height)
    if !list.contains(native) { list.append(native) }
    return list.sorted { ($0.width, $0.height) < ($1.width, $1.height) }
}

func defaultResolution(for display: DisplayChoice?) -> Resolution {
    guard let display else { return Resolution(width: 1920, height: 1080) }
    let aspect = aspectName(display.width, display.height)
    let candidates = resolutions(for: display).filter { $0.aspect == aspect && $0.height <= 1440 }
    return candidates.last ?? Resolution(width: min(1920, display.width), height: min(1080, display.height))
}
