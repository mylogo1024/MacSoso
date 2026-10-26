#include "SearchEngine.h"
#include "StringUtils.h"
#include <algorithm>
#include <unordered_set>
#include <thread>
#include <dispatch/dispatch.h>

// ---------------------------------------------------------------------------
// Trigram posting list intersection utilities
// ---------------------------------------------------------------------------

std::vector<uint32_t> SearchEngine::intersectPostingLists(
    const std::unordered_map<Trigram, std::vector<uint32_t>>& index,
    const std::string& keyword,
    bool& allFound) {
    allFound = true;
    auto keyTrigrams = ContentIndex::extractTrigrams(keyword);
    std::unordered_set<Trigram> unique(keyTrigrams.begin(), keyTrigrams.end());
    if (unique.empty()) { allFound = false; return {}; }

    std::vector<const std::vector<uint32_t>*> postings;
    for (Trigram t : unique) {
        auto it = index.find(t);
        if (it == index.end()) { allFound = false; return {}; }
        postings.push_back(&it->second);
    }

    std::sort(postings.begin(), postings.end(),
        [](const auto* a, const auto* b) { return a->size() < b->size(); });

    std::vector<uint32_t> result;
    result.reserve(postings[0]->size());
    result.assign(postings[0]->begin(), postings[0]->end());

    for (size_t i = 1; i < postings.size() && !result.empty(); i++) {
        const auto& other = *postings[i];
        std::vector<uint32_t> isect;
        isect.reserve(std::min(result.size(), other.size()));
        std::set_intersection(result.begin(), result.end(),
                              other.begin(), other.end(),
                              std::back_inserter(isect));
        result = std::move(isect);
    }
    return result;
}

std::vector<uint32_t> SearchEngine::intersectPostingListsMulti(
    const std::unordered_map<Trigram, std::vector<uint32_t>>& index,
    const std::vector<std::string>& segments,
    bool& allFound) {
    allFound = true;
    // Collect trigrams from all segments and deduplicate
    std::unordered_set<Trigram> unique;
    for (const auto& seg : segments) {
        auto segTrigrams = ContentIndex::extractTrigrams(seg);
        unique.insert(segTrigrams.begin(), segTrigrams.end());
    }
    if (unique.empty()) { allFound = false; return {}; }

    std::vector<const std::vector<uint32_t>*> postings;
    for (Trigram t : unique) {
        auto it = index.find(t);
        if (it == index.end()) { allFound = false; return {}; }
        postings.push_back(&it->second);
    }

    std::sort(postings.begin(), postings.end(),
        [](const auto* a, const auto* b) { return a->size() < b->size(); });

    std::vector<uint32_t> result;
    result.reserve(postings[0]->size());
    result.assign(postings[0]->begin(), postings[0]->end());

    for (size_t i = 1; i < postings.size() && !result.empty(); i++) {
        const auto& other = *postings[i];
        std::vector<uint32_t> isect;
        isect.reserve(std::min(result.size(), other.size()));
        std::set_intersection(result.begin(), result.end(),
                              other.begin(), other.end(),
                              std::back_inserter(isect));
        result = std::move(isect);
    }
    return result;
}

std::vector<uint32_t> SearchEngine::unionPostingListsMulti(
    const std::unordered_map<Trigram, std::vector<uint32_t>>& index,
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
        for (Trigram t : scratch) {
            index[t].push_back(i);
        }
    }
    for (auto& [trigram, list] : index) {
        std::sort(list.begin(), list.end());
    }
    return index;
}

void SearchEngine::buildTrigramIndex() {
    nameTrigramIndex_ = buildTrigramIndexFromData(types_, namePool_);
}

void SearchEngine::addTrigramsForRecord(uint32_t idx, const char* data, uint16_t len) {
    // P-2 fix: sort+unique instead of unordered_set
    std::vector<Trigram> trigrams;
    ContentIndex::extractTrigramsInto(std::string_view(data, len), trigrams);
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());

    for (Trigram t : trigrams) {
        auto& list = nameTrigramIndex_[t];
        // Insert in sorted position to maintain sorted posting lists
        auto pos = std::lower_bound(list.begin(), list.end(), idx);
        list.insert(pos, idx);
    }
}

