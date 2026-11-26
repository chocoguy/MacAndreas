import SwiftUI

struct ContentView: View {
    @ObservedObject var model: LauncherModel
    @State private var aspect = "All"

    var body: some View {
        Form {
            Section("Game") {
                HStack {
                    Text(model.config.gamePath.isEmpty ? "No folder chosen" : model.config.gamePath)
                        .lineLimit(1)
                        .truncationMode(.middle)
                        .foregroundStyle(model.config.gamePath.isEmpty ? .secondary : .primary)
                    Spacer()
                    Button("Choose…") { model.chooseGame() }
                        .disabled(model.converting || model.running)
                }
                if let v = model.verify {
                    Label(v.supported ? "gta_sa.exe \(v.build), verified" : v.message,
                          systemImage: v.supported ? "checkmark.seal.fill" : "xmark.octagon.fill")
                        .foregroundStyle(v.supported ? .green : .red)
                } else if let e = model.verifyError {
                    Label(e, systemImage: "xmark.octagon.fill").foregroundStyle(.red)
                }
            }
            Section("Conversion") {
                switch model.conversion {
                case .upToDate:
                    Label("Converted and up to date", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                case .needed(let why):
                    Text(why).foregroundStyle(.secondary)
                case .unavailable(let why):
                    Text(why).foregroundStyle(.secondary)
                }
                if model.converting || !model.phase.isEmpty {
                    VStack(alignment: .leading, spacing: 4) {
                        if let f = model.fraction {
                            ProgressView(value: f) { Text(model.phase) }
                        } else {
                            ProgressView { Text(model.phase) }
                        }
                        Text(model.lastLine).font(.caption.monospaced()).foregroundStyle(.secondary).lineLimit(1)
                    }
                }
                HStack {
                    Button(model.converting ? "Converting…" : "Convert") { model.convert() }
                        .disabled(!model.canConvert)
                    Text("Takes about 10 minutes and uses every core.").font(.caption).foregroundStyle(.secondary)
                }
            }
            Section("Display") {
                Picker("Display", selection: $model.config.display) {
                    ForEach(model.displays) { d in
                        Text("\(d.name) (\(d.width)×\(d.height))").tag(d.id)
                    }
                }
                Picker("Mode", selection: $model.config.windowMode) {
                    ForEach(WindowMode.allCases) { m in Text(m.title).tag(m) }
                }
                Picker("Aspect ratio", selection: $aspect) {
                    ForEach(aspects, id: \.self) { Text($0) }
                }
                Picker("Resolution", selection: resolutionBinding) {
                    ForEach(filteredResolutions) { r in Text("\(r.width)×\(r.height) (\(r.aspect))").tag(r) }
                }
                Toggle("Skip intro movies", isOn: $model.config.skipIntro)
                Text("A resolution saved in the game's own settings takes precedence.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section {
                HStack {
                    Button("Play") { model.play() }
                        .keyboardShortcut(.defaultAction)
                        .disabled(!model.canPlay)
                    Button("Show Logs") { model.revealLogs() }
                    Spacer()
                }
                if !model.status.isEmpty {
                    Text(model.status).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                }
            }
        }
        .formStyle(.grouped)
        .frame(minWidth: 520, minHeight: 560)
        .onChange(of: model.config) { model.save() }
        .onAppear { model.start() }
    }

    private var allResolutions: [Resolution] { resolutions(for: model.selectedDisplay) }
    private var aspects: [String] { ["All"] + Array(Set(allResolutions.map(\.aspect))).sorted() }
    private var filteredResolutions: [Resolution] {
        var list = aspect == "All" ? allResolutions : allResolutions.filter { $0.aspect == aspect }
        let current = Resolution(width: model.config.width, height: model.config.height)
        if !list.contains(current) { list.append(current) }
        return list
    }
    private var resolutionBinding: Binding<Resolution> {
        Binding(get: { Resolution(width: model.config.width, height: model.config.height) },
                set: { model.config.width = $0.width; model.config.height = $0.height })
    }
}
