import SwiftUI

@main
struct MacSoSoApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @ObservedObject private var searchOptions = SearchOptions.shared

    var body: some Scene {
        Window("MacSoSo", id: "main") {
            ContentView()
        }
        .windowStyle(.titleBar)
        .defaultSize(width: 900, height: 600)
        .commands {
            CommandMenu("Search") {
                Toggle("Match Case", isOn: $searchOptions.isCaseSensitive)
                    .keyboardShortcut("c", modifiers: [.command, .shift])
                Toggle("Match Whole Word", isOn: $searchOptions.isWholeWord)
                    .keyboardShortcut("w", modifiers: [.command, .shift])
                Toggle("Match Path", isOn: $searchOptions.isMatchPath)
                    .keyboardShortcut("p", modifiers: [.command, .shift])
                Toggle("Enable Regex", isOn: $searchOptions.isRegex)
                    .keyboardShortcut("r", modifiers: [.command])
                Toggle("Match Filename", isOn: $searchOptions.isMatchFilename)
                    .keyboardShortcut("f", modifiers: [.command, .shift])
            }

            // Everything-compatible sort menu. Each entry emits a `sort:`
            // modifier that the C++ engine understands directly.
            CommandMenu("Sort By") {
                ForEach(SortFieldName.allCases) { field in
                    Button(field.displayName) {
                        searchOptions.sortField = field
                        searchOptions.sortDescending = false
                    }
                    .keyboardShortcut(KeyEquivalent(Character(field.shortcutKey)),
                                      modifiers: [.command, .option])
                }

                Divider()

                Toggle("Descending", isOn: $searchOptions.sortDescending)
                    .keyboardShortcut("d", modifiers: [.command, .option, .shift])
                    .disabled(searchOptions.sortField == nil)

                Button("Relevance (Default)") {
                    searchOptions.sortField = nil
                    searchOptions.sortDescending = false
                }
            }

            CommandGroup(replacing: .help) {
                Button("Search Syntax Help") {
                    SearchSyntaxHelpWindowController.shared.showWindow()
                }
                .keyboardShortcut("/", modifiers: [.command, .shift])
            }

            CommandGroup(after: .appSettings) {
                Button("Rebuild Index") {
                    NotificationCenter.default.post(name: .rebuildIndex, object: nil)
                }
                .keyboardShortcut("r", modifiers: [.command, .shift])

                Button("Shortcut Settings...") {
                    ShortcutSettingsWindowController.shared.showWindow()
                }
                .keyboardShortcut(",", modifiers: [.command])

                Button("Content Settings...") {
                    ContentSettingsWindowController.shared.showWindow()
                }

                Divider()

                Menu("MCP Integration") {
                    ForEach(MCPClient.allCases, id: \.self) { client in
                        Toggle(client.displayName, isOn: Binding(
                            get: { MCPConfigManager.isEnabled(for: client) },
                            set: { MCPConfigManager.setEnabled($0, for: client) }
                        ))
                    }
                }
            }
        }
    }
}

extension Notification.Name {
    static let rebuildIndex = Notification.Name("rebuildIndex")
}

class ContentSettingsWindowController {
    static let shared = ContentSettingsWindowController()
    private var window: NSWindow?

    func showWindow() {
        if let existing = window, existing.isVisible {
            existing.makeKeyAndOrderFront(nil)
            return
        }

        let settingsView = ContentSettingsView()
        let hostingController = NSHostingController(rootView: settingsView)
        let win = NSWindow(contentViewController: hostingController)
        win.title = "Content Settings"
        win.styleMask = [.titled, .closable]
        win.center()
        win.makeKeyAndOrderFront(nil)
        window = win
    }
}

class ShortcutSettingsWindowController {
    static let shared = ShortcutSettingsWindowController()
    private var window: NSWindow?

    func showWindow() {
        if let existing = window, existing.isVisible {
            existing.makeKeyAndOrderFront(nil)
            return
        }

        let settingsView = ShortcutSettingsView()
        let hostingController = NSHostingController(rootView: settingsView)
        let win = NSWindow(contentViewController: hostingController)
        win.title = "Shortcut Settings"
        win.styleMask = [.titled, .closable]
        win.center()
        win.makeKeyAndOrderFront(nil)
        window = win
    }
}

class SearchSyntaxHelpWindowController {
    static let shared = SearchSyntaxHelpWindowController()
    private var window: NSWindow?

    func showWindow() {
        if let existing = window, existing.isVisible {
            existing.makeKeyAndOrderFront(nil)
            return
        }

        let helpView = SearchSyntaxHelpView()
        let hostingController = NSHostingController(rootView: helpView)
        let win = NSWindow(contentViewController: hostingController)
        win.title = "Search Syntax Help"
        win.styleMask = [.titled, .closable, .resizable]
        win.setContentSize(NSSize(width: 580, height: 720))
        win.minSize = NSSize(width: 450, height: 400)
        win.center()
        win.makeKeyAndOrderFront(nil)
        window = win
    }
}
