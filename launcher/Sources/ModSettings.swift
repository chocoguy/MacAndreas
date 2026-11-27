import Foundation

struct ModSettingSpec: Identifiable, Equatable {
    enum Kind: Equatable { case bool, int(min: Int, max: Int), number(min: Double, max: Double, step: Double), choice([String]) }

    var key: String
    var kind: Kind
    var defaultValue: String
    var title: String
    var notes: String

    var id: String { key }

    static func format(_ n: Double) -> String {
        let text = String(format: "%.4f", n)
        let trimmed = text.replacingOccurrences(of: "0+$", with: "", options: .regularExpression)
        return trimmed.hasSuffix(".") ? String(trimmed.dropLast()) : trimmed
    }
    var mod: String { String(key.split(separator: ".").first ?? "") }

    func accepts(_ value: String) -> Bool {
        switch kind {
        case .bool: return value == "true" || value == "false"
        case let .int(min, max):
            guard let n = Int(value) else { return false }
            return n >= min && n <= max
        case let .number(min, max, _):
            guard let n = Double(value), n.isFinite else { return false }
            return n >= min && n <= max
        case let .choice(choices): return choices.contains(value)
        }
    }
}

enum ModSettingsSchema {
    static func parse(_ text: String) -> [ModSettingSpec] {
        var specs: [ModSettingSpec] = []
        var fields: [String: String] = [:]
        func flush() {
            defer { fields = [:] }
            guard let key = fields["key"], let type = fields["type"], let def = fields["default"],
                  let title = fields["title"] else { return }
            let kind: ModSettingSpec.Kind
            switch type {
            case "bool": kind = .bool
            case "int": kind = .int(min: Int(fields["min"] ?? "") ?? Int.min, max: Int(fields["max"] ?? "") ?? Int.max)
            case "number":
                kind = .number(min: Double(fields["min"] ?? "") ?? -1e300, max: Double(fields["max"] ?? "") ?? 1e300,
                               step: Double(fields["step"] ?? "") ?? 1)
            case "choice":
                let list = (fields["choices"] ?? "").trimmingCharacters(in: CharacterSet(charactersIn: "[] "))
                kind = .choice(list.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) })
            default: return
            }
            specs.append(ModSettingSpec(key: key, kind: kind, defaultValue: def, title: title, notes: fields["notes"] ?? ""))
        }
        for raw in text.components(separatedBy: .newlines) {
            var line = raw
            if line.trimmingCharacters(in: .whitespaces).hasPrefix("#") { continue }
            if line.hasPrefix("- ") {
                flush()
                line = String(line.dropFirst(2))
            }
            guard let colon = line.firstIndex(of: ":") else { continue }
            let key = line[..<colon].trimmingCharacters(in: .whitespaces)
            var value = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
            if value.hasPrefix("\""), value.hasSuffix("\""), value.count >= 2 {
                value = String(value.dropFirst().dropLast()).replacingOccurrences(of: "\\\"", with: "\"")
            }
            fields[key] = value
        }
        flush()
        return specs
    }

    static func load() -> [ModSettingSpec] {
        guard let text = try? String(contentsOf: Paths.modSettingsSchema, encoding: .utf8) else { return [] }
        return parse(text)
    }
}

struct ModSettingValues: Equatable {
    var values: [String: String] = [:]

    static func parse(_ text: String, specs: [ModSettingSpec]) -> ModSettingValues {
        var out = ModSettingValues()
        for raw in text.components(separatedBy: .newlines) {
            let line = raw.split(separator: "#", maxSplits: 1, omittingEmptySubsequences: false).first.map(String.init) ?? ""
            guard let eq = line.firstIndex(of: "=") else { continue }
            let key = line[..<eq].trimmingCharacters(in: .whitespaces)
            let value = line[line.index(after: eq)...].trimmingCharacters(in: .whitespaces)
            if let spec = specs.first(where: { $0.key == key }), spec.accepts(value) {
                out.values[key] = value
            }
        }
        return out
    }

    static func load(specs: [ModSettingSpec]) -> ModSettingValues {
        guard let text = try? String(contentsOf: Paths.modSettings, encoding: .utf8) else { return ModSettingValues() }
        return parse(text, specs: specs)
    }

    func value(_ spec: ModSettingSpec) -> String { values[spec.key] ?? spec.defaultValue }

    func serialize(specs: [ModSettingSpec]) -> String {
        specs.map { "\($0.key) = \(value($0))\n" }.joined()
    }

    func save(specs: [ModSettingSpec]) {
        try? FileManager.default.createDirectory(at: Paths.support, withIntermediateDirectories: true)
        try? serialize(specs: specs).write(to: Paths.modSettings, atomically: true, encoding: .utf8)
    }
}
