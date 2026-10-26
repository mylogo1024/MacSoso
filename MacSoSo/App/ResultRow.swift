import SwiftUI
import AppKit

/// Shared icon cache to avoid repeated NSWorkspace.shared.icon(forFile:) calls.
/// - App bundles (type=5): cached by full path (each app has a unique icon)
/// - Regular files (type=1): cached by file extension (same ext → same icon)
/// - Directories/symlinks/other: cached by type (one icon per type)
final class FileIconCache {
    static let shared = FileIconCache()
    private let cache = NSCache<NSString, NSImage>()

    private init() {
        cache.countLimit = 500
    }

    func icon(for item: FileItem) -> NSImage {
        // Detect app bundles by name suffix (handles both type=5 and legacy type=2)
        let isApp = item.name.hasSuffix(".app")

        let key: String
        if isApp {
            // App bundles — unique icon per app
            key = "app:" + item.path + "/" + item.name
        } else if item.type == 1 {
            // Regular file — same icon per extension
            let ext = (item.name as NSString).pathExtension.lowercased()
            key = "ext:" + (ext.isEmpty ? "__no_ext__" : ext)
        } else {
            // Dir, symlink, other — one icon per type
            key = "type:\(item.type)"
        }

        let nsKey = key as NSString
        if let cached = cache.object(forKey: nsKey) {
            return cached
        }

        let fullPath = item.path + "/" + item.name
        let nsImage = NSWorkspace.shared.icon(forFile: fullPath)
        nsImage.size = NSSize(width: 24, height: 24)
        cache.setObject(nsImage, forKey: nsKey)
        return nsImage
    }

    func icon(forPath path: String) -> NSImage {
        let ext = (path as NSString).pathExtension.lowercased()
        let key = "ext:" + (ext.isEmpty ? "__no_ext__" : ext) as NSString
        if let cached = cache.object(forKey: key) {
            return cached
        }
        let nsImage = NSWorkspace.shared.icon(forFile: path)
        nsImage.size = NSSize(width: 24, height: 24)
        cache.setObject(nsImage, forKey: key)
        return nsImage
    }
}

/// Everything-style column formatting.
enum ResultFormatting {
    private static let dateFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateStyle = .short
        formatter.timeStyle = .short
        return formatter
    }()

    /// Everything's default Size column shows the exact byte count with
    /// grouping separators; folders have no size.
    static func size(_ item: FileItem) -> String {
        guard item.type == 1 else { return "" }
        return item.size.formatted(.number.grouping(.automatic))
    }

    static func dateModified(_ item: FileItem) -> String {
        dateFormatter.string(
            from: Date(timeIntervalSince1970: TimeInterval(item.modTime)))
    }
}

/// Row actions shared by the results table and its context menu.
enum ResultActions {
    static func fullPath(_ item: FileItem) -> String {
        item.path + "/" + item.name
    }

    static func open(_ item: FileItem) {
        let url = URL(fileURLWithPath: fullPath(item))
        if item.type == 5 {
            NSWorkspace.shared.openApplication(
                at: url, configuration: NSWorkspace.OpenConfiguration())
        } else if !NSWorkspace.shared.open(url) {
            NSSound.beep()
        }
    }

    static func reveal(_ item: FileItem) {
        NSWorkspace.shared.selectFile(fullPath(item), inFileViewerRootedAtPath: "")
    }

    static func copyFullName(_ item: FileItem) {
        copy([fullPath(item)])
    }

    static func copyFullNames(_ items: [FileItem]) {
        copy(items.map(fullPath))
    }

    private static func copy(_ strings: [String]) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(
            strings.joined(separator: "\n"), forType: .string)
    }
}

// MARK: - Table cells

struct ResultNameCell: View {
    let item: FileItem
    let hints: [HighlightHint]

    var body: some View {
        HStack(spacing: 6) {
            Image(nsImage: FileIconCache.shared.icon(for: item))
                .resizable()
                .aspectRatio(contentMode: .fit)
                .frame(width: 16, height: 16)

            let highlighted = highlightCrossMatches(
                path: item.path, name: item.name, hints: hints,
                nameFont: .system(size: 12), nameColor: .primary,
                pathFont: .system(size: 12), pathColor: .primary)
            highlighted.nameText
                .lineLimit(1)
                .truncationMode(.middle)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .contentShape(Rectangle())
        .help(item.name)
        .onDrag {
            NSItemProvider(object: NSURL(fileURLWithPath: ResultActions.fullPath(item)))
        }
        .accessibilityIdentifier("resultRow")
    }
}

struct ResultPathCell: View {
    let item: FileItem
    let hints: [HighlightHint]

    var body: some View {
        let highlighted = highlightCrossMatches(
            path: item.path, name: item.name, hints: hints,
            nameFont: .system(size: 12), nameColor: .primary,
            pathFont: .system(size: 12), pathColor: .primary)
        highlighted.pathText
            .lineLimit(1)
            .truncationMode(.middle)
            .frame(maxWidth: .infinity, alignment: .leading)
            .help(item.path)
    }
}

struct ResultSizeCell: View {
    let item: FileItem

    var body: some View {
        Text(ResultFormatting.size(item))
            .font(.system(size: 12))
            .monospacedDigit()
            .foregroundColor(.primary)
            .lineLimit(1)
            .frame(maxWidth: .infinity, alignment: .trailing)
    }
}

struct ResultDateCell: View {
    let item: FileItem

    var body: some View {
        Text(ResultFormatting.dateModified(item))
            .font(.system(size: 12))
            .foregroundColor(.primary)
            .lineLimit(1)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}
