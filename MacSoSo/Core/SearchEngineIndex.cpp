#include "SearchEngine.h"
#include "StringUtils.h"
#include <algorithm>
#include <unordered_set>
#include <thread>
#include <dispatch/dispatch.h>

// ---------------------------------------------------------------------------
// Trigram posting list intersection utilities
// ---------------------------------------------------------------------------

namespace {
/// Narrow `trigrams` (which may contain duplicates) down to the posting lists
/// they name, rarest first. The smallest list is the only one materialised; every
/// remaining trigram is probed against that run, so a keyword made of common
/// trigrams still costs one small vector instead of a copy of each list.
/// `allFound` is cleared when any trigram is absent, which proves nothing matches.
std::vector<uint32_t> intersectByTrigrams(const PostingIndex<Trigram>& index,
                                          const std::vector<Trigram>& trigrams,
                                          bool& allFound) {
    std::vector<Trigram> unique = trigrams;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    if (unique.empty()) { allFound = false; return {}; }

    std::vector<std::pair<uint32_t, Trigram>> ranked;
    ranked.reserve(unique.size());
    for (Trigram t : unique) {
        uint32_t n = index.count(t);
        if (n == 0) { allFound = false; return {}; }
        ranked.emplace_back(n, t);
    }
    std::sort(ranked.begin(), ranked.end());

    std::vector<uint32_t> result;
    index.decode(ranked[0].second, result);
    for (size_t i = 1; i < ranked.size() && !result.empty(); i++) {
        // A varint run has no random access: narrowing costs a full decode of
        // that run (~2ns/posting measured), while the leftover candidates only
        // cost one verification each (~20ns/record on the 5.1M-record index).
        // Stop once narrowing is more than 16x past break-even; on a keyword of
        // common trigrams the extra narrowing prunes almost nothing anyway.
        // Because count(t) <= totalSize, skipping implies result.size() <
        // totalSize/16, so a skipped narrowing can never push the query over the
        // caller's `<= totalSize/10` candidate gate into a linear scan.
        if (static_cast<uint64_t>(ranked[i].first) >
            static_cast<uint64_t>(result.size()) * 16) break;
        result = index.intersectWith(result, ranked[i].second);
    }
    return result;
}
}  // namespace

std::vector<uint32_t> SearchEngine::intersectPostingLists(
    const PostingIndex<Trigram>& index,
    const std::string& keyword,
    bool& allFound) {
    allFound = true;
    return intersectByTrigrams(index, ContentIndex::extractTrigrams(keyword), allFound);
}

std::vector<uint32_t> SearchEngine::intersectPostingListsMulti(
    const PostingIndex<Trigram>& index,
    const std::vector<std::string>& segments,
    bool& allFound) {
    allFound = true;
    std::vector<Trigram> trigrams;
    for (const auto& seg : segments) {
        auto segTrigrams = ContentIndex::extractTrigrams(seg);
        trigrams.insert(trigrams.end(), segTrigrams.begin(), segTrigrams.end());
    }
    return intersectByTrigrams(index, trigrams, allFound);
}

