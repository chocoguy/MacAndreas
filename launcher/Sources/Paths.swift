import Foundation

enum Paths {
    static var contents: URL { Bundle.main.bundleURL.appendingPathComponent("Contents") }
    static func tool(_ name: String) -> URL { contents.appendingPathComponent("MacOS/\(name)") }
    static func resource(_ path: String) -> URL { contents.appendingPathComponent("Resources/\(path)") }

    static var manifest: URL { resource("manifest") }
    static var analysis: URL { resource("analysis") }
    static var intrinsics: URL { resource("runtime/guest_intrinsics.bc") }
    static var modSettingsSchema: URL { resource("mods/settings.yaml") }

    static var home: URL { FileManager.default.homeDirectoryForCurrentUser }
    static var support: URL { home.appendingPathComponent("Library/Application Support/MacAndreas") }
    static var build: URL { support.appendingPathComponent("build") }
    static var nativeLibrary: URL { build.appendingPathComponent("libgta_sa_native.dylib") }
    static var stamp: URL { build.appendingPathComponent("stamp.json") }
    static var config: URL { support.appendingPathComponent("launcher.json") }
    static var modSettings: URL { support.appendingPathComponent("mods.conf") }
    static var logs: URL { home.appendingPathComponent("Library/Logs/MacAndreas") }
    static var hostLog: URL { logs.appendingPathComponent("sa-host.log") }
    static var convertLog: URL { logs.appendingPathComponent("sa-recomp.log") }

    static func exists(_ url: URL) -> Bool { FileManager.default.fileExists(atPath: url.path) }
}
