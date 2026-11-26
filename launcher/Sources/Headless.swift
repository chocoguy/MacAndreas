import AppKit
import SwiftUI

@MainActor
enum Headless {
    static func run(_ args: [String]) -> Int32 {
        var config = LauncherConfig.load()
        let displays = availableDisplays()
        if config.display >= displays.count { config.display = 0 }
        if config.width == 0 || config.height == 0 {
            let r = defaultResolution(for: displays.first { $0.id == config.display })
            config.width = r.width
            config.height = r.height
        }
        var command = "status"
        var operand = ""
        var i = 0
        while i < args.count {
            let a = args[i]
            if a == "--game", i + 1 < args.count {
                config.gamePath = args[i + 1]
                i += 2
                continue
            }
            if command == "snapshot", operand.isEmpty, i > 0 {
                operand = a
            } else {
                command = a
            }
            i += 1
        }
        if command == "snapshot" {
            return snapshot(config, to: operand)
        }
        if config.gamePath.isEmpty {
            print("no game folder; pass --game DIR")
            return 64
        }
        let verify: VerifyResult
        do {
            verify = try verifyGame(config.gamePath)
        } catch {
            print("verify: \(error)")
            return 1
        }
        print("game: \(verify.path) \(verify.build) \(verify.status)")
        guard verify.supported else {
            print(verify.message)
            return 2
        }
        let state = conversionState(exeSha256: verify.sha256)
        switch command {
        case "status", "verify":
            print("conversion: \(describe(state))")
            print("sa-host \(hostArguments(config).joined(separator: " "))")
            return 0
        case "convert":
            if state == .upToDate {
                print("conversion: up to date")
                return 0
            }
            if case .unavailable(let why) = state {
                print("conversion: \(why)")
                return 1
            }
            var lastPhase = ""
            do {
                try convertGame(exe: verify.path) { p in
                    if p.phase != lastPhase {
                        lastPhase = p.phase
                        print("phase: \(p.phase)")
                    }
                    if let f = p.fraction, p.line.contains("compiled"), Int(f * 1000) % 100 == 0 {
                        print(String(format: "progress: %.0f%%", f * 100))
                    }
                }
                try Stamp.current(exeSha256: verify.sha256).save()
            } catch {
                print("convert: \(error)")
                return 1
            }
            print("conversion: \(describe(conversionState(exeSha256: verify.sha256)))")
            return 0
        default:
            print("usage: MacAndreas --headless [--game DIR] status|convert")
            return 64
        }
    }

    static func snapshot(_ config: LauncherConfig, to path: String) -> Int32 {
        guard !path.isEmpty else {
            print("usage: MacAndreas --headless [--game DIR] snapshot FILE.png")
            return 64
        }
        let model = LauncherModel(config: config)
        if !config.gamePath.isEmpty, let v = try? verifyGame(config.gamePath) {
            model.verify = v
            model.conversion = v.supported ? conversionState(exeSha256: v.sha256) : .unavailable(v.message)
        }
        _ = NSApplication.shared
        let size = NSSize(width: 560, height: 680)
        let window = NSWindow(contentRect: NSRect(origin: .zero, size: size), styleMask: [.titled],
                              backing: .buffered, defer: false)
        window.appearance = NSAppearance(named: .aqua)
        let host = NSHostingView(rootView: ContentView(model: model))
        host.frame = NSRect(origin: .zero, size: size)
        window.contentView = host
        host.layoutSubtreeIfNeeded()
        RunLoop.main.run(until: Date().addingTimeInterval(0.3))
        guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else {
            print("snapshot: rendering failed")
            return 1
        }
        host.cacheDisplay(in: host.bounds, to: rep)
        guard let png = rep.representation(using: .png, properties: [:]) else {
            print("snapshot: encoding failed")
            return 1
        }
        do {
            try png.write(to: URL(fileURLWithPath: path))
        } catch {
            print("snapshot: \(error)")
            return 1
        }
        print("snapshot: \(path)")
        return 0
    }

    static func describe(_ state: ConversionState) -> String {
        switch state {
        case .upToDate: return "up to date"
        case .needed(let why): return "needed: \(why)"
        case .unavailable(let why): return "unavailable: \(why)"
        }
    }
}
