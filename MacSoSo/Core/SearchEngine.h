#pragma once
#include "FileRecord.h"
#include "ContentIndex.h" // for Trigram type and extractTrigrams/makeTrigram
#include "PathIndexTable.h"
#include "PostingIndex.h"
#include "StringPool.h"
#include "SIMDSearch.h"
#include <vector>
#include <string>
#include <string_view>
#include <cstdint>
#include <cstdio>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <utility>

#include "IndexWAL.h"
#include "QueryAST.h"
#include "StructuredQueryParser.h"
#include "QueryCache.h"

/// Deduplication table for directory path strings.
/// Interns unique paths and returns a compact uint32_t index.
class PathTable {
public:
    /// Look up or insert a path, returning its index.
    uint32_t intern(const std::string& path) {
        auto it = lookup_.find(path);
        if (it != lookup_.end()) return it->second;
        uint32_t idx = static_cast<uint32_t>(paths_.size());
        paths_.push_back(path);
        lookup_[path] = idx;
        return idx;
    }

    /// Resolve an index back to its path string.
    /// Returns empty string if index is out of range.
    const std::string& resolve(uint32_t index) const {
        static const std::string empty;
        if (index >= paths_.size()) return empty;
        return paths_[index];
    }

    /// Number of unique paths stored.
    uint32_t size() const { return static_cast<uint32_t>(paths_.size()); }

    /// Clear all entries.
    void clear() { paths_.clear(); lookup_.clear(); }

    PathTable() = default;
    PathTable(PathTable&&) = default;
    PathTable& operator=(PathTable&&) = default;
    PathTable(const PathTable&) = default;
    PathTable& operator=(const PathTable&) = default;

private:
    std::vector<std::string> paths_;                    // index -> path
    std::unordered_map<std::string, uint32_t> lookup_;  // path -> index
};

/// Metadata stored in the index header (v3+). Extensible key-value pairs.
struct IndexMetadata {
    uint32_t formatVersion = 0; // populated on load
    int64_t  timestamp = 0;     // creation timestamp (seconds since epoch)
    uint64_t lastEventId = 0;   // FSEvents stream ID for incremental replay
    std::map<std::string, std::string> extra; // extensible key-value pairs

    // Well-known metadata keys
    static constexpr const char* kScanRoot     = "scan_root";      // e.g. "/"
    static constexpr const char* kAppVersion   = "app_version";    // e.g. "1.0.0"
    static constexpr const char* kOSVersion    = "os_version";     // e.g. "14.5"
    static constexpr const char* kRecordFormat = "record_format";  // e.g. "v2_inode"
};

/// Detailed timing breakdown from a search query.
struct QueryTimingInfo {
    double totalMs = 0;
    double lockWaitMs = 0;
    double trigramMs = 0;     // trigram index lookup (trigram path only)
    double phase1Ms = 0;      // name verification (trigram path only)
    double phase2Ms = 0;      // path-only match / NEON full scan
    double sortMs = 0;
    double lockHeldMs = 0;
    size_t totalRecords = 0;
    size_t candidates = 0;    // trigram candidates count
    size_t nameMatches = 0;   // phase1 results (name matches)
    size_t pathMatches = 0;   // phase2 results (path-only matches)
    size_t resultCount = 0;
    bool usedTrigram = false;
    std::string searchPath;   // "trigram", "trigram-split", or "linear"
    bool fromCache = false;   // true if the result came from the result cache
};

/// Bundled cancellation context passed through query call chain.
struct QueryCancelCtx {
    const std::atomic<uint64_t>* gen;
    uint64_t myGen;
    bool cancelled() const {
        return gen->load(std::memory_order_relaxed) != myGen;
    }
};

class SearchEngine {
public:
    /// Takes ownership of scanned records and pre-computes lowercase names.
    void loadRecords(std::vector<FileRecord>&& records);

    /// v5 fast-path load: accepts pre-separated data from on-disk path dictionary.
    /// Skips toLower of names and path dedup since they are pre-computed.
    void loadRecordsV5(std::vector<FileRecord>&& records,
                       std::vector<std::string>&& lowerNames,
                       std::vector<uint32_t>&& pathIndices,
                       StringPool&& pathDict,
                       StringPool&& lowerPathDict);

