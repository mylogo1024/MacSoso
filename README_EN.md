<p align="center">
  <img src="MacSoSo/Assets.xcassets/AppIcon.appiconset/icon_256.png" alt="MacSoSo" width="128" />
</p>

<h1 align="center">MacSoSo</h1>

<p align="center">
  <b>Instant file search for macOS</b> — find any file among millions in milliseconds.<br/>
  Inspired by <a href="https://www.voidtools.com/">Everything</a> on Windows. Nothing else comes close on Mac.
</p>

<p align="center">
  <a href="README.md">中文</a> | <b>English</b>
</p>

<p align="center">
  <a href="#installation"><img src="https://img.shields.io/badge/macOS-14%2B-blue?logo=apple" alt="macOS 14+" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License" /></a>
  <a href="#testing"><img src="https://img.shields.io/badge/tests-79%20modules-brightgreen" alt="79 test modules" /></a>
  <a href="#ai-tool-integration-mcp"><img src="https://img.shields.io/badge/MCP-compatible-blueviolet" alt="MCP Compatible" /></a>
</p>

---

<p align="center">
  <img src="assets/screen-shot.png" alt="MacSoSo Screenshot" width="720" /><br/>
  <sub>Screenshot from the pre-rename build; the current window title is MacSoSo.</sub>
</p>

## Relationship to Upstream

> **Rename note:** This project is now distributed as **MacSoSo** (renamed from
> MacEverything in October 2026). The upstream project is still named
> MacEverything; the attribution and links below are unchanged.

