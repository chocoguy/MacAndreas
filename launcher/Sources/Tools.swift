import Foundation

struct VerifyResult: Decodable, Equatable {
    var path: String
    var status: String
    var build: String
    var size: Int
    var sha256: String
    var message: String

    var supported: Bool { status == "supported" }
}

enum ToolError: Error, CustomStringConvertible {
    case missing(String)
    case failed(String)

    var description: String {
        switch self {
        case .missing(let what): return "\(what) is missing from the app bundle"
        case .failed(let text): return text
        }
    }
}

struct ProcessOutput {
    var status: Int32
    var stdout: String
    var stderr: String
}

func runTool(_ url: URL, _ arguments: [String]) throws -> ProcessOutput {
    guard Paths.exists(url) else { throw ToolError.missing(url.lastPathComponent) }
    let process = Process()
    process.executableURL = url
    process.arguments = arguments
    let out = Pipe()
    let err = Pipe()
    process.standardOutput = out
    process.standardError = err
    try process.run()
    let outData = out.fileHandleForReading.readDataToEndOfFile()
    let errData = err.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit()
    return ProcessOutput(status: process.terminationStatus,
                         stdout: String(decoding: outData, as: UTF8.self),
                         stderr: String(decoding: errData, as: UTF8.self))
}

func verifyGame(_ gamePath: String) throws -> VerifyResult {
    let output = try runTool(Paths.tool("sa-verify"), ["--json", gamePath])
    if let data = output.stdout.data(using: .utf8), !output.stdout.isEmpty,
       let result = try? JSONDecoder().decode(VerifyResult.self, from: data) {
        return result
    }
    let text = output.stderr.trimmingCharacters(in: .whitespacesAndNewlines)
    throw ToolError.failed(text.isEmpty ? "sa-verify failed with status \(output.status)" : text)
}

struct ConvertProgress {
    var phase: String
    var fraction: Double?
    var line: String
}

func conversionInputsMissing() -> String? {
    if !Paths.exists(Paths.tool("sa-recomp")) { return "sa-recomp is missing from the app bundle" }
    if !Paths.exists(Paths.intrinsics) { return "guest_intrinsics.bc is missing from the app bundle" }
    if !Paths.exists(Paths.analysis.appendingPathComponent("functions.json")) {
        return "The analysis data is missing from the app bundle. Export it with tools/recomp/ghidra/export.sh and rebuild the launcher."
    }
    return nil
}

func parseConvertLine(_ line: String) -> ConvertProgress {
    if let range = line.range(of: #"compiled (\d+)/(\d+) modules"#, options: .regularExpression) {
        let numbers = line[range].split(whereSeparator: { !$0.isNumber }).compactMap { Double($0) }
        if numbers.count >= 2, numbers[1] > 0 {
            return ConvertProgress(phase: "Compiling", fraction: 0.25 + 0.7 * numbers[0] / numbers[1], line: line)
        }
    }
    if line.contains("traces lifted") { return ConvertProgress(phase: "Compiling", fraction: 0.25, line: line) }
    if line.contains("wrote") && line.contains(".dylib") { return ConvertProgress(phase: "Linked", fraction: 1, line: line) }
    return ConvertProgress(phase: "Lifting", fraction: nil, line: line)
}

func convertGame(exe: String, onProgress: @escaping (ConvertProgress) -> Void) throws {
    if let missing = conversionInputsMissing() { throw ToolError.failed(missing) }
    let fm = FileManager.default
    try fm.createDirectory(at: Paths.build, withIntermediateDirectories: true)
    try fm.createDirectory(at: Paths.logs, withIntermediateDirectories: true)
    try? fm.removeItem(at: Paths.stamp)
    fm.createFile(atPath: Paths.convertLog.path, contents: nil)
    let log = try FileHandle(forWritingTo: Paths.convertLog)
    defer { try? log.close() }
    let process = Process()
    process.executableURL = Paths.tool("sa-recomp")
    process.arguments = ["build", "--exe", exe, "--analysis", Paths.analysis.path, "--manifest", Paths.manifest.path,
                         "--runtime", Paths.intrinsics.path, "--out", Paths.build.path]
    let err = Pipe()
    process.standardError = err
    process.standardOutput = log
    try process.run()
    var pending = Data()
    let handle = err.fileHandleForReading
    while true {
        let chunk = handle.availableData
        if chunk.isEmpty { break }
        log.write(chunk)
        pending.append(chunk)
        while let index = pending.firstIndex(where: { $0 == 0x0a || $0 == 0x0d }) {
            let line = String(decoding: pending[pending.startIndex..<index], as: UTF8.self)
            pending.removeSubrange(pending.startIndex...index)
            if !line.isEmpty { onProgress(parseConvertLine(line)) }
        }
    }
    process.waitUntilExit()
    guard process.terminationStatus == 0, Paths.exists(Paths.nativeLibrary) else {
        throw ToolError.failed("Conversion failed with status \(process.terminationStatus). See \(Paths.convertLog.path).")
    }
    try? fm.removeItem(at: Paths.build.appendingPathComponent("shards"))
}

func hostArguments(_ config: LauncherConfig) -> [String] {
    var args = ["--game", config.gamePath, "--native", Paths.nativeLibrary.path, "--manifest", Paths.manifest.path,
                "--window-mode", config.windowMode.rawValue, "--display", String(config.display)]
    if config.width >= 640 && config.height >= 480 {
        args += ["--resolution", config.resolution]
    }
    if config.skipIntro { args.append("--skip-intro") }
    return args
}

func launchHost(_ config: LauncherConfig, onExit: @escaping (Int32) -> Void) throws -> Process {
    let fm = FileManager.default
    try fm.createDirectory(at: Paths.logs, withIntermediateDirectories: true)
    fm.createFile(atPath: Paths.hostLog.path, contents: nil)
    let log = try FileHandle(forWritingTo: Paths.hostLog)
    let process = Process()
    process.executableURL = Paths.tool("sa-host")
    process.arguments = hostArguments(config)
    process.currentDirectoryURL = URL(fileURLWithPath: config.gamePath)
    process.standardOutput = log
    process.standardError = log
    process.terminationHandler = { p in
        try? log.close()
        onExit(p.terminationStatus)
    }
    try process.run()
    return process
}