    /// v6 fast-path load: accepts pre-separated SoA columns and StringPools.
    /// Only builds pathIndex_ (required for mutations). Trigram indices deferred to Phase 2.
    void loadRecordsV6(StringPool&& origNamePool,
                       StringPool&& namePool,
                       std::vector<uint32_t>&& pathIndices,
                       StringPool&& pathPool,
                       StringPool&& lowerPathPool,
                       std::vector<uint8_t>&& types,
                       std::vector<uint64_t>&& sizes,
                       std::vector<int64_t>&& modTimes,
                       std::vector<uint64_t>&& inodes,
                       std::vector<int32_t>&& devIds);

    /// Complete Phase 2 of two-stage startup: build trigram indices in background,
    /// then swap under lock and replay mutations that occurred during build.
    void completePhase2();

    /// Whether Phase 2 is pending (trigram indices not yet built).
    bool isPhase2Pending() const { return phase2Pending_.load(std::memory_order_acquire); }

    /// Snapshot data for v6 serialization (thread-safe).
    struct V6Snapshot {
        StringPool origNamePool;  // original-case names
        StringPool namePool;      // lowercase names
        std::vector<uint32_t> pathIndices;
        StringPool pathPool;
        StringPool lowerPathPool;
        std::vector<uint8_t> types;
        std::vector<uint64_t> sizes;
        std::vector<int64_t> modTimes;
        std::vector<uint64_t> inodes;
        std::vector<int32_t> devIds;
        uint32_t liveCount;
    };
    V6Snapshot snapshotForV6() const;

    // --- Parallel index construction (Phase 2 background build) -----------
    // Each variant splits the record range into contiguous chunks, builds a
    // partial index per chunk on a GCD worker, then merges by concatenation.
    // Because chunks are contiguous and visited in ascending order, the merged
    // posting lists come out already sorted — no re-sort pass is needed.
    // Falls back to the serial version for small inputs or 1 hardware thread.

    static std::unordered_map<Trigram, std::vector<uint32_t>>
        buildTrigramIndexFromDataParallel(const std::vector<uint8_t>& types,
                                          const StringPool& namePool,
                                          unsigned numThreads);

    static std::unordered_map<Trigram, std::vector<uint32_t>>
        buildPathTrigramIndexFromDataParallel(const StringPool& lowerPathPool,
                                              unsigned numThreads);

    static std::unordered_map<std::string, std::vector<uint32_t>>
        buildExtensionIndexFromDataParallel(const std::vector<uint8_t>& types,
                                            const StringPool& namePool,
                                            unsigned numThreads);

    // Serial counterparts, exposed so tests can verify that the parallel
    // builders produce identical indices.
    static std::unordered_map<Trigram, std::vector<uint32_t>>
        buildTrigramIndexFromData(const std::vector<uint8_t>& types,
                                  const StringPool& namePool);

    static std::unordered_map<Trigram, std::vector<uint32_t>>
        buildPathTrigramIndexFromData(const StringPool& lowerPathPool);

    static std::vector<std::vector<uint32_t>>
        buildPathIdxToRecordsFromData(const std::vector<uint8_t>& types,
                                      const std::vector<uint32_t>& pathIndices,
                                      uint32_t pathPoolSize);

    static std::unordered_map<std::string, std::vector<uint32_t>>
        buildExtensionIndexFromData(const std::vector<uint8_t>& types,
                                    const StringPool& namePool);


    /// Case-insensitive substring search. Returns indices into the records array.
    /// If maxResults > 0, stops early once enough matches are found.
    /// If useTrigram is false, bypasses trigram index and does NEON full scan.
    std::vector<uint32_t> query(const std::string& keyword, uint32_t maxResults = 0,
                                bool useTrigram = true, uint64_t sessionId = 0) const;