This project is a **modified derivative of
[joshua-wu/MacEverything](https://github.com/joshua-wu/MacEverything)**
(baseline commit `26f0332`, MIT licensed). The upstream project provided the
engine and app foundation; this fork adds Everything-compatible search
semantics, an Everything-style UI, external-volume indexing, and release
packaging.

- Detailed changes: [docs/CHANGES_FROM_UPSTREAM.md](docs/CHANGES_FROM_UPSTREAM.md)
- Attribution and copyright: [NOTICE](NOTICE)
- This fork is not affiliated with voidtools. "Everything" is used only to
  describe search-syntax compatibility.

## Feature Highlights

### Blazing Fast Search

Index an entire disk — **5 million+ files in just 14 seconds** — then get results in **under 5ms** for every search. Two orders of magnitude faster than Spotlight.

| Comparison | MacSoSo | Spotlight | `find` |
|------------|:---:|:---:|:---:|
| Index 5M files | ~14s | Minutes+ | No index |
| Search latency | **< 5ms** | 200ms–2s | 5–30s |
| Real-time monitoring | FSEvents | FSEvents | None |
| Content search | Trigram index | Metadata-heavy | `grep` |
| AI tool integration | Built-in MCP | No | No |

### Always at Your Fingertips

Press **`Option+Space`** to summon the search window anytime (hotkey is customizable). The search bar auto-focuses — start typing immediately, find what you need, and move on. Supports launch-at-login with minimized background mode, staying out of your workflow.

### Smart Input Experience

- **Ghost text autocomplete** — semi-transparent suggestions appear as you type, drawn from search history (sorted by frequency) or system keywords (e.g., typing `ex` suggests `ext:`). Press **Tab** to accept
- **Search bar syntax highlighting** — real-time color coding: filter names in purple, arguments in blue, quoted strings in orange, operators in red
- **Persistent search options** — `Aa` / `W` / `P` / `.*` / `FN` toggles always sit at the right of the search bar and highlight when active, switching Match Case / Whole Word / Match Path / Regex / Match Filename

### Everything-Style Interface

The main window mirrors Everything's layout and interaction model:

- **Four-column table** — Name / Path / Size / Date Modified with compact rows; clicking a header toggles ascending/descending and round-trips to the `sort:` query modifier, so the engine sorts before truncation
- **Flat toolbar** — just the search field and persistent option toggles on top, with no pagination footer or extra banners; the result table is resizable and virtualized
- **Bottom status bar** — result object count and selection on the left; content-index progress, live-monitoring state, total indexed files, and query time on the right
- **Row actions** — double-click to open; context menu with Open / Open Path / Copy Full Name; drag rows straight into other apps

### Everything-Style Query Syntax

Full AST parser matching voidtools Everything semantics: bare terms match the filename, terms containing `/` match the full path, `|` is OR (evaluated before AND), `< >` groups, and `!` negates. Built-in syntax help window (**Cmd+?**).

| Query | Description |
|-------|-------------|
| `readme` | Filename contains "readme" |
| `*.swift` | All Swift source files (`*` does not cross `/`) |
| `src/**/*.swift` | Swift files at any depth (`**` crosses folders) |
| `ext:py size:>1mb` | Python files larger than 1MB |
| `dm:today` | Files modified today |
| `config path:/usr` | Full path contains `/usr`, name contains "config" |
| `"exact phrase"` | Exact phrase matching |
| `foo \| bar` | Boolean OR (OR binds before AND) |
| `<foo \| bar> baz` | Angle-bracket grouping |
| `!foo` | Boolean NOT |
| `location:Downloads` | Items inside the Downloads folder |
| `parent:/Users/me` | Exact parent folder |
| `case:Makefile` | Case-sensitive search |
| `case:regex:^test_` | Stackable modifiers |
| `regex:^test_.*\.py$` | Regular expression search |
| `type:folder node_modules` | Search directories only |
| `~/Documents/*.pdf` | Tilde expansion + glob (direct children) |
| `infile:TODO ext:cpp` | Search "TODO" inside C++ files |
| `sort:size;name` | Multi-column sort |

> `AND` / `OR` / `NOT` keywords and parentheses are literal text in Everything — use space, `|`, `!` and `< >`.

<details>
<summary><b>Full filter list</b></summary>

| Filter | Description | Example |
|--------|-------------|---------|
| `ext:` | File extension | `ext:swift,h` |
| `noext:` | Exclude extensions | `noext:tmp,log` |
| `size:` / `fs:` | File size | `size:>1mb`, `size:100kb-5mb` |
| `type:` | File/folder/media type | `type:folder`, `type:image` |
| `path:` | Full path (incl. name) contains | `path:Downloads` |
| `location:` / `loc:` | Parent folder path contains | `location:Downloads` |
| `full-path:` | Full path and filename | `full-path:/usr/local` |
| `nopath:` | Full path excludes | `nopath:node_modules` |
| `parent:` | Exact parent folder (absolute) | `parent:/Users/me/src` |
| `parent-name:` | Parent folder name contains | `parent-name:local` |
| `ancestor:` | Below the given folder | `ancestor:/Users/me/src` |
| `ancestor-name:` | Any ancestor folder name | `ancestor-name:node_modules` |
| `depth:` | Directory depth | `depth:<3` |
| `len:` / `name-length:` | Filename length | `len:>50` |
| `full-path-length:` | Full path length | `fullpathlen:>100` |
| `dm:` | Date modified | `dm:today`, `dm:>2024-01-01` |
| `dc:` | Date created | `dc:thisweek` |
| `da:` | Date accessed | `da:last7days` |
| `fn:` | Whole filename | `fn:Makefile` |
| `fu:` | Whole full path | `fu:/usr/bin/ls` |
| `case:` | Case-sensitive | `case:README` |
| `regex:` | Regular expression | `regex:^test_` |
| `ww:` | Whole word match | `ww:test` |
| `wfn:` | Whole filename match | `wfn:Makefile` |
| `empty:` | Folders with no children | `empty:` |
| `childcount:` | Child count | `childcount:0` |
| `child:` | Contains a matching child | `child:readme` |
| `dupe:` | Duplicates by property | `dupe:name;size` |
| `dupe-min:` / `dupe-max:` | Duplicate group size bounds | `dupe-min:3` |
| `dupe-count:` | Max items shown per group | `dupe:size dupe-count:2` |
| `dupe-from:` | Groups with an item in the folder (subfolders included) | `dupe:size dupe-from:~/Photos` |
| `content:` / `infile:` | Content search | `infile:TODO` |
| `sort:` / `sorted:` | Result sorting | `sort:size-descending` |
| `audio:` `video:` `pic:` `doc:` `zip:` | File type macros | `audio:` = all audio files |

**Sorting** — field names match Everything's column names; separate columns with `;`:

| Syntax | Meaning |
|--------|---------|
| `sort:name` | Name ascending |
| `sort:-size` | Size descending (leading minus) |
| `sort:size-descending` | Size descending (suffix form) |
| `sort:dm:descending` | Newest modified first |
| `sort:size;name` | Size, then name |
| `sort:name;size-descending` | Per-column directions |
| `folders-first:` | Folders before files (applied before truncation) |

</details>

### Full-Text Content Search

Type `infile:keyword` to search file contents — results include highlighted context snippets around the keyword. Powered by a Trigram index for speed, only re-indexing changed files. Configure indexed file types and max file size in Content Settings.

### Real-Time Sync, Always Up to Date

- **File monitoring** — FSEvents-based real-time file system listener; new, renamed, or deleted files appear in search results immediately
- **Two-phase instant startup** — loads disk cache on launch (searchable immediately), then catches up with changes via FSEvents in the background. Zero wait time
- **Focus-aware power saving** — pauses refresh when the window is in the background, batch-catches up on refocus. Near-zero background CPU usage

### Interaction Details

- **Smart highlighting** — matches highlighted in search results, AST-aware: correctly handles glob wildcards, regex, case sensitivity, NOT exclusions, and other complex patterns
- **Drag & drop** — drag files directly from search results to Finder, VS Code, Xcode, or any application
- **Context menu** — Open / Reveal in Finder / Copy Full Name
- **Cmd+Click** — quickly locate files in Finder
- **Recent files** — automatically shows recently modified files when the search bar is empty, in the Everything-style result table

### AI Tool Integration (MCP)

Built-in [Model Context Protocol](https://modelcontextprotocol.io/) server — lets AI coding tools instantly search your file system. Enable with one click from the menu bar. Supports **Claude Code**, **Cursor**, and **Claude Desktop**.

```
Claude Code / Cursor / Claude Desktop
       │
       ▼  (stdio JSON-RPC 2.0)
  MacSoSoMCP
       │
       ▼  (HTTP localhost:19860)
  MacSoSo.app
```

| Tool | Description |
|------|-------------|
| `search_files` | Filename search (Trigram-accelerated) |
| `search_content` | Full-text content search |
| `recent_files` | Recently modified files |
| `index_status` | Index statistics & health |

### HTTP API

Local REST API on `localhost:19860` for scripting and automation:

```bash
curl "http://localhost:19860/api/search?q=readme&limit=10"       # Search files
curl "http://localhost:19860/api/search/content?q=TODO"           # Content search
curl "http://localhost:19860/api/recent?limit=20"                 # Recent files
curl "http://localhost:19860/api/status"                          # Index status
```

### Installation

#### Download DMG (Recommended)

1. Download the DMG for your Mac from [Releases](../../releases):
   `MacSoSo-<version>-arm64.dmg` (Apple Silicon) or
   `MacSoSo-<version>-x86_64.dmg` (Intel)
2. Drag `MacSoSo.app` to Applications
3. If Gatekeeper blocks the first launch, right-click the app → Open, or run
   `xattr -dr com.apple.quarantine /Applications/MacSoSo.app`
4. Launch and grant **Full Disk Access** when prompted
5. Wait for the initial scan (it now includes local volumes under `/Volumes`)
6. Press `Option+Space` to start searching

> Release builds are ad-hoc signed and not notarized by default. Configure a
> Developer ID certificate to ship without Gatekeeper warnings — see
> [Release process](docs/RELEASE.md).

#### Build from Source

**Requirements:** macOS 14+, Xcode 15+, Homebrew `re2` and `abseil`

```bash
brew install re2 abseil
git clone https://github.com/mylogo1024/MacSoso.git && cd MacSoso

# Build + package (bundles re2/abseil, emits DMG and SHA256)
./scripts/package_release.sh --arch arm64
# On Intel use: --arch x86_64
```

Build the app only: `make app`.

#### CLI Daemon

Headless mode for servers or automation environments:

```bash
make daemon
./macsoso-daemon --port 19860 --root /
```

---

<h2 align="center">For Developers: Technical Deep Dive</h2>

<p align="center">
  The following content is for developers interested in implementation details.
</p>

### Architecture Overview

```
┌─────────────────────────────────────┐
│       SwiftUI App Layer             │  UI · ViewModel · MVVM
├─────────────────────────────────────┤
│    Objective-C++ Bridge Layer       │  Zero-overhead interop
├─────────────────────────────────────┤
│       C++20 Core Engine             │  Scan · Index · Search · Persist
└─────────────────────────────────────┘
```

The same C++20 core engine powers three deployment modes:

| Mode | Description |
|------|-------------|
| **GUI App** | SwiftUI menu-bar app, `Option+Space` global hotkey |
| **CLI Daemon** | Headless `macsoso-daemon` — same engine, no UI |
| **MCP Server** | `MacSoSoMCP` — stdio JSON-RPC proxy for AI tools |

### Core Engine

| Component | Key Design |
|-----------|-----------|
| **DirectoryScanner** | Multi-threaded work-stealing + `getattrlistbulk` single-syscall bulk attribute reads, 4–32 threads adaptive |
| **SearchEngine** | Trigram inverted index (name + path dual indexes) + competitive optimal candidate selection + SoA columnar filtering |
| **ContentIndex** | Trigram full-text inverted index, FNV-1a hash incremental updates, only re-indexes changed files |
| **SIMDSearch** | ARM NEON 128-bit first-last byte vectorized matching + 2x loop unrolling, 11.5 GB/s single-thread |
| **IndexPersistence** | WAL + CRC32 + paged dirty-page flushing + atomic rename, COW non-blocking compaction (lock held < 100ms) |
| **FileSystemWatcher** | FSEvents + eventId incremental replay + log truncation detection with automatic subtree rescan |
| **PathTable** | Path string interning — directory paths stored as `uint32` index, saving ~550MB at million-file scale |
| **QueryParser** | Full AST pipeline: Tokenizer → FilterParser → Parser → QueryAST, 30+ filter keywords |

### Benchmarks

Test environment: macOS Darwin 24.3.0, **5.4 million indexed files**, 48 query types:

#### Search Latency

| Query Type | Avg Latency | Example |
|------------|:-----------:|---------|
| Long keywords (7+ chars) | **0.1–1ms** | `screenshot` 0.1ms, `dockerfile` 0.1ms |
| Medium keywords (4–6 chars) | **1–5ms** | `readme` 1.2ms, `config` 4.7ms |
| Glob patterns | **0.7–18ms** | `*.cpp` 0.7ms, `*.swift` 1.5ms |
| Path queries | **3–32ms** | `package.json` 2.9ms |
| All 48 queries (average) | **10.5ms** | Latest results after SoA optimization |

#### Trigram vs Linear Scan

| Query | Trigram | Linear Scan | Speedup |
|-------|:------:|:------:|:------:|
| `node_modules` | 0.5ms | 154ms | **308x** |
| `application` | 2.1ms | 175ms | **83x** |
| `readme` | 1.2ms | 49ms | **41x** |

#### SIMD String Search (Apple M3 Pro)

| Method | Throughput | vs `std::string::find` |
|--------|:---------:|:----------------------:|
| `std::string::find` | 1.2 GB/s | baseline |
| **NEON 128-bit (single-thread)** | **11.5 GB/s** | **9.5x** |
| **NEON 128-bit (12 threads)** | **74.3 GB/s** | **60.7x** |

### Key Technologies

| Technology | Impact |
|------------|--------|
| `getattrlistbulk` | Single syscall for bulk file attributes — avoids per-file `stat` |
| Trigram inverted index | Sub-linear search: 33x–308x faster than linear scan |
| SoA columnar layout | Cache-friendly memory access, SIMD batch-checks 16 records for pure filter queries |
| `__builtin_prefetch` | Prefetch distance 8, hides random memory access latency during candidate verification |
| ARM NEON SIMD | 128-bit vectorized string matching, 2x loop unrolling, approaches memory bandwidth ceiling |
| GCD parallel scan | Multi-core linear scan when trigram can't accelerate |
| StringPool contiguous memory | Filenames packed in a single `char` buffer, SIMD-friendly |
| PathTable interning | Directory paths stored as `uint32` index — saves ~550MB at million-file scale |
| Generation counter | Checked every 1024 iterations, zero-overhead cancellation of stale queries during fast typing |
| APFS Firmlink dedup | inode + devid detection, correctly handles macOS Data/System volume merge loops |
| Regex Trigram pre-filter | Extracts literals from regex to generate trigram candidates, ~7s → <100ms |
| Adaptive Trigram bypass | Falls back to parallel scan when candidate set is too large, avoiding wasteful index lookups |
| COW non-blocking compaction | Copy-on-write, exclusive lock held < 100ms during compaction (was 30–60s) |
| Paged incremental persistence | Only writes dirty pages, typical flush I/O drops from ~112MB to KB-level |

### Testing

79 test modules covering the full stack, with AddressSanitizer and ThreadSanitizer support:

```bash
make test          # Fast unit tests + bridge lint
make test-slow     # Integration tests (full disk scan, FSEvents, E2E)
make test-all      # All tests
make test-asan     # AddressSanitizer
make test-tsan     # ThreadSanitizer
```

Coverage:
- **Core engine**: scan, query, mutation, compaction, ranking, path search
- **Persistence**: WAL CRC integrity, batch replay, race conditions, paged persistence v5
- **Content index**: trigram, compaction, mod-time tracking, WAL tracking
- **Search/Query**: tokenizer, parser, filters, date filters, structured queries, regex trigram, highlight hints
- **Performance**: SIMD search, 10M-record synthetic benchmarks, trigram competition
- **Integration**: thread safety, E2E, HTTP engine hot-swap, MCP protocol
- **Memory safety**: ASan + TSan builds

### Project Structure

```
MacSoSo/
├── Core/                  # C++20 core engine
│   ├── SearchEngine       # Trigram index + parallel query (5 .cpp files)
│   ├── DirectoryScanner   # Multi-threaded bulk scanner
│   ├── ContentIndex       # Full-text inverted index
│   ├── IndexPersistence   # WAL + paged persistence
│   ├── FileSystemWatcher  # FSEvents real-time monitor
│   ├── HttpServer         # Embedded REST API server
│   ├── SIMDSearch         # ARM NEON vectorized search
│   ├── QueryAST/Parser    # Full query language pipeline
│   ├── PathTable          # String interning table
│   └── ServiceEngine      # Lifecycle orchestration
├── Bridge/                # Objective-C++ bridge
│   └── MacSearchBridge    # C++ ↔ Swift zero-overhead interop
├── App/                   # SwiftUI application
│   ├── ContentView        # Main search UI
│   ├── SearchViewModel    # MVVM + tiered debouncing
│   ├── HotkeyManager      # Global hotkey registration
│   └── MCPConfigManager   # One-click MCP setup
├── CLI/                   # Command-line tools
│   ├── daemon_main        # Headless daemon
│   └── mcp_main           # MCP server (stdio JSON-RPC)
└── tests/                 # 79 test modules
```

## Contributing

Contributions are welcome! Please follow this workflow:

1. Fork the repository
2. Create a feature branch (`feat/...`) or bugfix branch (`fix/...`)
3. Write tests for new functionality
4. Ensure `make test-all` passes
5. Submit a Pull Request

## License and Credits

This project is licensed under the MIT License — see [LICENSE](LICENSE).
Upstream attribution, the change summary, and third-party licenses are in
[NOTICE](NOTICE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

<p align="center">
  <b>If MacSoSo helps you find files faster, consider giving it a star!</b>
</p>
