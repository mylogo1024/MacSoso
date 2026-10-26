import SwiftUI

struct SearchSyntaxHelpView: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                basicSearchSection
                booleanSection
                wildcardSection
                regexSection
                extensionFilterSection
                sizeFilterSection
                typeFilterSection
                pathFilterSection
                dateFilterSection
                lengthFilterSection
                modifierSection
                sortSection
                aggregateFilterSection
                typeMacroSection
                structuredPathSection
                contentSearchSection
                tildeSection
            }
            .padding(24)
        }
    }

    // MARK: - Sections

    private var basicSearchSection: some View {
        SyntaxSection(title: "Basic Search") {
            SyntaxRow("hello", "Filename contains (case-insensitive)")
            SyntaxRow("hello world", "AND — both must match")
            SyntaxRow("\"exact phrase\"", "Quoted phrase")
            SyntaxNote("Bare terms match the filename only, as in Everything.")
            SyntaxNote("Add path: or enable Match Path to search the full path.")
        }
    }

    private var booleanSection: some View {
        SyntaxSection(title: "Boolean Operators") {
            SyntaxRow("A | B", "OR — either matches")
            SyntaxRow("A B", "AND — space separates terms")
            SyntaxRow("!A", "NOT — exclude matches")
            SyntaxRow("<A | B> C", "Grouping with angle brackets")
            SyntaxNote("Everything evaluates OR before AND:")
            SyntaxNote("apple banana | orange = apple AND (banana OR orange)")
            SyntaxNote("AND, OR, NOT and ( ) are literal text, not operators.")
        }
    }

    private var wildcardSection: some View {
        SyntaxSection(title: "Wildcards / Glob") {
            SyntaxRow("*", "Zero or more characters (does not cross /)")
            SyntaxRow("?", "Exactly one character")
            SyntaxRow("**", "Any characters including folder separators")
            SyntaxRow("*.txt", "Files ending with .txt")
            SyntaxRow("test*", "Names starting with test")
            SyntaxRow("src/**/*.swift", "Swift files anywhere under src")
        }
    }

    private var regexSection: some View {
        SyntaxSection(title: "Regex") {
            SyntaxRow("regex:pattern", "ECMAScript regex (case-insensitive)")
            SyntaxNote("Example: regex:^test  regex:\\.cpp$")
        }
    }

    private var extensionFilterSection: some View {
        SyntaxSection(title: "Extension Filter") {
            SyntaxRow("ext:cpp", "Files with .cpp extension")
            SyntaxRow("ext:cpp;h;hpp", "Multiple extensions (semicolon-separated)")
            SyntaxRow("ext:cpp,h", "Multiple extensions (comma or pipe also work)")
            SyntaxRow("ext:.cpp", "A leading dot is ignored")
            SyntaxRow("noext:tmp,log", "Exclude these extensions")
        }
    }

    private var sizeFilterSection: some View {
        SyntaxSection(title: "Size Filter") {
            SyntaxRow("size:>1mb", "Larger than 1 MB")
            SyntaxRow("size:<500kb", "Smaller than 500 KB")
            SyntaxRow("size:>=1gb", "At least 1 GB")
            SyntaxRow("size:100kb..1mb", "Between 100 KB and 1 MB")
            SyntaxRow("size:1mb-5mb", "Range (hyphen form)")
            SyntaxRow("fs:>10kb", "fs: is an alias for size:")
            SyntaxNote("Units: b, kb/k, mb/m, gb/g, tb/t")
        }
    }

    private var typeFilterSection: some View {
        SyntaxSection(title: "Type Filter") {
            SyntaxRow("file:", "Match only files")
            SyntaxRow("folder:", "Match only directories")
            SyntaxRow("type:file", "Same as file:")
            SyntaxRow("type:folder", "Same as folder:")
        }
    }

    private var pathFilterSection: some View {
        SyntaxSection(title: "Path Filters") {
            SyntaxRow("path:keyword", "Full path and filename contain keyword")
            SyntaxRow("location:keyword", "Parent folder path contains keyword")
            SyntaxRow("full-path:keyword", "Full path and filename (same as path:)")
            SyntaxRow("parent:/usr/local", "Exact parent folder (absolute path)")
            SyntaxRow("parent-name:local", "Parent folder name contains")
            SyntaxRow("ancestor:/usr", "Below this folder (subfolders included)")
            SyntaxRow("ancestor-name:local", "Any ancestor folder name contains")
            SyntaxRow("nopath:keyword", "Full path does not contain keyword")
            SyntaxRow("depth:<3", "Directory depth less than 3")
        }
    }

    private var dateFilterSection: some View {
        SyntaxSection(title: "Date Filter") {
            SyntaxNote("Prefixes: dm: (modified)  dc: (created)  da: (accessed)")
            SyntaxRow("dm:today", "Modified today")
            SyntaxRow("dm:yesterday", "Modified yesterday")
            SyntaxRow("dm:thisweek", "Modified this week")
            SyntaxRow("dm:lastmonth", "Modified last month")
            SyntaxRow("dm:last7days", "Modified in the last 7 days (rolling)")
            SyntaxRow("dm:last3months", "Modified in the last 3 months")
            SyntaxRow("dm:2024-01-15", "Modified on specific date")
            SyntaxRow("dm:>2024-01", "Modified after January 2024")
            SyntaxRow("dm:2024-01..2024-06", "Modified between Jan and Jun 2024")
            SyntaxRow("dm-month:december", "Modified in December (any year)")
            SyntaxRow("dm-hour:9..17", "Modified during working hours")
            SyntaxNote("Also: datemodified:  datecreated:  dateaccessed:")
        }
    }

    private var lengthFilterSection: some View {
        SyntaxSection(title: "Length Filters") {
            SyntaxRow("len:>10", "Filename longer than 10 chars")
            SyntaxRow("name-length:<5", "Filename shorter than 5 chars")
            SyntaxRow("full-path-length:>100", "Full path longer than 100 chars")
        }
    }

    private var modifierSection: some View {
        SyntaxSection(title: "Match Modifiers") {
            SyntaxRow("case:term", "Force case-sensitive match")
            SyntaxRow("nocase:term", "Force case-insensitive match")
            SyntaxRow("ww:hello", "Whole word match")
            SyntaxRow("wfn:readme", "Whole filename match")
            SyntaxRow("fn:readme", "Whole filename (Everything form)")
            SyntaxRow("fu:/usr/bin/ls", "Whole full-path match")
            SyntaxRow("prefix:test", "Filename starts with text")
            SyntaxRow("suffix:test", "Filename ends with text")
            SyntaxRow("ignore-punctuation:foo", "Ignore punctuation when matching")
            SyntaxRow("ignore-whitespace:foo", "Ignore whitespace when matching")
            SyntaxRow("no-wildcards:*", "Treat * and ? literally")
            SyntaxRow("path:term", "Match this term against the full path")
            SyntaxRow("?path:term", "Global: match every term against the path")
            SyntaxRow("case:regex:^test_", "Modifiers stack: case + regex")
            SyntaxNote("Search ▸ Match Path (⇧⌘P) toggles ?path: for the whole query.")
            SyntaxNote("Aliases: wholeword:  wholefilename:  start-with:  end-with:")
        }
    }

    private var sortSection: some View {
        SyntaxSection(title: "Sorting (sort:)") {
            SyntaxRow("sort:name", "Sort by name, ascending")
            SyntaxRow("sort:size", "Sort by size, ascending")
            SyntaxRow("sort:-size", "Descending (leading minus)")
            SyntaxRow("sort:size-descending", "Descending (suffix form)")
            SyntaxRow("sort:dm:descending", "Newest modified first")
            SyntaxRow("sort:size;name", "Multi-column: size, then name")
            SyntaxRow("sort:name;size-descending", "Per-column direction")
            SyntaxRow("folders-first:", "Folders before files, then sort order")
            SyntaxNote("Fields: name, path, size, ext, type, dm, dc, da, depth, len")
            SyntaxNote("Separate columns with ';' — any number of columns is supported.")
            SyntaxNote("Also available from the Sort By menu (⌘⌥ + key).")
        }
    }

    private var aggregateFilterSection: some View {
        SyntaxSection(title: "Duplicate & Folder Filters") {
            SyntaxRow("empty:", "Folders with no children")
            SyntaxRow("childcount:0", "Folders with no children (numeric test)")
            SyntaxRow("child-file-count:>5", "Folders with more than 5 files")
            SyntaxRow("child:readme", "Folders containing readme")
            SyntaxRow("child-file:*.txt", "Folders containing a matching file")
            SyntaxRow("child-folder:src", "Folders containing a matching folder")
            SyntaxRow("dupe:", "Files duplicated by name (default property)")
            SyntaxRow("dupe:name;size", "Duplicates by the listed properties")
            SyntaxRow("dupe-min:3", "Duplicate groups with at least 3 items")
            SyntaxRow("dupe-max:10", "Duplicate groups with at most 10 items")
            SyntaxRow("dupe-count:2", "Show at most 2 items per duplicate group")
            SyntaxRow("dupe-from:folder", "Groups with an item in folder (subfolders too)")
            SyntaxNote("empty: matches empty folders, not zero-byte files.")
            SyntaxNote("These scan the whole index, so they take longer.")
        }
    }

    private var typeMacroSection: some View {
        SyntaxSection(title: "File Type Macros") {
            SyntaxRow("audio:", "mp3, wav, flac, aac, ogg, m4a, wma, alac")
            SyntaxRow("video:", "mp4, avi, mkv, mov, wmv, flv, webm, m4v")
            SyntaxRow("pic:", "jpg, png, gif, bmp, tiff, webp, svg, heic ...")
            SyntaxRow("doc:", "pdf, doc, docx, xls, xlsx, ppt, pptx, txt, md ...")
            SyntaxRow("exe:", "app, dmg, pkg, sh, command")
            SyntaxRow("zip:", "zip, rar, 7z, tar, gz, bz2, xz, tgz, zst, lz4")
        }
    }

    private var structuredPathSection: some View {
        SyntaxSection(title: "Path Terms") {
            SyntaxRow("/abc/def", "Full path contains /abc/def")
            SyntaxRow("/abc/def/", "Items inside the def folder")
            SyntaxRow("/abc/**/def", "** crosses folder separators")
            SyntaxRow("\\Users\\local", "Backslashes are normalised to /")
            SyntaxNote("Any term containing / is matched against the full path.")
            SyntaxNote("A trailing / restricts the match to items inside the folder.")
        }
    }

    private var contentSearchSection: some View {
        SyntaxSection(title: "Content Search") {
            SyntaxRow("content:keyword", "Search file contents for keyword")
        }
    }

    private var tildeSection: some View {
        SyntaxSection(title: "Tilde Expansion") {
            SyntaxRow("~/Downloads", "Expands ~ to your home directory")
            SyntaxRow("~/Downloads/*.txt", "Files directly in Downloads")
            SyntaxRow("~/**/*.txt", "Nested files (** crosses folders)")
            SyntaxNote("Only expanded when ~ is at the start of the query")
        }
    }
}

// MARK: - Helper Components

private struct SyntaxSection<Content: View>: View {
    let title: String
    @ViewBuilder let content: () -> Content

    init(title: String, @ViewBuilder content: @escaping () -> Content) {
        self.title = title
        self.content = content
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
                .font(.headline)
                .padding(.bottom, 2)
            content()
        }
    }
}

private struct SyntaxRow: View {
    let syntax: String
    let description: String

    init(_ syntax: String, _ description: String) {
        self.syntax = syntax
        self.description = description
    }

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Text(syntax)
                .font(.system(.body, design: .monospaced))
                .foregroundColor(.accentColor)
                .frame(width: 180, alignment: .leading)
            Text(description)
                .foregroundColor(.secondary)
            Spacer()
        }
    }
}

private struct SyntaxNote: View {
    let text: String

    init(_ text: String) {
        self.text = text
    }

    var body: some View {
        Text(text)
            .font(.callout)
            .foregroundColor(.secondary)
            .italic()
            .padding(.leading, 4)
    }
}