    /// Same as query() but also populates timing breakdown.
    std::vector<uint32_t> query(const std::string& keyword, uint32_t maxResults,
                                bool useTrigram, QueryTimingInfo& timing,
                                uint64_t sessionId = 0) const;

    /// Number of entries currently held in the rendered-result cache.
    /// Exposed for diagnostics and tests.
    size_t queryCacheSize() const { return queryCache_.size(); }

    /// Per-structure heap usage breakdown (MiB) of the in-memory index.
    /// Logged once after startup; also served by the status API.
    std::string memoryProfile() const;

    /// Give back capacity that bulk loading and appending left behind.
    /// Idempotent and cheap when nothing over-allocates; call it once the index
    /// has settled so an idle daemon does not pay for vector growth headroom.
    void trimCapacity();

    /// Advanced query: evaluate an AST with boolean operators, quoted phrases,
    /// and filter nodes. Uses trigram index for TERM nodes, then applies
    /// AND/OR/NOT logic. Called automatically by query() when advanced syntax
    /// is detected.
    std::vector<uint32_t> queryAdvanced(const std::string& input, uint32_t maxResults,
                                         bool useTrigram, QueryTimingInfo& timing,
                                         uint64_t myGen = 0,
                                         const std::atomic<uint64_t>* genPtr = nullptr) const;

    /// Cancel in-flight queries for the given session. Lock-free on the generation atomic.
    void cancelSession(uint64_t sessionId) const;

    /// Get or create the generation atomic for a session. Returns {atomicPtr, currentGenValue}.
    std::pair<std::shared_ptr<std::atomic<uint64_t>>, uint64_t>
    acquireSessionGeneration(uint64_t sessionId) const;

    FileRecord getRecord(uint32_t index) const;
    uint32_t recordCount() const;

    // --- Incremental mutation (for FSEvents) ---

    /// Batch mutation operation descriptor.
    struct MutationOp {
        enum Type { REMOVE, UPDATE };
        Type type;
        std::string path;
        FileRecord record;  // only used for UPDATE
    };

    /// Append a new record. Returns the new index. Thread-safe.
    uint32_t addRecord(FileRecord&& record);

    /// Mark record at fullPath as deleted (tombstone). Thread-safe.
    /// Returns true if found and removed.
    bool removeByPath(const std::string& fullPath);

    /// Remove old record at fullPath (if any) and add updated record. Thread-safe.
    void updateByPath(const std::string& fullPath, FileRecord&& updated);

    /// Apply multiple mutations under a single write lock. Thread-safe.
    /// Reduces lock contention compared to calling removeByPath/updateByPath individually.
    void batchMutate(std::vector<MutationOp>&& ops);

    /// Remove all records whose full path starts with the given prefix. Thread-safe.
    /// Returns the number of records removed.
    uint32_t removeByPathPrefix(const std::string& pathPrefix);

    /// Batch-replace all records under pathPrefix with freshRecords.
    /// Tombstones old records, adds fresh records, and rebuilds trigram index once.
    /// Returns the number of old records removed. Thread-safe.
    uint32_t batchRescanPrefix(const std::string& pathPrefix,
                               std::vector<FileRecord>&& freshRecords);

    /// Replay WAL entries in-place using pathIndex_ for O(1) lookups.
    /// Skips WAL writes (entries came FROM the WAL). Thread-safe.
    void replayWALEntries(std::vector<WALEntry>&& entries);

    /// Number of live (non-tombstoned) records.
    uint32_t liveRecordCount() const { return liveCount_.load(std::memory_order_relaxed); }

    /// Compact in-memory records by removing tombstoned entries.
    /// Rebuilds pathIndex_ and namePool_. Thread-safe.
    /// Returns a mapping from old index → new index for live records.
    /// Returns empty map if nothing was compacted.
    std::unordered_map<uint32_t, uint32_t> compactRecords();

    /// Save live records to a binary file (format v3). Thread-safe.
    /// metadata.extra can contain arbitrary key-value pairs for forward compatibility.
    bool saveToFile(const std::string& filePath, const IndexMetadata& metadata) const;

