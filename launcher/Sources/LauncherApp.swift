import AppKit
import SwiftUI

@main
@MainActor
enum Main {
    static func main() {
        let args = Array(CommandLine.arguments.dropFirst())
        if args.first == "--headless" {
            exit(Headless.run(Array(args.dropFirst())))
        }
        LauncherApp.main()
    }
}

final class LauncherDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

struct LauncherApp: App {
    @NSApplicationDelegateAdaptor(LauncherDelegate.self) var delegate
    @StateObject private var model = LauncherModel()

    var body: some Scene {
        WindowGroup("MacAndreas") {
            ContentView(model: model)
        }
        .windowResizability(.contentSize)
    }
}