void SearchEngine::removeTrigramsForRecord(uint32_t idx) {
    if (idx >= namePool_.entryCount() || !namePool_.isLive(idx)) return;
    // Recompute trigrams from namePool_ instead of storing per-record lists
    std::vector<Trigram> trigrams;
    ContentIndex::extractTrigramsInto(
        std::string_view(namePool_.data(idx), namePool_.length(idx)), trigrams);
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
    for (Trigram t : trigrams) {
        auto it = nameTrigramIndex_.find(t);
        if (it != nameTrigramIndex_.end()) {
            auto& list = it->second;
            // Binary search in sorted list
            auto pos = std::lower_bound(list.begin(), list.end(), idx);
            if (pos != list.end() && *pos == idx) {
                list.erase(pos);
            }
            if (list.empty()) {
                nameTrigramIndex_.erase(it);
            }
        }
    }
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
    extensionIndex_ = buildExtensionIndexFromData(types_, namePool_);
}

void SearchEngine::addExtensionForRecord(uint32_t idx) {
    if (idx >= namePool_.entryCount() || !namePool_.isLive(idx)) return;
    std::string_view ext = extractExtFromName(namePool_.data(idx), namePool_.length(idx));
    if (ext.empty()) return;
    auto& list = extensionIndex_[std::string(ext)];
    auto pos = std::lower_bound(list.begin(), list.end(), idx);
    list.insert(pos, idx);
}

void SearchEngine::removeExtensionForRecord(uint32_t idx) {
    if (idx >= namePool_.entryCount() || !namePool_.isLive(idx)) return;
    std::string_view ext = extractExtFromName(namePool_.data(idx), namePool_.length(idx));
    if (ext.empty()) return;
    auto it = extensionIndex_.find(std::string(ext));
    if (it != extensionIndex_.end()) {
        auto& list = it->second;
        auto pos = std::lower_bound(list.begin(), list.end(), idx);
        if (pos != list.end() && *pos == idx) {
            list.erase(pos);
        }
        if (list.empty()) {
            extensionIndex_.erase(it);
        }
    }
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
        for (Trigram t : scratch) {
            index[t].push_back(pi);
        }
    }
    for (auto& [_, list] : index) {
        std::sort(list.begin(), list.end());
    }
    return index;
}

std::vector<std::vector<uint32_t>>
SearchEngine::buildPathIdxToRecordsFromData(const std::vector<uint8_t>& types,
                                            const std::vector<uint32_t>& pathIndices,
                                            uint32_t pathPoolSize) {
    std::vector<std::vector<uint32_t>> mapping(pathPoolSize);
    for (size_t i = 0; i < types.size(); i++) {
        if (types[i] == 0) continue;
        uint32_t pi = pathIndices[i];
        if (pi < mapping.size()) {
            mapping[pi].push_back(static_cast<uint32_t>(i));
        }
    }
    for (auto& list : mapping) {
        std::sort(list.begin(), list.end());
    }
    return mapping;
}

void SearchEngine::buildPathTrigramIndex() {
    pathTrigramIndex_ = buildPathTrigramIndexFromData(lowerPathPool_);
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

void SearchEngine::ensurePathTrigramsForPathIdx(uint32_t pathIdx) {
    // Check if this pathIdx already has entries in pathTrigramIndex_
    std::vector<Trigram> trigrams;
    ContentIndex::extractTrigramsInto(lowerPathPool_.view(pathIdx), trigrams);
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
    for (Trigram t : trigrams) {
        auto& list = pathTrigramIndex_[t];
        auto pos = std::lower_bound(list.begin(), list.end(), pathIdx);
        if (pos == list.end() || *pos != pathIdx) {
            list.insert(pos, pathIdx);
        }
    }
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
    return index;
}