    /// Legacy overload for backward compatibility.
    bool saveToFile(const std::string& filePath, uint64_t lastEventId = 0) const;

    /// Load records from a binary file (v1, v2, or v3), rebuilding indices. Thread-safe.
    /// outMetadata receives all metadata including formatVersion, timestamp, lastEventId, and extra pairs.
    bool loadFromFile(const std::string& filePath, IndexMetadata* outMetadata = nullptr);

    /// Legacy overload for backward compatibility.
    bool loadFromFile(const std::string& filePath, uint64_t* outLastEventId);

    /// Attach a WAL for logging mutations. Must be already opened.
    void attachWAL(std::shared_ptr<IndexWAL> wal);

    /// Detach the WAL (e.g. before compaction).
    void detachWAL();

    /// Records per page for paged persistence
    static constexpr uint32_t kRecordsPerPage = 1024;

    /// Get the list of dirty page numbers (pages modified since last clearDirtyPages). Thread-safe.
    std::vector<uint32_t> getDirtyPageNumbers() const;

    /// Clear the dirty page bitmap. Thread-safe.
    void clearDirtyPages();

    /// Whether a full rewrite is needed (set after compactRecords renumbers indices).
    bool needsFullRewrite() const { return fullRewriteNeeded_.load(std::memory_order_relaxed); }
    void clearFullRewriteNeeded() { fullRewriteNeeded_.store(false, std::memory_order_relaxed); }

    /// Monotonically increasing generation counter, incremented on each compaction.
    uint64_t compactionGeneration() const { return compactionGen_.load(std::memory_order_relaxed); }

    /// Return indices of the most recently modified live records, sorted by modTime descending.
    /// Accesses recentCache_ directly under the lock to avoid per-record copy overhead.
    std::vector<uint32_t> recentIndices(uint32_t count) const;

    /// Look up the record index for a given full path. Returns UINT32_MAX if not found.
    uint32_t indexForPath(const std::string& fullPath) const;

    /// Export a copy of all live records with their paths restored. Thread-safe.
    std::vector<FileRecord> exportRecords() const;

    /// Build the full path from a record's path and name components.
    static std::string makeFullPath(std::string_view path, std::string_view name);

    /// Resolve a record's path via pathPool_. Thread-safe (acquires shared_lock).
    /// Returns by value to prevent dangling references during concurrent compaction.
    std::string resolveRecordPath(uint32_t index) const {
        std::shared_lock lock(mutex_);
        if (index >= pathIndices_.size()) return {};
        return pathPool_.str(pathIndices_[index]);
    }

    /// Batch callback access under a single shared_lock. Reconstructs FileRecord from SoA.
    /// Callback signature: void(uint32_t idx, const FileRecord& record, const std::string& path)
    template<typename Func>
    void forEachRecordWithPath(const std::vector<uint32_t>& indices, Func&& func) const {
        std::shared_lock lock(mutex_);
        for (uint32_t idx : indices) {
            if (idx >= types_.size() || types_[idx] == 0) continue;
            std::string path = pathPool_.str(pathIndices_[idx]);
            FileRecord rec;
            rec.name = origNamePool_.str(idx);
            rec.path = path;
            rec.type = types_[idx];
            rec.size = sizes_[idx];
            rec.modTime = static_cast<time_t>(modTimes_[idx]);
            rec.inode = inodes_[idx];
            rec.devId = devIds_[idx];
            func(idx, rec, path);
        }
    }

    /// Batch callback for a contiguous range of records, including tombstones.
    /// Used by PagedIndexWriter to serialize pages with stable index positions.
    /// Callback signature: void(uint32_t idx, const FileRecord& record, const std::string& path)
    template<typename Func>
    void forEachRecordInRange(uint32_t startIdx, uint32_t count, Func&& func) const {
        std::shared_lock lock(mutex_);
        uint32_t end = std::min(startIdx + count, static_cast<uint32_t>(types_.size()));
        for (uint32_t i = startIdx; i < end; i++) {
            std::string path = pathPool_.str(pathIndices_[i]);
            FileRecord rec;
            rec.name = origNamePool_.str(i);
            rec.path = path;
            rec.type = types_[i];
            rec.size = sizes_[i];
            rec.modTime = static_cast<time_t>(modTimes_[i]);
            rec.inode = inodes_[i];
            rec.devId = devIds_[i];
            func(i, rec, path);
        }
    }

