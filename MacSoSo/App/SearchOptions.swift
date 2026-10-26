import SwiftUI
import Combine

/// Sortable columns, mirroring the field names voidtools Everything accepts in
/// its `sort:` modifier. The raw value is what gets appended to the query.
enum SortFieldName: String, CaseIterable, Identifiable {
    case name = "name"
    case path = "path"
    case size = "size"
    case ext = "ext"
    case type = "type"
    case dateModified = "dm"
    case dateCreated = "dc"
    case dateAccessed = "da"
    case depth = "depth"

    var id: String { rawValue }

    /// Menu label, matching Everything's column headers.
    var displayName: String {
        switch self {
        case .name: return "Name"
        case .path: return "Path"
        case .size: return "Size"
        case .ext: return "Extension"
        case .type: return "Type"
        case .dateModified: return "Date Modified"
        case .dateCreated: return "Date Created"
        case .dateAccessed: return "Date Accessed"
        case .depth: return "Depth"
        }
    }

    /// Keyboard shortcut letter, matching Everything where possible.
    var shortcutKey: String {
        switch self {
        case .name: return "n"
        case .path: return "p"
        case .size: return "s"
        case .ext: return "e"
        case .type: return "t"
        case .dateModified: return "m"
        case .dateCreated: return "c"
        case .dateAccessed: return "a"
        case .depth: return "d"
        }
    }
}

@MainActor
class SearchOptions: ObservableObject {
    static let shared = SearchOptions()

    @Published var isRegex: Bool = false {
        didSet {
            if isRegex {
                isWholeWord = false
                isMatchFilename = false
            }
        }
    }
    @Published var isCaseSensitive: Bool = false
    @Published var isWholeWord: Bool = false {
        didSet {
            if isWholeWord {
                isRegex = false
                isMatchFilename = false
            }
        }
    }
    @Published var isMatchFilename: Bool = false {
        didSet {
            if isMatchFilename {
                isRegex = false
                isWholeWord = false
                isMatchPath = false
            }
        }
    }
    /// Everything's Search > Match Path option: terms match the full path
    /// instead of just the filename.
    @Published var isMatchPath: Bool = false {
        didSet {
            if isMatchPath {
                isMatchFilename = false
            }
        }
    }

    /// Requested sort order. `nil` keeps the engine's relevance ordering,
    /// which is what Everything shows when no sort: modifier is given.
    @Published var sortField: SortFieldName? = nil
    @Published var sortDescending: Bool = false

    private init() {}

    var hasActiveSort: Bool { sortField != nil }

    /// The `sort:` modifier fragment to append to a query, or "" when the
    /// default relevance ordering is wanted.
    var sortModifier: String {
        guard let field = sortField else { return "" }
        return "sort:" + (sortDescending ? "-" : "") + field.rawValue
    }

    /// Build query string with prefixes for the C++ engine
    func buildQuery(_ keyword: String) -> String {
        let sort = sortModifier

        // An empty search box with a sort chosen lists the whole index in that
        // order — exactly how Everything behaves.
        if keyword.isEmpty {
            return sort
        }

        var prefix = ""
        if isRegex { prefix += "regex:" }
        if isCaseSensitive { prefix += "case:" }
        if isWholeWord { prefix += "ww:" }
        if isMatchFilename { prefix += "wfn:" }
        // `?path:` is a global directive: it retargets every term in the query
        // to the full path, mirroring Everything's Match Path search option.
        if isMatchPath { prefix += "?path:" }
        // The sort modifier is a query-level directive, so it goes at the end.
        if !sort.isEmpty {
            return prefix + keyword + " " + sort
        }
        return prefix + keyword
    }
}

struct SearchOptionBadges: View {
    @ObservedObject var options: SearchOptions

    /// Everything keeps its search-option toggles visible at all times, with
    /// the active state highlighted, instead of hiding inactive options.
    var body: some View {
        HStack(spacing: 2) {
            option("Aa", active: options.isCaseSensitive,
                   help: "Match Case (⇧⌘C)") { options.isCaseSensitive.toggle() }
            option("W", active: options.isWholeWord,
                   help: "Match Whole Word (⇧⌘W)") { options.isWholeWord.toggle() }
            option("P", active: options.isMatchPath,
                   help: "Match Path (⇧⌘P)") { options.isMatchPath.toggle() }
            option(".*", active: options.isRegex,
                   help: "Enable Regex (⌘R)") { options.isRegex.toggle() }
            option("FN", active: options.isMatchFilename,
                   help: "Match Filename (⇧⌘F)") { options.isMatchFilename.toggle() }
        }
    }

    private func option(_ text: String, active: Bool, help: String,
                        action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(text)
                .font(.system(size: 10, weight: .semibold, design: .rounded))
                .foregroundColor(active ? .white : .secondary)
                .frame(width: 24, height: 18)
                .background(
                    RoundedRectangle(cornerRadius: 4)
                        .fill(active ? Color.accentColor : Color.secondary.opacity(0.08))
                )
        }
        .buttonStyle(.plain)
        .help(help)
        .accessibilityLabel(help)
        .accessibilityAddTraits(active ? [.isSelected] : [])
    }
}
