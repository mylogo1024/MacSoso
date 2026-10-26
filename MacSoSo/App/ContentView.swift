import SwiftUI

struct ContentView: View {
    @StateObject private var viewModel = SearchViewModel()
    @ObservedObject private var searchOptions = SearchOptions.shared
    @State private var selection = Set<FileItem.ID>()
    @State private var sortOrder: [KeyPathComparator<FileItem>] = []
    @FocusState private var isSearchFieldFocused: Bool

    var body: some View {
        VStack(spacing: 0) {
            // Permission banner
            PermissionView()

            searchBar

            Divider()

            resultsArea

            Divider()

            statusBar
        }
        .frame(minWidth: 760, minHeight: 400)
        .onChange(of: sortOrder) { _, newOrder in
            applySortOrder(newOrder)
        }
        .onChange(of: searchOptions.sortField) { _, _ in
            syncSortOrderFromOptions()
        }
        .onChange(of: searchOptions.sortDescending) { _, _ in
            syncSortOrderFromOptions()
        }
        .onChange(of: viewModel.searchText) { _, _ in
            selection.removeAll()
        }
        .onReceive(NotificationCenter.default.publisher(for: .rebuildIndex)) { _ in
            viewModel.rebuildIndex()
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            viewModel.onWindowFocusChanged(true)
            isSearchFieldFocused = true
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didResignActiveNotification)) { _ in
            viewModel.onWindowFocusChanged(false)
        }
        .onReceive(NotificationCenter.default.publisher(for: NSWindow.didDeminiaturizeNotification)) { _ in
            selection.removeAll()
        }
    }

    // MARK: - Search bar

    /// Everything-style flat toolbar: search field plus always-visible option
    /// toggles. Active options are highlighted like Everything's toolbar.
    private var searchBar: some View {
        HStack(spacing: 8) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 12, weight: .medium))
                .foregroundColor(.secondary)

            HighlightedSearchField(
                text: $viewModel.searchText,
                placeholder: "Search Everything",
                ghostSuggestion: viewModel.ghostSuggestion,
                isFocused: $isSearchFieldFocused,
                onTab: {
                    if viewModel.ghostSuggestion != nil {
                        viewModel.acceptGhostSuggestion()
                        return true
                    }
                    return false
                }
            )
            .frame(height: 22)
            .onChange(of: viewModel.searchText) {
                viewModel.onSearchTextChanged()
            }

            SearchOptionBadges(options: searchOptions)

            if !viewModel.searchText.isEmpty {
                Button {
                    viewModel.searchText = ""
                    // .onChange(of: searchText) triggers onSearchTextChanged()
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 12))
                        .foregroundColor(.secondary)
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("clearButton")
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(.bar)
    }

    // MARK: - Results

    @ViewBuilder
    private var resultsArea: some View {
        // Keep presenting cached/partial results while the background scan
        // catches up — Everything never hides the list behind a spinner.
        if viewModel.isScanning && viewModel.displayItems.isEmpty {
            VStack(spacing: 12) {
                Spacer()
                ProgressView()
                    .controlSize(.large)
                Text("Indexing files... \(viewModel.scannedCount) items scanned")
                    .font(.callout)
                    .foregroundColor(.secondary)
                Spacer()
            }
        } else if viewModel.isContentSearch {
            contentResultsView
        } else if viewModel.displayItems.isEmpty && !viewModel.searchText.isEmpty && viewModel.scanComplete {
            VStack(spacing: 8) {
                Spacer()
                Text("No results found")
                    .foregroundColor(.secondary)
                    .accessibilityIdentifier("noResultsLabel")
                Spacer()
            }
        } else {
            resultsTable
        }
    }

    /// Everything's default column set: Name, Path, Size, Date Modified.
    /// Clicking a header re-runs the query with the matching `sort:` modifier.
    private var resultsTable: some View {
        Table(viewModel.displayItems, selection: $selection, sortOrder: $sortOrder) {
            TableColumn("Name", value: \.nameSortKey) { item in
                ResultNameCell(item: item, hints: viewModel.highlightHints)
            }
            .width(min: 160, ideal: 260)

            TableColumn("Path", value: \.pathSortKey) { item in
                ResultPathCell(item: item, hints: viewModel.highlightHints)
            }
            .width(min: 160, ideal: 340)

            TableColumn("Size", value: \.size) { item in
                ResultSizeCell(item: item)
            }
            .width(min: 70, ideal: 90, max: 160)

            TableColumn("Date Modified", value: \.modTime) { item in
                ResultDateCell(item: item)
            }
            .width(min: 120, ideal: 150)
        }
        .tableStyle(.inset(alternatesRowBackgrounds: false))
        .contextMenu(forSelectionType: FileItem.ID.self) { ids in
            resultMenu(for: ids)
        } primaryAction: { ids in
            if let item = singleItem(in: ids) {
                ResultActions.open(item)
            }
        }
        .accessibilityIdentifier("fileResultsList")
    }

    private var contentResultsView: some View {
        Group {
            if viewModel.contentResults.isEmpty && !viewModel.contentKeyword.isEmpty && viewModel.scanComplete {
                VStack(spacing: 8) {
                    Spacer()
                    if viewModel.isContentIndexing {
                        ProgressView()
                            .controlSize(.large)
                            .padding(.bottom, 4)
                        Text("Content index is building...")
                            .font(.headline)
                            .foregroundColor(.orange)
                        if let progress = viewModel.contentIndexProgress {
                            Text("Indexed \(progress.indexed) / \(progress.total) files")
                                .font(.subheadline)
                                .foregroundColor(.secondary)
                        }
                        Text("Search results will appear after indexing completes")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    } else {
                        Image(systemName: "doc.text.magnifyingglass")
                            .font(.system(size: 36))
                            .foregroundColor(.secondary.opacity(0.5))
                            .padding(.bottom, 4)
                        Text("No content matches found")
                            .foregroundColor(.secondary)
                        if viewModel.contentIndexedCount == 0 {
                            Text("No files indexed. Configure extensions in Content Settings.")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                    }
                    Spacer()
                }
            } else if viewModel.contentKeyword.isEmpty {
                VStack {
                    Spacer()
                    Text("Type a keyword after infile: to search file contents")
                        .foregroundColor(.secondary)
                    Spacer()
                }
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(viewModel.contentResults) { item in
                            ContentResultRow(item: item, keyword: viewModel.contentKeyword)
                                .padding(.horizontal, 8)
                                .padding(.vertical, 2)
                        }
                    }
                }
                .accessibilityIdentifier("contentResultsList")
            }
        }
    }

    // MARK: - Status bar

    /// Everything's status bar lives at the bottom and leads with the object
    /// count; indexing and live-update state sit on the right.
    private var statusBar: some View {
        HStack(spacing: 6) {
            if viewModel.isScanning {
                ProgressView()
                    .controlSize(.small)
                Text("Scanning... \(viewModel.scannedCount) items")
            } else if viewModel.scanComplete {
                if viewModel.isContentSearch {
                    Text("\(viewModel.contentResults.count) content matches")
                } else {
                    Text("\(viewModel.totalMatches) objects")
                        .accessibilityIdentifier("matchCount")
                    if !selection.isEmpty {
                        Text("·")
                        Text("\(selection.count) selected")
                    }
                }
            }

            Spacer()

            if viewModel.isContentIndexing, let progress = viewModel.contentIndexProgress {
                ProgressView()
                    .controlSize(.small)
                Text("Content indexing \(progress.indexed)/\(progress.total)")
                    .foregroundColor(.orange)
            }
            if viewModel.isSyncing {
                ProgressView()
                    .controlSize(.small)
                Text("Syncing...")
                    .foregroundColor(.orange)
            } else if viewModel.isMonitoring {
                Circle()
                    .fill(.green)
                    .frame(width: 6, height: 6)
                Text("Live")
                    .foregroundColor(.green)
                    .fontWeight(.medium)
            }
            if viewModel.scanComplete {
                Text("\(viewModel.totalRecords) files indexed")
                    .accessibilityIdentifier("indexedCount")
            }
            if viewModel.queryTimeMs > 0 && !viewModel.isScanning {
                Text(String(format: "%.1f ms", viewModel.queryTimeMs))
            }
        }
        .font(.system(size: 11))
        .foregroundColor(.secondary)
        .padding(.horizontal, 10)
        .padding(.vertical, 4)
        .background(Color(nsColor: .controlBackgroundColor))
        .accessibilityIdentifier("statusBar")
    }

    // MARK: - Result actions

    @ViewBuilder
    private func resultMenu(for ids: Set<FileItem.ID>) -> some View {
        let items = viewModel.displayItems.filter { ids.contains($0.id) }
        if items.count == 1, let item = items.first {
            Button("Open") { ResultActions.open(item) }
            Button("Open Path") { ResultActions.reveal(item) }
            Divider()
            Button("Copy Full Name") { ResultActions.copyFullName(item) }
        } else if items.count > 1 {
            Button("Copy Full Names") { ResultActions.copyFullNames(items) }
        }
    }

    private func singleItem(in ids: Set<FileItem.ID>) -> FileItem? {
        guard ids.count == 1, let id = ids.first else { return nil }
        return viewModel.displayItems.first { $0.id == id }
    }

    // MARK: - Column sort <-> sort: modifier

    private func applySortOrder(_ order: [KeyPathComparator<FileItem>]) {
        // An empty order means the menu chose a field without a column (or
        // cleared sorting); leave SearchOptions as-is in that case.
        guard let first = order.first, let field = sortField(for: first.keyPath) else {
            return
        }
        let descending = first.order == .reverse
        if searchOptions.sortField != field ||
            searchOptions.sortDescending != descending {
            searchOptions.sortField = field
            searchOptions.sortDescending = descending
        }
    }

    private func syncSortOrderFromOptions() {
        guard let field = searchOptions.sortField else {
            if !sortOrder.isEmpty { sortOrder = [] }
            return
        }

        let order: SortOrder = searchOptions.sortDescending ? .reverse : .forward
        let newOrder: [KeyPathComparator<FileItem>]
        switch field {
        case .name:
            newOrder = [KeyPathComparator(\FileItem.nameSortKey, order: order)]
        case .path:
            newOrder = [KeyPathComparator(\FileItem.pathSortKey, order: order)]
        case .size:
            newOrder = [KeyPathComparator(\FileItem.size, order: order)]
        case .dateModified:
            newOrder = [KeyPathComparator(\FileItem.modTime, order: order)]
        default:
            newOrder = []
        }

        if let current = sortOrder.first,
           sortField(for: current.keyPath) == field,
           (current.order == .reverse) == searchOptions.sortDescending {
            return
        }
        sortOrder = newOrder
    }

    private func sortField(for keyPath: PartialKeyPath<FileItem>) -> SortFieldName? {
        switch keyPath {
        case \FileItem.nameSortKey: return .name
        case \FileItem.pathSortKey: return .path
        case \FileItem.size: return .size
        case \FileItem.modTime: return .dateModified
        default: return nil
        }
    }
}