    /// Batch callback for v5 serialization. Provides pathIdx and lower name data directly.
    /// Callback signature: void(uint32_t idx, const FileRecord& rec, uint32_t pathIdx,
    ///                          const char* lowerName, uint16_t lowerNameLen)
    template<typename Func>
    void forEachRecordInRangeV5(uint32_t startIdx, uint32_t count, Func&& func) const {
        std::shared_lock lock(mutex_);
        uint32_t end = std::min(startIdx + count, static_cast<uint32_t>(types_.size()));
        for (uint32_t i = startIdx; i < end; i++) {
            FileRecord rec;
            rec.name = origNamePool_.str(i);
            rec.type = types_[i];
            rec.size = sizes_[i];
            rec.modTime = static_cast<time_t>(modTimes_[i]);
            rec.inode = inodes_[i];
            rec.devId = devIds_[i];
            func(i, rec, pathIndices_[i],
                 namePool_.data(i), namePool_.length(i));
        }
    }

    /// Thread-safe copy of pathPool_ for serialization.
    StringPool pathPoolSnapshot() const {
        std::shared_lock lock(mutex_);
        return pathPool_;
    }

    /// Thread-safe copy of lowerPathPool_ for serialization.
    StringPool lowerPathPoolSnapshot() const {
        std::shared_lock lock(mutex_);
        return lowerPathPool_;
    }

    /// Thread-safe snapshot of PathTable. Reconstructs from pathPool_ for compatibility.
    PathTable pathTableSnapshot() const {
        std::shared_lock lock(mutex_);
        PathTable pt;
        for (uint32_t i = 0; i < pathPool_.entryCount(); i++) {
            if (pathPool_.isLive(i)) {
                pt.intern(pathPool_.str(i));
            }
        }
        return pt;
    }

private:
    StringPool origNamePool_;              // contiguous original-case filenames (for v6 persistence)
    StringPool namePool_;                  // contiguous lowercase filenames
    std::vector<uint32_t> pathIndices_;    // per-record index into pathPool_
    StringPool pathPool_;                  // contiguous directory paths (deduplicated)
    StringPool lowerPathPool_;             // parallel to pathPool_, stores pre-lowered paths

    // SoA columnar arrays — the canonical per-record storage
    std::vector<uint8_t>  types_;          // 1=file, 2=directory, 0=tombstone
    std::vector<uint64_t> sizes_;          // file size in bytes
    std::vector<int64_t>  modTimes_;       // modification time (Unix epoch)
    std::vector<uint64_t> inodes_;         // inode number
    std::vector<int32_t>  devIds_;         // device ID
    /// Directory interning tables. Keys are the hash of a directory as it is
    /// stored in the matching pool; the pool itself owns the text, so nothing is
    /// duplicated here.
    PathIndexTable pathLookup_;       // path hash -> pathPool_ index
    PathIndexTable lowerPathLookup_;  // lowered path hash -> pathPool_ index
    PathIndexTable pathIndex_;      // lowercase full path hash -> record index
    std::atomic<uint32_t> liveCount_{0};
    mutable std::shared_mutex mutex_;

    // Rendered-result LRU cache (the "index cache" for query answers).
    // Mutated on every index change (see invalidateQueryCache()) and read on
    // each query(); guarded internally by its own mutex, so a hit needs no
    // engine lock at all.
    mutable QueryResultCache queryCache_;
    void invalidateQueryCache() { queryCache_.clear(); }

    // Trigram inverted index for fast filename search
    PostingIndex<Trigram> nameTrigramIndex_;   // trigram -> record indices

    // Path trigram index: two-level lookup for fast path-only matching
    PostingIndex<Trigram> pathTrigramIndex_;   // trigram -> sorted pathIdx
    std::vector<std::vector<uint32_t>> pathIdxToRecords_; // pathIdx -> sorted record indices