std::vector<uint32_t> SearchEngine::unionPostingListsMulti(
    const PostingIndex<Trigram>& index,
    const std::vector<std::string>& segments) {
    // For each segment, intersect its trigrams to get that segment's candidates.
    // Then UNION across all segments. This is safe for both AND and OR semantics
    // from FilteredRE2 — wider than pure intersection but never drops results.
    std::vector<uint32_t> result;
    for (const auto& seg : segments) {
        bool allFound = false;
        auto segCands = intersectPostingLists(index, seg, allFound);
        if (!allFound) continue;  // skip atoms with no trigram coverage
        if (result.empty()) {
            result = std::move(segCands);
        } else {
            std::vector<uint32_t> merged;
            merged.reserve(result.size() + segCands.size());
            std::set_union(result.begin(), result.end(),
                           segCands.begin(), segCands.end(),
                           std::back_inserter(merged));
            result = std::move(merged);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Name trigram index build / add / remove
// ---------------------------------------------------------------------------

namespace {
/// Sort + de-duplicate a freshly extracted trigram run. Without this, repeated
/// substrings ("aaa" in a name) are posted multiple times, which inflates every
/// posting list and slows down intersections.
void sortAndUnique(std::vector<Trigram>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

/// Release the growth headroom left by push_back. Posting lists are built by
/// appending, so each vector keeps up to 2x the bytes it needs; at 4.6M records
/// that headroom is hundreds of MiB.
template <typename Map>
void shrinkPostings(Map& index) {
    for (auto& [_, list] : index) {
        list.shrink_to_fit();
    }
}
}  // namespace

std::unordered_map<Trigram, std::vector<uint32_t>>
SearchEngine::buildTrigramIndexFromData(const std::vector<uint8_t>& types,
                                        const StringPool& namePool) {
    std::unordered_map<Trigram, std::vector<uint32_t>> index;
    // Reused across records so the hot path performs no per-record allocation.
    std::vector<Trigram> scratch;
    for (uint32_t i = 0; i < namePool.entryCount(); i++) {
        if (types[i] == 0 || !namePool.isLive(i)) continue;
        scratch.clear();
        ContentIndex::extractTrigramsInto(namePool.view(i), scratch);
        sortAndUnique(scratch);
        for (Trigram t : scratch) {
            index[t].push_back(i);
        }
    }
    for (auto& [trigram, list] : index) {
        std::sort(list.begin(), list.end());
        list.shrink_to_fit();
    }
    return index;
}

void SearchEngine::buildTrigramIndex() {
    nameTrigramIndex_.build(buildTrigramIndexFromData(types_, namePool_));
}

void SearchEngine::addTrigramsForRecord(uint32_t idx, const char* data, uint16_t len) {
    // P-2 fix: sort+unique instead of unordered_set
    std::vector<Trigram> trigrams;
    ContentIndex::extractTrigramsInto(std::string_view(data, len), trigrams);
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());

    for (Trigram t : trigrams) nameTrigramIndex_.add(t, idx);
}

void SearchEngine::removeTrigramsForRecord(uint32_t idx) {
    if (idx >= namePool_.entryCount() || !namePool_.isLive(idx)) return;
    // Recompute trigrams from namePool_ instead of storing per-record lists
    std::vector<Trigram> trigrams;
    ContentIndex::extractTrigramsInto(
        std::string_view(namePool_.data(idx), namePool_.length(idx)), trigrams);
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
    for (Trigram t : trigrams) nameTrigramIndex_.remove(t, idx);
}

// ---------------------------------------------------------------------------
// Extension index build / add / remove
// ---------------------------------------------------------------------------

/// Extract lowercase extension from name data. Returns empty string_view if no dot.
/// nameData must already be lowercase (namePool_ guarantees this).
static std::string_view extractExtFromName(const char* data, uint16_t len) {
    for (int i = static_cast<int>(len) - 1; i >= 0; --i) {
        if (data[i] == '.') {
            if (i == static_cast<int>(len) - 1) return {};  // trailing dot
            return std::string_view(data + i + 1, len - i - 1);
        }
    }
    return {};
}

std::unordered_map<std::string, std::vector<uint32_t>>
SearchEngine::buildExtensionIndexFromData(const std::vector<uint8_t>& types,
                                           const StringPool& namePool) {
    std::unordered_map<std::string, std::vector<uint32_t>> index;
    for (uint32_t i = 0; i < namePool.entryCount(); i++) {
        if (types[i] == 0 || !namePool.isLive(i)) continue;
        std::string_view ext = extractExtFromName(namePool.data(i), namePool.length(i));
        if (!ext.empty()) {
            index[std::string(ext)].push_back(i);
        }
    }
    // Posting lists are already in order since we iterate i = 0..N
    return index;
}

void SearchEngine::buildExtensionIndex() {
    extensionIndex_.build(buildExtensionIndexFromData(types_, namePool_));
}

void SearchEngine::addExtensionForRecord(uint32_t idx) {
    if (idx >= namePool_.entryCount() || !namePool_.isLive(idx)) return;
    std::string_view ext = extractExtFromName(namePool_.data(idx), namePool_.length(idx));
    if (ext.empty()) return;
    extensionIndex_.add(std::string(ext), idx);
}

void SearchEngine::removeExtensionForRecord(uint32_t idx) {
    if (idx >= namePool_.entryCount() || !namePool_.isLive(idx)) return;
    std::string_view ext = extractExtFromName(namePool_.data(idx), namePool_.length(idx));
    if (ext.empty()) return;
    extensionIndex_.remove(std::string(ext), idx);
}

// ---------------------------------------------------------------------------
// Path trigram index build / add / remove
// ---------------------------------------------------------------------------

std::unordered_map<Trigram, std::vector<uint32_t>>
SearchEngine::buildPathTrigramIndexFromData(const StringPool& lowerPathPool) {
    std::unordered_map<Trigram, std::vector<uint32_t>> index;
    // Reused across paths so the hot path performs no per-path allocation.
    std::vector<Trigram> scratch;
    for (uint32_t pi = 0; pi < lowerPathPool.entryCount(); pi++) {
        if (!lowerPathPool.isLive(pi)) continue;
        scratch.clear();
        ContentIndex::extractTrigramsInto(lowerPathPool.view(pi), scratch);
        sortAndUnique(scratch);
        for (Trigram t : scratch) {
            index[t].push_back(pi);
        }
    }
    for (auto& [_, list] : index) {
        std::sort(list.begin(), list.end());
        list.shrink_to_fit();
    }
    return index;
}

std::vector<std::vector<uint32_t>>
SearchEngine::buildPathIdxToRecordsFromData(const std::vector<uint8_t>& types,
                                            const std::vector<uint32_t>& pathIndices,
                                            uint32_t pathPoolSize) {
    // Count children per directory first so every list is allocated exactly once.
    std::vector<uint32_t> counts(pathPoolSize, 0);
    for (size_t i = 0; i < types.size(); i++) {
        if (types[i] == 0) continue;
        uint32_t pi = pathIndices[i];
        if (pi < pathPoolSize) counts[pi]++;
    }
    std::vector<std::vector<uint32_t>> mapping(pathPoolSize);
    for (uint32_t pi = 0; pi < pathPoolSize; pi++) {
        if (counts[pi]) mapping[pi].reserve(counts[pi]);
    }
    for (size_t i = 0; i < types.size(); i++) {
        if (types[i] == 0) continue;
        uint32_t pi = pathIndices[i];
        if (pi < mapping.size()) {
            mapping[pi].push_back(static_cast<uint32_t>(i));
        }
    }
    // Appended in ascending record order — no sort pass needed.
    return mapping;
}

void SearchEngine::buildPathTrigramIndex() {
    pathTrigramIndex_.build(buildPathTrigramIndexFromData(lowerPathPool_));
}

void SearchEngine::rebuildPathIdxToRecords() {
    pathIdxToRecords_ = buildPathIdxToRecordsFromData(types_, pathIndices_, pathPool_.entryCount());
}

void SearchEngine::addPathTrigramsForRecord(uint32_t idx) {
    if (idx >= pathIndices_.size()) return;
    uint32_t pi = pathIndices_[idx];
    // Ensure pathIdxToRecords_ is large enough
    if (pi >= pathIdxToRecords_.size()) {
        pathIdxToRecords_.resize(pi + 1);
    }
    auto& list = pathIdxToRecords_[pi];
    auto pos = std::lower_bound(list.begin(), list.end(), idx);
    list.insert(pos, idx);
}

void SearchEngine::removePathTrigramsForRecord(uint32_t idx) {
    if (idx >= pathIndices_.size()) return;
    uint32_t pi = pathIndices_[idx];
    if (pi >= pathIdxToRecords_.size()) return;
    auto& list = pathIdxToRecords_[pi];
    auto pos = std::lower_bound(list.begin(), list.end(), idx);
    if (pos != list.end() && *pos == idx) {
        list.erase(pos);
    }
}

// ---------------------------------------------------------------------------
// Bulk index removal
//
// Removing N records one at a time is quadratic: each per-record removal erases
// from a posting list that is itself O(N) long, so deleting a folder of 50k
// files costs minutes. Filtering each *affected* list once is linear instead.
// ---------------------------------------------------------------------------

namespace {
/// `list` and `toRemove` are both ascending; drop every element of `toRemove`
/// from `list` in a single pass.
void eraseSortedFrom(std::vector<uint32_t>& list, const std::vector<uint32_t>& toRemove) {
    if (list.empty() || toRemove.empty()) return;
    size_t out = 0;
    size_t k = 0;
    for (size_t i = 0; i < list.size(); i++) {
        while (k < toRemove.size() && toRemove[k] < list[i]) k++;
        if (k < toRemove.size() && toRemove[k] == list[i]) continue;
        list[out++] = list[i];
    }
    list.resize(out);
}
}  // namespace

void SearchEngine::removeRecordsFromIndexesBatch(const std::vector<uint32_t>& records) {
    if (records.empty()) return;

    std::vector<Trigram> nameTrigrams;
    std::vector<uint32_t> affectedPaths;
    std::vector<std::string> affectedExts;
    nameTrigrams.reserve(records.size() * 8);
    affectedPaths.reserve(records.size());
    for (uint32_t idx : records) {
        if (idx >= namePool_.entryCount() || !namePool_.isLive(idx)) continue;
        ContentIndex::extractTrigramsInto(namePool_.view(idx), nameTrigrams);
        if (idx < pathIndices_.size() && pathIndices_[idx] < pathIdxToRecords_.size()) {
            affectedPaths.push_back(pathIndices_[idx]);
        }
        std::string_view ext = extractExtFromName(namePool_.data(idx), namePool_.length(idx));
        if (!ext.empty()) affectedExts.emplace_back(ext);
    }
    std::sort(nameTrigrams.begin(), nameTrigrams.end());
    nameTrigrams.erase(std::unique(nameTrigrams.begin(), nameTrigrams.end()), nameTrigrams.end());
    std::sort(affectedPaths.begin(), affectedPaths.end());
    affectedPaths.erase(std::unique(affectedPaths.begin(), affectedPaths.end()), affectedPaths.end());
    std::sort(affectedExts.begin(), affectedExts.end());
    affectedExts.erase(std::unique(affectedExts.begin(), affectedExts.end()), affectedExts.end());

    for (Trigram t : nameTrigrams) nameTrigramIndex_.removeMany(t, records);
    for (uint32_t pi : affectedPaths) {
        eraseSortedFrom(pathIdxToRecords_[pi], records);
    }
    for (const std::string& ext : affectedExts) extensionIndex_.removeMany(ext, records);
}

void SearchEngine::ensurePathTrigramsForPathIdx(uint32_t pathIdx) {
    // New directories only ever land in the delta layer; a pathIdx is unique and
    // is never re-interned, so there is nothing to de-duplicate against the arena.
    std::vector<Trigram> trigrams;
    ContentIndex::extractTrigramsInto(lowerPathPool_.view(pathIdx), trigrams);
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
    for (Trigram t : trigrams) pathTrigramIndex_.add(t, pathIdx);
}

// ---------------------------------------------------------------------------
// Dirty page tracking
// ---------------------------------------------------------------------------

void SearchEngine::markPageDirty(uint32_t recordIndex) {
    uint32_t page = recordIndex / kRecordsPerPage;
    if (page < dirtyPages_.size()) {
        dirtyPages_[page] = true;
    }
}

std::vector<uint32_t> SearchEngine::getDirtyPageNumbers() const {
    std::shared_lock lock(mutex_);
    std::vector<uint32_t> result;
    for (size_t i = 0; i < dirtyPages_.size(); i++) {
        if (dirtyPages_[i]) result.push_back(static_cast<uint32_t>(i));
    }
    return result;
}

void SearchEngine::clearDirtyPages() {
    std::unique_lock lock(mutex_);
    std::fill(dirtyPages_.begin(), dirtyPages_.end(), false);
}
// ---------------------------------------------------------------------------
// Parallel index construction
//
// Phase 2 (background startup) rebuilds every inverted index from the loaded
// SoA columns. Those builds are pure functions over disjoint index ranges, so
// each worker can build a partial index over a contiguous chunk and the
// results can be merged by concatenation — because the chunks are contiguous
// and each is visited in ascending order, the concatenated posting lists come
// out already sorted.
// ---------------------------------------------------------------------------

#include <dispatch/dispatch.h>

/// Clamp a thread count to a sane range for a workload of `n` items.
unsigned parallelThreadsFor(size_t n, size_t minItemsPerThread = 50000) {
    unsigned t = std::thread::hardware_concurrency();
    if (t < 1) t = 1;
    if (t > 32) t = 32;
    size_t maxUseful = (n + minItemsPerThread - 1) / minItemsPerThread;
    if (maxUseful < 1) maxUseful = 1;
    if (t > maxUseful) t = static_cast<unsigned>(maxUseful);
    return t;
}

std::unordered_map<Trigram, std::vector<uint32_t>>
SearchEngine::buildTrigramIndexFromDataParallel(const std::vector<uint8_t>& types,
                                                const StringPool& namePool,
                                                unsigned numThreads) {
    const uint32_t n = namePool.entryCount();
    if (numThreads <= 1 || n < 100000) {
        return buildTrigramIndexFromData(types, namePool);
    }

    __block std::vector<std::unordered_map<Trigram, std::vector<uint32_t>>> partials(numThreads);
    const size_t chunk = (static_cast<size_t>(n) + numThreads - 1) / numThreads;

    dispatch_apply(numThreads, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t t) {
        size_t start = t * chunk;
        size_t end = std::min(start + chunk, static_cast<size_t>(n));
        if (start >= end) return;
        auto& local = partials[t];
        std::vector<Trigram> scratch;
        for (uint32_t i = static_cast<uint32_t>(start); i < static_cast<uint32_t>(end); i++) {
            if (types[i] == 0 || !namePool.isLive(i)) continue;
            scratch.clear();
            ContentIndex::extractTrigramsInto(namePool.view(i), scratch);
            sortAndUnique(scratch);
            for (Trigram tg : scratch) local[tg].push_back(i);
        }
    });

    // Seed the merged index with chunk 0, then append the remaining chunks.
    // Chunk t covers [t*chunk, (t+1)*chunk), so appending in t order keeps
    // every posting list globally sorted.
    std::unordered_map<Trigram, std::vector<uint32_t>> index = std::move(partials[0]);
    for (unsigned t = 1; t < numThreads; t++) {
        for (auto& [tg, list] : partials[t]) {
            auto& dst = index[tg];
            dst.insert(dst.end(), list.begin(), list.end());
        }
        partials[t].clear();
    }
    shrinkPostings(index);
    return index;
}

std::unordered_map<Trigram, std::vector<uint32_t>>
SearchEngine::buildPathTrigramIndexFromDataParallel(const StringPool& lowerPathPool,
                                                    unsigned numThreads) {
    const uint32_t n = lowerPathPool.entryCount();
    if (numThreads <= 1 || n < 100000) {
        return buildPathTrigramIndexFromData(lowerPathPool);
    }

    __block std::vector<std::unordered_map<Trigram, std::vector<uint32_t>>> partials(numThreads);
    const size_t chunk = (static_cast<size_t>(n) + numThreads - 1) / numThreads;

    dispatch_apply(numThreads, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t t) {
        size_t start = t * chunk;
        size_t end = std::min(start + chunk, static_cast<size_t>(n));
        if (start >= end) return;
        auto& local = partials[t];
        std::vector<Trigram> scratch;
        for (uint32_t pi = static_cast<uint32_t>(start); pi < static_cast<uint32_t>(end); pi++) {
            if (!lowerPathPool.isLive(pi)) continue;
            scratch.clear();
            ContentIndex::extractTrigramsInto(lowerPathPool.view(pi), scratch);
            sortAndUnique(scratch);
            for (Trigram tg : scratch) local[tg].push_back(pi);
        }
    });

    std::unordered_map<Trigram, std::vector<uint32_t>> index = std::move(partials[0]);
    for (unsigned t = 1; t < numThreads; t++) {
        for (auto& [tg, list] : partials[t]) {
            auto& dst = index[tg];
            dst.insert(dst.end(), list.begin(), list.end());
        }
        partials[t].clear();
    }
    shrinkPostings(index);
    return index;
}

std::unordered_map<std::string, std::vector<uint32_t>>
SearchEngine::buildExtensionIndexFromDataParallel(const std::vector<uint8_t>& types,
                                                  const StringPool& namePool,
                                                  unsigned numThreads) {
    const uint32_t n = namePool.entryCount();
    if (numThreads <= 1 || n < 100000) {
        return buildExtensionIndexFromData(types, namePool);
    }

    const size_t chunk = (static_cast<size_t>(n) + numThreads - 1) / numThreads;
    __block std::vector<std::unordered_map<std::string, std::vector<uint32_t>>> partials(numThreads);

    dispatch_apply(numThreads, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t t) {
        size_t start = t * chunk;
        size_t end = std::min(start + chunk, static_cast<size_t>(n));
        if (start >= end) return;
        auto& local = partials[t];
        for (uint32_t i = static_cast<uint32_t>(start); i < static_cast<uint32_t>(end); i++) {
            if (types[i] == 0 || !namePool.isLive(i)) continue;
            std::string_view ext = extractExtFromName(namePool.data(i), namePool.length(i));
            if (!ext.empty()) local[std::string(ext)].push_back(i);
        }
    });

    std::unordered_map<std::string, std::vector<uint32_t>> index = std::move(partials[0]);
    for (unsigned t = 1; t < numThreads; t++) {
        for (auto& [ext, list] : partials[t]) {
            auto& dst = index[ext];
            dst.insert(dst.end(), list.begin(), list.end());
        }
        partials[t].clear();
    }
    shrinkPostings(index);
    return index;
}
