import CryptoKit
import Foundation

struct Stamp: Codable, Equatable {
    var exeSha256: String
    var analysis: String
    var manifest: String
    var runtime: String
    var recomp: String

    static func current(exeSha256: String) -> Stamp {
        Stamp(exeSha256: exeSha256,
              analysis: digest(directory: Paths.analysis),
              manifest: digest(directory: Paths.manifest),
              runtime: digest(file: Paths.intrinsics),
              recomp: digest(file: Paths.tool("sa-recomp")))
    }

    static func load() -> Stamp? {
        guard let data = try? Data(contentsOf: Paths.stamp) else { return nil }
        return try? JSONDecoder().decode(Stamp.self, from: data)
    }

    func save() throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(self).write(to: Paths.stamp, options: .atomic)
    }

    static func digest(file: URL) -> String {
        guard let handle = try? FileHandle(forReadingFrom: file) else { return "missing" }
        defer { try? handle.close() }
        var hasher = SHA256()
        while let chunk = try? handle.read(upToCount: 1 << 20), !chunk.isEmpty {
            hasher.update(data: chunk)
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    static func digest(directory: URL) -> String {
        let fm = FileManager.default
        guard let walker = fm.enumerator(at: directory, includingPropertiesForKeys: [.isRegularFileKey]) else {
            return "missing"
        }
        var files: [URL] = []
        for case let url as URL in walker {
            if (try? url.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true,
               url.lastPathComponent != ".DS_Store" {
                files.append(url)
            }
        }
        if files.isEmpty { return "missing" }
        let base = directory.standardizedFileURL.path
        var hasher = SHA256()
        for url in files.sorted(by: { $0.path < $1.path }) {
            let relative = String(url.standardizedFileURL.path.dropFirst(base.count))
            hasher.update(data: Data(relative.utf8))
            hasher.update(data: Data(digest(file: url).utf8))
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }
}

enum ConversionState: Equatable {
    case upToDate
    case needed(String)
    case unavailable(String)
}

func conversionState(exeSha256: String) -> ConversionState {
    if let missing = conversionInputsMissing() {
        return Paths.exists(Paths.nativeLibrary) && Stamp.load()?.exeSha256 == exeSha256
            ? .upToDate : .unavailable(missing)
    }
    guard Paths.exists(Paths.nativeLibrary), let stamp = Stamp.load() else {
        return .needed("The game has not been converted yet.")
    }
    let current = Stamp.current(exeSha256: exeSha256)
    if stamp == current { return .upToDate }
    if stamp.exeSha256 != current.exeSha256 { return .needed("The game executable changed.") }
    return .needed("MacAndreas was updated since the last conversion.")
}