    // Extension index: extension string -> sorted record indices (files only)
    PostingIndex<std::string> extensionIndex_;

    /// Intern a directory path into pathPool_ and lowerPathPool_. Returns pathPool_ index.
    /// Deduplicates via pathLookup_. Must be called under unique_lock.
    uint32_t internPath(const std::string& path);

    /// --- pathIndex_ helpers (lowercase full path -> record index) -----------
    /// All three take the path in already-lowered form and verify hits against
    /// the record's own directory + name, so a hash collision cannot alias two
    /// different files. Caller must hold mutex_.

    uint32_t findRecordByLoweredPathUnlocked(std::string_view loweredFull) const;
    void indexRecordUnlocked(std::string_view loweredFull, uint32_t idx);
    bool unindexRecordUnlocked(std::string_view loweredFull);

    /// Hash of record `idx`'s lowercase full path, folded straight out of the
    /// pools so a bulk load never materialises one joined string per record.
    uint64_t loweredPathHashUnlocked(uint32_t idx) const;

    /// Bulk-load counterpart of indexRecordUnlocked() for callers that hold only
    /// record indices. Last writer wins on duplicates, like the old map.
    void indexRecordByIdxUnlocked(uint32_t idx);

    /// True when both records' interned directory + name are identical.
    bool sameLoweredPathUnlocked(uint32_t a, uint32_t b) const;

    /// Drop record `idx` from pathIndex_ using its own pooled path halves, so a
    /// bulk removal never builds a joined path string.
    bool unindexRecordByIdxUnlocked(uint32_t idx);

    /// --- pathLookup_ / lowerPathLookup_ helpers (directory -> pathPool_ index) ---
    /// Keys are hashes only; a hit is confirmed against the pool text, so a
    /// collision cannot alias two directories. Return PathIndexTable::kNotFound
    /// when absent. Caller must hold mutex_.
    uint32_t findPathIdxUnlocked(std::string_view path) const;
    uint32_t findLowerPathIdxUnlocked(std::string_view loweredPath) const;
    /// Register a freshly interned pathPool_/lowerPathPool_ entry in both tables.
    void insertPathLookupUnlocked(uint32_t pIdx);
    /// Rebuild both lookup tables from the live pathPool_ entries.
    void rebuildPathLookupsUnlocked();

    /// Bulk-build pathIndex_ over records [0, n): hashes are folded in parallel,
    /// insertion stays serial so the table is sized once.
    void buildPathIndexUnlocked(size_t n);

    /// Record indices whose full path equals `lowerPrefix` or lies beneath it at
    /// a path-component boundary. Result is a snapshot, safe to mutate the index
    /// while consuming it. Caller must hold mutex_.
    std::vector<uint32_t> collectSubtreeRecordsUnlocked(const std::string& lowerPrefix) const;

    /// Tombstone a record at idx: clear SoA columns. Must be called under unique_lock.
    void tombstoneAt(uint32_t idx);

    /// Append a record to SoA columns. Must be called under unique_lock.
    void pushRecord(FileRecord&& rec);

    /// Unlocked variants — caller must hold unique_lock on mutex_.
    bool removeByPathUnlocked(const std::string& fullPath);
    void updateByPathUnlocked(const std::string& fullPath, FileRecord&& updated);

    /// Build trigram index from namePool_ (called inside loadRecords/compactRecords under lock)
    void buildTrigramIndex();
    /// Add trigrams for a single record to the index
    void addTrigramsForRecord(uint32_t idx, const char* data, uint16_t len);
    /// Remove trigrams for a single record from the index
    void removeTrigramsForRecord(uint32_t idx);

    /// Bulk variant of removeTrigramsForRecord()/removePathTrigramsForRecord()/
    /// removeExtensionForRecord(): filters every affected posting list once
    /// instead of once per record, which turns a folder-sized removal from
    /// quadratic into linear. `records` must be sorted ascending and still
    /// live, so call it before tombstoning. Caller must hold unique_lock.
    void removeRecordsFromIndexesBatch(const std::vector<uint32_t>& records);

    /// Build extension index from namePool_ (maps extension -> sorted record indices)
    void buildExtensionIndex();
    /// Add a record to the extension index (sorted insert)
    void addExtensionForRecord(uint32_t idx);
    /// Remove a record from the extension index
    void removeExtensionForRecord(uint32_t idx);

    /// Build path trigram index from pathPool_
    void buildPathTrigramIndex();
    /// Rebuild pathIdx -> record indices mapping
    void rebuildPathIdxToRecords();
    /// Add a record to pathIdxToRecords_ (sorted insert)
    void addPathTrigramsForRecord(uint32_t idx);
    /// Remove a record from pathIdxToRecords_ (sorted remove)
    void removePathTrigramsForRecord(uint32_t idx);
    /// Ensure pathTrigramIndex_ has entries for a given pathIdx (called when PathTable grows)
    void ensurePathTrigramsForPathIdx(uint32_t pathIdx);

    static bool globMatch(const std::string& pattern, const std::string& text);

    /// Match result from search: (record index, priority, full path length).
    /// Priority: 0=exact, 1=starts-with, 2=contains, 3=path-only match.
    struct Match { uint32_t idx; uint8_t priority; uint32_t pathLen; };

    /// Node-centric structured query: name trigram → name verify → path constraint verify.
    /// Handles SEGMENTS and DIR_EXACT modes.
    void queryStructured(const ParsedQuery& pq,
                         size_t totalSize, const QueryCancelCtx& cancel,
                         std::vector<Match>& merged) const;

    /// DIR_LIST mode: find matching directories, return their direct children.
    void queryDirList(const ParsedQuery& pq,
                      size_t totalSize, const QueryCancelCtx& cancel,
                      std::vector<Match>& merged) const;

public:
    /// Check if a directory path matches the path segment constraints.
    static bool pathSegmentsMatch(std::string_view dirPath,
                                  const std::vector<PathSegment>& segments);
private:

    /// Memory breakdown. Caller must hold mutex_.
    std::string memoryProfileUnlocked() const;

    /// Estimate trigram candidate count for a keyword (min posting list size).
    /// Returns SIZE_MAX if keyword is < 3 chars, 0 if any trigram not in index.
    size_t estimateTrigramCost(const std::string& keyword) const;

    /// Tree-walk from anchor directory down to target segment, collecting matches.
    void treeWalkDown(uint32_t dirIdx, const ParsedQuery& pq,
                      int fromSegIdx, int toSegIdx,
                      size_t totalSize, const QueryCancelCtx& cancel,
                      std::vector<Match>& merged) const;

    /// Anchor on namePattern: trigram intersect + path verify. Returns false to fall back.
    bool queryStructuredNameAnchor(const ParsedQuery& pq,
                                   size_t totalSize, const QueryCancelCtx& cancel,
                                   std::vector<Match>& merged) const;

    /// Anchor on path segment: trigram intersect + tree-walk. Returns false to fall back.
    bool queryStructuredPathAnchor(const ParsedQuery& pq,
                                   size_t anchorIdx,
                                   size_t totalSize, const QueryCancelCtx& cancel,
                                   std::vector<Match>& merged) const;

    std::vector<bool> dirtyPages_;                // dirty page bitmap
    std::atomic<bool> fullRewriteNeeded_{false};   // set by compactRecords()

    /// Mark the page containing the given record index as dirty. Must be called under lock.
    void markPageDirty(uint32_t recordIndex);

    std::shared_ptr<IndexWAL> wal_;
    std::atomic<uint64_t> compactionGen_{0};

    // Phase 2 two-stage startup: trigram indices built in background
    std::atomic<bool> phase2Pending_{false};
    uint32_t phase2StartRecordCount_{0};

    // Per-session query generation for cancellation.
    // Key: sessionId (0 = no cancellation). Value: shared atomic counter.
    mutable std::unordered_map<uint64_t, std::shared_ptr<std::atomic<uint64_t>>> sessionGenerations_;
    mutable std::mutex sessionGenMutex_;  // protects sessionGenerations_ map only

    // Recent files cache: top-K records by modTime, maintained incrementally
    static constexpr uint32_t kRecentCacheSize = 200;
    struct RecentEntry {
        time_t modTime;
        uint32_t index;
        bool operator<(const RecentEntry& o) const {
            return modTime > o.modTime || (modTime == o.modTime && index > o.index);
        }
    };
    std::set<RecentEntry> recentCache_;
    void rebuildRecentCache();
    void addToRecentCache(uint32_t idx, time_t modTime);
    void removeFromRecentCache(uint32_t idx, time_t modTime);

    /// Compute match priority: 0=exact, 1=starts-with, 2=contains.
    static uint8_t namePriority(const char* nameData, uint16_t nameLen,
                                const char* keyData, size_t keyLen);

    /// Intersect posting lists from a trigram index for a given keyword.
    /// Returns sorted candidate indices, or empty if any trigram is missing.
    /// Sets allFound to false if any trigram is not in the index.
    static std::vector<uint32_t> intersectPostingLists(
        const PostingIndex<Trigram>& index,
        const std::string& keyword,
        bool& allFound);

    /// Intersect trigrams from multiple literal segments for complex glob pre-filtering.
    static std::vector<uint32_t> intersectPostingListsMulti(
        const PostingIndex<Trigram>& index,
        const std::vector<std::string>& segments,
        bool& allFound);

    /// Union posting lists across multiple atoms for regex pre-filtering.
    /// Each atom's trigrams are intersected internally, then results are unioned.
    /// Safe for both AND and OR semantics from FilteredRE2.
    static std::vector<uint32_t> unionPostingListsMulti(
        const PostingIndex<Trigram>& index,
        const std::vector<std::string>& segments);

    /// Build full path in a reusable buffer: path + '/' + name.
    /// Lowercases the path portion via SIMD. Returns the full path length.
    /// Buffer is resized if needed (with 2x growth).
    static size_t buildFullPathBuf(std::vector<char>& buf,
                                   const char* pathData, uint16_t pathLen,
                                   const char* nameData, uint16_t nameLen);

    /// Build trigram index from standalone data (no member access, used by COW compaction)

    /// Build extension index from standalone data (no member access, used by COW compaction)

    /// Build path trigram index from pre-lowered StringPool (used by COW compaction)

    /// Build pathIdx -> record indices mapping from standalone data (used by COW compaction)

    /// Build recent cache from standalone data (no member access, used by COW compaction)
    static std::set<RecentEntry>
        buildRecentCacheFromData(const std::vector<uint8_t>& types,
                                 const std::vector<int64_t>& modTimes,
                                 uint32_t cacheSize);

    // --- Everything-compatible aggregate filters -------------------------
    // These need data that spans records (child counts, duplicate detection)
    // and therefore run as a post-pass over the candidate set instead of
    // inside per-record evaluation.

    /// Apply the aggregate post-pass filters to the match set in place.
    /// Caller must hold at least a shared_lock on mutex_.
    void applyAggregateFilters(std::vector<Match>& matches,
                               const std::vector<const QueryNode*>& nodes) const;

    /// Apply `distinct:` / `unique:` result controls to the match set in place.
    /// `distinctBy` is a ';'-separated property list (name/size/ext/path),
    /// empty = "name". `uniqueMode` = true keeps only values that occur once.
    /// Caller must hold at least a shared_lock on mutex_.
    void applyDistinctFilter(std::vector<Match>& matches,
                             const std::string& distinctBy,
                             bool uniqueMode) const;

    /// Sort matches according to spec and write up to resultCount indices
    /// into out. May reorder `matches` in place.
    /// Caller must hold at least a shared_lock on mutex_.
    void sortMatches(std::vector<Match>& matches, const SortSpec& spec,
                     size_t resultCount, std::vector<uint32_t>& out) const;

    /// Default relevance comparator (match quality, then path length).
    static bool relevanceLess(const Match& a, const Match& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.pathLen < b.pathLen;
    }
};
