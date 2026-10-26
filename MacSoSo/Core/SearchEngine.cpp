#include "SearchEngine.h"
#include "StringUtils.h"
#include "IndexWAL.h"
#include "Logger.h"
#include <algorithm>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstring>

std::string SearchEngine::makeFullPath(std::string_view path, std::string_view name) {
    if (path.empty() || path.back() == '/') {
        std::string result;
        result.reserve(path.size() + name.size());
        result.append(path);
        result.append(name);
        return result;
    }
    std::string result;
    result.reserve(path.size() + 1 + name.size());
    result.append(path);
    result += '/';
    result.append(name);
    return result;
}

uint32_t SearchEngine::internPath(const std::string& path) {
    uint32_t existing = findPathIdxUnlocked(path);
    if (existing != PathIndexTable::kNotFound) return existing;
    uint32_t pIdx = pathPool_.append(path);
    lowerPathPool_.append(me::toLower(path));
    insertPathLookupUnlocked(pIdx);
    return pIdx;
}

uint32_t SearchEngine::findPathIdxUnlocked(std::string_view path) const {
    return pathLookup_.get(PathIndexTable::hashPath(path), [&](uint32_t pIdx) {
        return pIdx < pathPool_.entryCount() && pathPool_.view(pIdx) == path;
    });
}

uint32_t SearchEngine::findLowerPathIdxUnlocked(std::string_view loweredPath) const {
    return lowerPathLookup_.get(PathIndexTable::hashPath(loweredPath), [&](uint32_t pIdx) {
        return pIdx < lowerPathPool_.entryCount() && lowerPathPool_.view(pIdx) == loweredPath;
    });
}

void SearchEngine::insertPathLookupUnlocked(uint32_t pIdx) {
    std::string_view path = pathPool_.view(pIdx);
    std::string_view lowerPath = lowerPathPool_.view(pIdx);
    pathLookup_.put(PathIndexTable::hashPath(path), pIdx, [&](uint32_t other) {
        return other < pathPool_.entryCount() && pathPool_.view(other) == path;
    });
    lowerPathLookup_.put(PathIndexTable::hashPath(lowerPath), pIdx, [&](uint32_t other) {
        return other < lowerPathPool_.entryCount() && lowerPathPool_.view(other) == lowerPath;
    });
}

void SearchEngine::rebuildPathLookupsUnlocked() {
    const uint32_t pathCount = pathPool_.entryCount();
    pathLookup_.clear();
    lowerPathLookup_.clear();
    pathLookup_.reserve(pathCount);
    lowerPathLookup_.reserve(pathCount);
    for (uint32_t i = 0; i < pathCount; i++) {
        if (!pathPool_.isLive(i)) continue;
        insertPathLookupUnlocked(i);
    }
}

namespace {
/// Does `loweredFull` equal the record's directory + '/' + name, using the same
/// joining rule as SearchEngine::makeFullPath? Reading the two halves straight
/// out of the pools keeps this allocation-free.
bool loweredPathMatches(std::string_view loweredFull,
                        std::string_view dir, std::string_view name) {
    if (name.empty()) return false;  // tombstoned record
    if (!dir.empty() && dir.back() != '/') {
        if (loweredFull.size() != dir.size() + 1 + name.size()) return false;
        if (loweredFull.substr(0, dir.size()) != dir) return false;
        if (loweredFull[dir.size()] != '/') return false;
        return loweredFull.substr(dir.size() + 1) == name;
    }
    if (loweredFull.size() != dir.size() + name.size()) return false;
    return loweredFull.substr(0, dir.size()) == dir &&
           loweredFull.substr(dir.size()) == name;
}
}  // namespace

uint32_t SearchEngine::findRecordByLoweredPathUnlocked(std::string_view loweredFull) const {
    const uint64_t hash = PathIndexTable::hashPath(loweredFull);
    return pathIndex_.get(hash, [&](uint32_t idx) {
        if (idx >= pathIndices_.size()) return false;
        return loweredPathMatches(loweredFull,
                                  lowerPathPool_.view(pathIndices_[idx]),
                                  namePool_.view(idx));
    });
}

void SearchEngine::indexRecordUnlocked(std::string_view loweredFull, uint32_t idx) {
    const uint64_t hash = PathIndexTable::hashPath(loweredFull);
    pathIndex_.put(hash, idx, [&](uint32_t existingIdx) {
        if (existingIdx >= pathIndices_.size()) return false;
        return loweredPathMatches(loweredFull,
                                  lowerPathPool_.view(pathIndices_[existingIdx]),
                                  namePool_.view(existingIdx));
    });
}

bool SearchEngine::unindexRecordUnlocked(std::string_view loweredFull) {
    const uint64_t hash = PathIndexTable::hashPath(loweredFull);
    return pathIndex_.erase(hash, [&](uint32_t idx) {
        if (idx >= pathIndices_.size()) return false;
        return loweredPathMatches(loweredFull,
                                  lowerPathPool_.view(pathIndices_[idx]),
                                  namePool_.view(idx));
    });
}

uint64_t SearchEngine::loweredPathHashUnlocked(uint32_t idx) const {
    std::string_view dir = lowerPathPool_.view(pathIndices_[idx]);
    std::string_view name = namePool_.view(idx);
    uint64_t hash = PathIndexTable::hashSeed();
    hash = PathIndexTable::hashFeed(hash, dir);
    if (!dir.empty() && dir.back() != '/') hash = PathIndexTable::hashFeed(hash, "/");
    return PathIndexTable::hashFeed(hash, name);
}

void SearchEngine::indexRecordByIdxUnlocked(uint32_t idx) {
    pathIndex_.put(loweredPathHashUnlocked(idx), idx,
                   [&](uint32_t existingIdx) { return sameLoweredPathUnlocked(idx, existingIdx); });
}

bool SearchEngine::sameLoweredPathUnlocked(uint32_t a, uint32_t b) const {
    if (a >= pathIndices_.size() || b >= pathIndices_.size()) return false;
    return lowerPathPool_.view(pathIndices_[a]) == lowerPathPool_.view(pathIndices_[b]) &&
           namePool_.view(a) == namePool_.view(b);
}

bool SearchEngine::unindexRecordByIdxUnlocked(uint32_t idx) {
    return pathIndex_.erase(loweredPathHashUnlocked(idx),
                            [&](uint32_t existingIdx) { return existingIdx == idx; });
}

void SearchEngine::buildPathIndexUnlocked(size_t n) {
    pathIndex_.clear();
    pathIndex_.reserve(static_cast<uint32_t>(n));

    unsigned numThreads = std::thread::hardware_concurrency();
    if (numThreads < 1) numThreads = 1;
    if (numThreads > 32) numThreads = 32;
    const size_t chunkSize = (n + numThreads - 1) / numThreads;
    std::vector<uint64_t> hashes(n, 0);
    {
        std::vector<std::thread> pathThreads;
        pathThreads.reserve(numThreads);
        for (unsigned t = 0; t < numThreads; t++) {
            size_t start = t * chunkSize;
            size_t end = std::min(start + chunkSize, n);
            if (start >= end) break;
            pathThreads.emplace_back([this, &hashes, start, end] {
                for (size_t i = start; i < end; i++) {
                    uint32_t idx = static_cast<uint32_t>(i);
                    if (types_[idx] == 0) continue;
                    hashes[i] = loweredPathHashUnlocked(idx);
                }
            });
        }
        for (auto& th : pathThreads) th.join();
    }
    for (size_t i = 0; i < n; i++) {
        if (types_[i] == 0) continue;
        pathIndex_.put(hashes[i], static_cast<uint32_t>(i),
                       [&](uint32_t existingIdx) { return sameLoweredPathUnlocked(i, existingIdx); });
    }
}

std::vector<uint32_t> SearchEngine::collectSubtreeRecordsUnlocked(
    const std::string& lowerPrefix) const {
    std::vector<uint32_t> matched;

    // pathIdxToRecords_ is only complete once Phase 2 has swapped the trigram
    // indices in; before that fall back to scanning every record.
    if (pathIdxToRecords_.size() < pathPool_.entryCount() || phase2Pending_.load()) {
        matched.reserve(liveCount_.load(std::memory_order_relaxed) / 8);
        for (uint32_t i = 0; i < types_.size(); i++) {
            if (types_[i] == 0) continue;
            std::string_view dir = lowerPathPool_.view(pathIndices_[i]);
            std::string_view name = namePool_.view(i);
            if (dir.size() >= lowerPrefix.size()) {
                if (dir.substr(0, lowerPrefix.size()) != lowerPrefix) continue;
                if (dir.size() > lowerPrefix.size() && dir[lowerPrefix.size()] != '/') continue;
            } else {
                // Directory is above the prefix: only the record whose full path
                // is exactly the prefix belongs to the subtree.
                if (!loweredPathMatches(lowerPrefix, dir, name)) continue;
            }
            matched.push_back(i);
        }
        return matched;
    }

    for (uint32_t pi = 0; pi < lowerPathPool_.entryCount(); pi++) {
        if (!lowerPathPool_.isLive(pi)) continue;
        std::string_view dir = lowerPathPool_.view(pi);
        if (dir.size() < lowerPrefix.size()) continue;
        if (dir.substr(0, lowerPrefix.size()) != lowerPrefix) continue;
        if (dir.size() > lowerPrefix.size() && dir[lowerPrefix.size()] != '/') continue;
        if (pi >= pathIdxToRecords_.size()) continue;
        const auto& children = pathIdxToRecords_[pi];
        matched.insert(matched.end(), children.begin(), children.end());
    }

    // Plus the record whose full path is the prefix itself, when it exists.
    uint32_t self = findRecordByLoweredPathUnlocked(lowerPrefix);
    if (self != PathIndexTable::kNotFound) matched.push_back(self);

    std::sort(matched.begin(), matched.end());
    matched.erase(std::unique(matched.begin(), matched.end()), matched.end());
    return matched;
}

void SearchEngine::tombstoneAt(uint32_t idx) {
    types_[idx] = 0;
    markPageDirty(idx);
    sizes_[idx] = 0;
    modTimes_[idx] = 0;
    if (idx < inodes_.size()) inodes_[idx] = 0;
    if (idx < devIds_.size()) devIds_[idx] = 0;
    namePool_.tombstone(idx);
    origNamePool_.tombstone(idx);
}

void SearchEngine::pushRecord(FileRecord&& rec) {
    types_.push_back(rec.type);
    sizes_.push_back(rec.size);
    modTimes_.push_back(static_cast<int64_t>(rec.modTime));
    inodes_.push_back(rec.inode);
    devIds_.push_back(rec.devId);
}

void SearchEngine::loadRecords(std::vector<FileRecord>&& records) {
    std::unique_lock lock(mutex_);
    invalidateQueryCache();   // bulk rebuild invalidates all cached answers

    const size_t n = records.size();

    // Build SoA columns from incoming records
    types_.resize(n);
    sizes_.resize(n);
    modTimes_.resize(n);
    inodes_.resize(n);
    devIds_.resize(n);
    for (size_t i = 0; i < n; i++) {
        types_[i] = records[i].type;
        sizes_[i] = records[i].size;
        modTimes_[i] = static_cast<int64_t>(records[i].modTime);
        inodes_[i] = records[i].inode;
        devIds_[i] = records[i].devId;
    }

    // Build origNamePool_ from original-case names (for v6 persistence)
    {
        std::vector<std::string> origNames(n);
        for (size_t i = 0; i < n; i++) {
            origNames[i] = records[i].name;
        }
        origNamePool_.loadBulk(origNames);
    }

    // Pre-compute lowercase names into a temporary vector (parallel),
    // then bulk-load into namePool_ (single-threaded append)
    std::vector<std::string> tempLowerNames(n);
    {
        unsigned numThreads = std::thread::hardware_concurrency();
        if (numThreads < 1) numThreads = 1;
        if (numThreads > 32) numThreads = 32;
        size_t chunkSize = (n + numThreads - 1) / numThreads;
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (unsigned t = 0; t < numThreads; t++) {
            size_t start = t * chunkSize;
            size_t end = std::min(start + chunkSize, n);
            if (start >= end) break;
            threads.emplace_back([&records, &tempLowerNames, start, end] {
                for (size_t i = start; i < end; i++) {
                    tempLowerNames[i] = me::toLower(records[i].name);
                }
            });
        }
        for (auto& th : threads) th.join();
    }
    namePool_.loadBulk(tempLowerNames);

    // Path deduplication: intern paths into pathPool_ + lowerPathPool_ + pathLookup_
    pathPool_.clear();
    lowerPathPool_.clear();
    pathLookup_.clear();
    lowerPathLookup_.clear();
    pathIndices_.resize(n);
    for (size_t i = 0; i < n; i++) {
        pathIndices_[i] = internPath(records[i].path);
    }

    // Build pathIndex_ (lowercase full path -> record index)
    buildPathIndexUnlocked(n);

    // Tombstone orphaned duplicates: records not in pathIndex_ as winners
    std::vector<bool> isWinner(n, false);
    pathIndex_.forEach([&](uint64_t, uint32_t idx) { isWinner[idx] = true; });
    uint32_t actualLive = 0;
    for (size_t i = 0; i < n; i++) {
        if (types_[i] == 0) continue;
        if (isWinner[i]) {
            actualLive++;
        } else {
            tombstoneAt(static_cast<uint32_t>(i));
        }
    }

    // Build trigram index for fast filename search
    buildTrigramIndex();
    // Build path trigram index for fast path-only search
    buildPathTrigramIndex();
    rebuildPathIdxToRecords();
    // Build extension index for fast ext: filter queries
    buildExtensionIndex();
    rebuildRecentCache();

    liveCount_.store(actualLive, std::memory_order_relaxed);

    // Initialize dirty page bitmap (no pages dirty after initial load)
    uint32_t pageCount = (static_cast<uint32_t>(n) + kRecordsPerPage - 1) / kRecordsPerPage;
    dirtyPages_.assign(pageCount, false);
    fullRewriteNeeded_.store(false, std::memory_order_relaxed);
}

void SearchEngine::loadRecordsV5(std::vector<FileRecord>&& records,
                                  std::vector<std::string>&& lowerNames,
                                  std::vector<uint32_t>&& pathIndices,
                                  StringPool&& pathDict,
                                  StringPool&& lowerPathDict) {
    std::unique_lock lock(mutex_);

    const size_t n = records.size();

    // Build SoA columns from incoming records
    types_.resize(n);
    sizes_.resize(n);
    modTimes_.resize(n);
    inodes_.resize(n);
    devIds_.resize(n);
    for (size_t i = 0; i < n; i++) {
        types_[i] = records[i].type;
        sizes_[i] = records[i].size;
        modTimes_[i] = static_cast<int64_t>(records[i].modTime);
        inodes_[i] = records[i].inode;
        devIds_[i] = records[i].devId;
    }

    // Build origNamePool_ from original-case names (for v6 persistence)
    {
        std::vector<std::string> origNames(n);
        for (size_t i = 0; i < n; i++) {
            origNames[i] = records[i].name;
        }
        origNamePool_.loadBulk(origNames);
    }

    // Install pre-lowered names directly into namePool_ (skip parallel toLower)
    namePool_.loadBulk(lowerNames);

    // Install pre-built path dictionaries (skip path dedup + internPath loop)
    pathPool_ = std::move(pathDict);
    lowerPathPool_ = std::move(lowerPathDict);
    pathIndices_ = std::move(pathIndices);

    // Rebuild pathLookup_ and lowerPathLookup_ from pathPool_ entries
    rebuildPathLookupsUnlocked();

    // Build pathIndex_ (lowercase full path -> record index)
    buildPathIndexUnlocked(n);

    // Tombstone orphaned duplicates
    std::vector<bool> isWinner(n, false);
    pathIndex_.forEach([&](uint64_t, uint32_t idx) { isWinner[idx] = true; });
    uint32_t actualLive = 0;
    for (size_t i = 0; i < n; i++) {
        if (types_[i] == 0) continue;
        if (isWinner[i]) {
            actualLive++;
        } else {
            tombstoneAt(static_cast<uint32_t>(i));
        }
    }

    // Build trigram index for fast filename search
    buildTrigramIndex();
    buildPathTrigramIndex();
    rebuildPathIdxToRecords();
    buildExtensionIndex();
    rebuildRecentCache();

    liveCount_.store(actualLive, std::memory_order_relaxed);

    // Initialize dirty page bitmap
    uint32_t pageCount = (static_cast<uint32_t>(n) + kRecordsPerPage - 1) / kRecordsPerPage;
    dirtyPages_.assign(pageCount, false);
    fullRewriteNeeded_.store(false, std::memory_order_relaxed);
}

FileRecord SearchEngine::getRecord(uint32_t index) const {
    std::shared_lock lock(mutex_);
    if (index >= types_.size()) return {};
    FileRecord r;
    r.type = types_[index];
    if (r.type == 0) return r;
    r.name = origNamePool_.str(index);
    r.path = pathPool_.str(pathIndices_[index]);
    r.size = sizes_[index];
    r.modTime = static_cast<time_t>(modTimes_[index]);
    r.inode = inodes_[index];
    r.devId = devIds_[index];
    return r;
}

uint32_t SearchEngine::recordCount() const {
    std::shared_lock lock(mutex_);
    return static_cast<uint32_t>(types_.size());
}

uint32_t SearchEngine::addRecord(FileRecord&& record) {
    std::unique_lock lock(mutex_);
    invalidateQueryCache();   // addition changes query answers

    uint32_t idx = static_cast<uint32_t>(types_.size());
    std::string fullPath = makeFullPath(record.path, record.name);
    std::string lowerFull = me::toLower(fullPath);
    std::string lower = me::toLower(record.name);

    if (wal_) wal_->append(WALOp::Add, fullPath, record);

    // Tombstone existing record at same path to prevent orphaned duplicates
    uint32_t existIdx = findRecordByLoweredPathUnlocked(lowerFull);
    if (existIdx != PathIndexTable::kNotFound) {
        time_t oldModTime = static_cast<time_t>(modTimes_[existIdx]);
        removeTrigramsForRecord(existIdx);
        removePathTrigramsForRecord(existIdx);
        unindexRecordUnlocked(lowerFull);
        tombstoneAt(existIdx);
        liveCount_.fetch_sub(1, std::memory_order_relaxed);
        removeFromRecentCache(existIdx, oldModTime);
    }

    // Intern path before moving record
    uint32_t pIdx = internPath(record.path);
    if (pIdx >= pathIdxToRecords_.size()) {
        ensurePathTrigramsForPathIdx(pIdx);
    }
    record.path.clear();
    record.path.shrink_to_fit();

    origNamePool_.append(record.name);
    pushRecord(std::move(record));
    uint32_t nameIdx = namePool_.append(lower);
    (void)nameIdx; // nameIdx == idx since namePool_ grows in lockstep
    pathIndices_.push_back(pIdx);
    indexRecordUnlocked(lowerFull, idx);

    // Update trigram index
    addTrigramsForRecord(idx, namePool_.data(idx), namePool_.length(idx));
    addPathTrigramsForRecord(idx);
    addExtensionForRecord(idx);

    // Dirty page tracking
    if (idx / kRecordsPerPage >= dirtyPages_.size()) {
        dirtyPages_.resize(idx / kRecordsPerPage + 1, false);
    }
    markPageDirty(idx);

    liveCount_.fetch_add(1, std::memory_order_relaxed);
    addToRecentCache(idx, static_cast<time_t>(modTimes_[idx]));

    return idx;
}

bool SearchEngine::removeByPathUnlocked(const std::string& fullPath) {
    std::string lowered = me::toLower(fullPath);
    uint32_t idx = findRecordByLoweredPathUnlocked(lowered);
    if (idx == PathIndexTable::kNotFound) return false;

    if (wal_) wal_->append(WALOp::Remove, fullPath);

    time_t oldModTime = static_cast<time_t>(modTimes_[idx]);
    removeTrigramsForRecord(idx);
    removePathTrigramsForRecord(idx);
    removeExtensionForRecord(idx);
    unindexRecordUnlocked(lowered);
    tombstoneAt(idx);

    liveCount_.fetch_sub(1, std::memory_order_relaxed);
    removeFromRecentCache(idx, oldModTime);

    return true;
}

bool SearchEngine::removeByPath(const std::string& fullPath) {
    std::unique_lock lock(mutex_);
    invalidateQueryCache();   // removal changes query answers
    return removeByPathUnlocked(fullPath);
}

uint32_t SearchEngine::removeByPathPrefix(const std::string& pathPrefix) {
    std::shared_ptr<IndexWAL> wal;
    uint32_t removed = 0;
    {
        std::unique_lock lock(mutex_);
        invalidateQueryCache();   // bulk prefix removal changes query answers

        std::string lowerPrefix = me::toLower(pathPrefix);
        const std::vector<uint32_t> matched = collectSubtreeRecordsUnlocked(lowerPrefix);
        removeRecordsFromIndexesBatch(matched);
        for (uint32_t idx : matched) {
            if (idx >= types_.size() || types_[idx] == 0) continue;

            // H-4: Write WAL entry for each removed path
            if (wal_) {
                std::string fullPath = makeFullPath(pathPool_.str(pathIndices_[idx]), origNamePool_.str(idx));
                wal_->append(WALOp::Remove, fullPath);
            }

            time_t oldModTime = static_cast<time_t>(modTimes_[idx]);
            unindexRecordByIdxUnlocked(idx);
            tombstoneAt(idx);
            liveCount_.fetch_sub(1, std::memory_order_relaxed);
            removeFromRecentCache(idx, oldModTime);
            removed++;
        }
        wal = wal_;
    }
    if (wal) wal->flushPending();
    return removed;
}

uint32_t SearchEngine::batchRescanPrefix(const std::string& pathPrefix,
                                         std::vector<FileRecord>&& freshRecords) {
    std::shared_ptr<IndexWAL> wal;
    uint32_t removed = 0;
    {
        std::unique_lock lock(mutex_);

        // ── Phase 1: Tombstone old records matching prefix ──
        // Postings are filtered in one pass over the affected lists, then each
        // record is unindexed and tombstoned.
        std::string lowerPrefix = me::toLower(pathPrefix);
        const std::vector<uint32_t> stale = collectSubtreeRecordsUnlocked(lowerPrefix);
        removeRecordsFromIndexesBatch(stale);
        for (uint32_t idx : stale) {
            if (idx >= types_.size() || types_[idx] == 0) continue;

            // Write WAL Remove entry for each removed path
            if (wal_) {
                std::string fullPath = makeFullPath(pathPool_.str(pathIndices_[idx]), origNamePool_.str(idx));
                wal_->append(WALOp::Remove, fullPath);
            }

            unindexRecordByIdxUnlocked(idx);
            tombstoneAt(idx);
            liveCount_.fetch_sub(1, std::memory_order_relaxed);
            removed++;
        }

        // ── Phase 2: Add fresh records with incremental trigram insertion ──
        for (auto& record : freshRecords) {
            uint32_t newIdx = static_cast<uint32_t>(types_.size());
            std::string fullPath = makeFullPath(record.path, record.name);
            std::string lower = me::toLower(record.name);

            if (wal_) wal_->append(WALOp::Update, fullPath, record);

            uint32_t pIdx = internPath(record.path);
            if (pIdx >= pathIdxToRecords_.size()) {
                ensurePathTrigramsForPathIdx(pIdx);
            }
            record.path.clear();
            record.path.shrink_to_fit();

            origNamePool_.append(record.name);
            pushRecord(std::move(record));
            namePool_.append(lower);
            pathIndices_.push_back(pIdx);
            indexRecordByIdxUnlocked(newIdx);
            addTrigramsForRecord(newIdx, namePool_.data(newIdx), namePool_.length(newIdx));
            addPathTrigramsForRecord(newIdx);
            addExtensionForRecord(newIdx);
            if (newIdx / kRecordsPerPage >= dirtyPages_.size()) {
                dirtyPages_.resize(newIdx / kRecordsPerPage + 1, false);
            }
            markPageDirty(newIdx);
            liveCount_.fetch_add(1, std::memory_order_relaxed);
        }

        // ── Phase 3: Rebuild recent cache ──
        rebuildRecentCache();
        wal = wal_;
    }
    if (wal) wal->flushPending();
    return removed;
}

void SearchEngine::updateByPathUnlocked(const std::string& fullPath, FileRecord&& updated) {
    if (wal_) wal_->append(WALOp::Update, fullPath, updated);

    // Remove old record if exists (case-insensitive lookup)
    std::string lowered = me::toLower(fullPath);
    uint32_t oldIdx = findRecordByLoweredPathUnlocked(lowered);
    if (oldIdx != PathIndexTable::kNotFound) {
        time_t oldModTime = static_cast<time_t>(modTimes_[oldIdx]);
        // Clean up trigram index (must happen before tombstoning namePool_)
        removeTrigramsForRecord(oldIdx);
        removePathTrigramsForRecord(oldIdx);
        removeExtensionForRecord(oldIdx);
        unindexRecordUnlocked(lowered);
        tombstoneAt(oldIdx);
        liveCount_.fetch_sub(1, std::memory_order_relaxed);
        removeFromRecentCache(oldIdx, oldModTime);
    }

    // Add new record
    uint32_t newIdx = static_cast<uint32_t>(types_.size());
    std::string lower = me::toLower(updated.name);

    // Intern path before moving record
    uint32_t pIdx = internPath(updated.path);
    if (pIdx >= pathIdxToRecords_.size()) {
        ensurePathTrigramsForPathIdx(pIdx);
    }
    updated.path.clear();
    updated.path.shrink_to_fit();

    origNamePool_.append(updated.name);
    pushRecord(std::move(updated));
    namePool_.append(lower);
    pathIndices_.push_back(pIdx);
    indexRecordByIdxUnlocked(newIdx);
    addTrigramsForRecord(newIdx, namePool_.data(newIdx), namePool_.length(newIdx));
    addPathTrigramsForRecord(newIdx);
    addExtensionForRecord(newIdx);
    if (newIdx / kRecordsPerPage >= dirtyPages_.size()) {
        dirtyPages_.resize(newIdx / kRecordsPerPage + 1, false);
    }
    markPageDirty(newIdx);
    liveCount_.fetch_add(1, std::memory_order_relaxed);
    addToRecentCache(newIdx, static_cast<time_t>(modTimes_[newIdx]));
}

void SearchEngine::updateByPath(const std::string& fullPath, FileRecord&& updated) {
    std::unique_lock lock(mutex_);
    invalidateQueryCache();   // update changes query answers
    updateByPathUnlocked(fullPath, std::move(updated));
}

void SearchEngine::batchMutate(std::vector<MutationOp>&& ops) {
    if (ops.empty()) return;
    std::shared_ptr<IndexWAL> wal;
    {
        std::unique_lock lock(mutex_);
        invalidateQueryCache();   // any index change invalidates cached answers
        for (auto& op : ops) {
            if (op.type == MutationOp::REMOVE) {
                removeByPathUnlocked(op.path);
            } else {
                updateByPathUnlocked(op.path, std::move(op.record));
            }
        }
        wal = wal_;
    }
    // Durability is handed back to the WAL once the batch is done, outside the
    // index lock: a write() syscall per record here stalled every query behind
    // the FSEvents writer.
    if (wal) wal->flushPending();
}

std::unordered_map<uint32_t, uint32_t> SearchEngine::compactRecords() {
    // ── Phase 1: Snapshot under shared_lock ──
    // Queries continue unblocked during snapshot copy.
    invalidateQueryCache();   // record remapping invalidates cached answers
    std::vector<uint8_t> snapTypes;
    std::vector<uint64_t> snapSizes;
    std::vector<int64_t> snapModTimes;
    std::vector<uint64_t> snapInodes;
    std::vector<int32_t> snapDevIds;
    StringPool snapNamePool;
    StringPool snapOrigNamePool;
    std::vector<uint32_t> snapPathIndices;
    StringPool snapPathPool;
    std::vector<bool> snapWinners;
    uint32_t snapSize;
    {
        std::shared_lock lock(mutex_);
        uint32_t live = liveCount_.load(std::memory_order_relaxed);
        if (live == types_.size()) return {}; // nothing to compact
        snapTypes = types_;
        snapSizes = sizes_;
        snapModTimes = modTimes_;
        snapInodes = inodes_;
        snapDevIds = devIds_;
        snapNamePool = namePool_;
        snapOrigNamePool = origNamePool_;
        snapPathIndices = pathIndices_;
        snapPathPool = pathPool_;
        snapSize = static_cast<uint32_t>(types_.size());
        // The table's values are exactly the duplicate winners, so membership can
        // be checked from a bitmap instead of carrying a path map along.
        snapWinners.assign(snapSize, false);
        pathIndex_.forEach([&](uint64_t, uint32_t idx) {
            if (idx < snapSize) snapWinners[idx] = true;
        });
    }

    LOG_INFO("SearchEngine", "COW compaction Phase 2: building compacted data from "
             << snapSize << " records");

    // ── Phase 2: Build compacted data (no lock held) ──
    // Mutations (addRecord/removeByPath/updateByPath/batchRescanPrefix) continue
    // on the live data; they will be replayed in Phase 3.
    std::unordered_map<uint32_t, uint32_t> remap;
    remap.reserve(snapSize);

    std::vector<uint8_t> cdTypes;
    std::vector<uint64_t> cdSizes;
    std::vector<int64_t> cdModTimes;
    std::vector<uint64_t> cdInodes;
    std::vector<int32_t> cdDevIds;
    cdTypes.reserve(snapSize);
    cdSizes.reserve(snapSize);
    cdModTimes.reserve(snapSize);
    cdInodes.reserve(snapSize);
    cdDevIds.reserve(snapSize);
    StringPool cdNamePool;
    StringPool cdOrigNamePool;
    std::vector<uint32_t> cdPathIndices;
    cdPathIndices.reserve(snapSize);
    StringPool cdPathPool;
    StringPool cdLowerPathPool;
    PathIndexTable cdPathLookup;
    PathIndexTable cdLowerPathLookup;
    PathIndexTable cdPathIndex;
    cdPathIndex.reserve(snapSize);

    for (size_t i = 0; i < snapSize; i++) {
        if (snapTypes[i] == 0) continue;
        // Skip orphaned duplicates: live records not referenced by pathIndex_
        if (!snapWinners[i]) continue;
        uint32_t newIdx = static_cast<uint32_t>(cdTypes.size());
        remap[static_cast<uint32_t>(i)] = newIdx;
        // Intern path into compacted pool (both original and lowered)
        std::string_view origPath = snapPathPool.view(snapPathIndices[i]);
        const uint64_t origHash = PathIndexTable::hashPath(origPath);
        uint32_t newPIdx = cdPathLookup.get(origHash, [&](uint32_t other) {
            return other < cdPathPool.entryCount() && cdPathPool.view(other) == origPath;
        });
        if (newPIdx == PathIndexTable::kNotFound) {
            newPIdx = cdPathPool.append(origPath);
            cdLowerPathPool.append(me::toLower(origPath));
            std::string_view lowerPath = cdLowerPathPool.view(newPIdx);
            cdPathLookup.put(origHash, newPIdx, [&](uint32_t other) {
                return other < cdPathPool.entryCount() && cdPathPool.view(other) == origPath;
            });
            cdLowerPathLookup.put(PathIndexTable::hashPath(lowerPath), newPIdx, [&](uint32_t other) {
                return other < cdLowerPathPool.entryCount() && cdLowerPathPool.view(other) == lowerPath;
            });
        }
        std::string_view cdLowerDir = cdLowerPathPool.view(newPIdx);
        std::string_view cdLowerName = snapNamePool.view(i);
        uint64_t pathHash = PathIndexTable::hashSeed();
        pathHash = PathIndexTable::hashFeed(pathHash, cdLowerDir);
        if (!cdLowerDir.empty() && cdLowerDir.back() != '/') {
            pathHash = PathIndexTable::hashFeed(pathHash, "/");
        }
        pathHash = PathIndexTable::hashFeed(pathHash, cdLowerName);
        cdPathIndex.put(pathHash, newIdx, [&](uint32_t existingIdx) {
            std::string_view otherDir = cdLowerPathPool.view(cdPathIndices[existingIdx]);
            return otherDir == cdLowerDir && cdNamePool.view(existingIdx) == cdLowerName;
        });
        // Copy name from snapshot pool into compacted pool
        cdOrigNamePool.append(snapOrigNamePool.data(i), snapOrigNamePool.length(i));
        cdNamePool.append(snapNamePool.data(i), snapNamePool.length(i));
        cdPathIndices.push_back(newPIdx);
        cdTypes.push_back(snapTypes[i]);
        cdSizes.push_back(snapSizes[i]);
        cdModTimes.push_back(snapModTimes[i]);
        cdInodes.push_back(snapInodes[i]);
        cdDevIds.push_back(snapDevIds[i]);
    }
    uint32_t cdLiveCount = static_cast<uint32_t>(cdTypes.size());

    // Build trigram index, path trigram index, extension index, and recent cache outside any lock
    auto cdTrigramIndex = buildTrigramIndexFromData(cdTypes, cdNamePool);
    auto cdPathTrigramIndex = buildPathTrigramIndexFromData(cdLowerPathPool);
    auto cdPathIdxToRecords = buildPathIdxToRecordsFromData(cdTypes, cdPathIndices, cdPathPool.entryCount());
    auto cdExtensionIndex = buildExtensionIndexFromData(cdTypes, cdNamePool);
    auto cdRecentCache = buildRecentCacheFromData(cdTypes, cdModTimes, kRecentCacheSize);

    // Encoding into the compact arenas happens here, not under the swap lock: it
    // walks every posting list, and doing it while readers are blocked would show
    // up as a multi-hundred-millisecond query spike. `build()` releases each
    // source list as it encodes, so the uncompressed map never coexists with the
    // finished arena.
    PostingIndex<Trigram> nameTrigramIndex;
    nameTrigramIndex.build(std::move(cdTrigramIndex));
    PostingIndex<Trigram> pathTrigramIndex;
    pathTrigramIndex.build(std::move(cdPathTrigramIndex));
    PostingIndex<std::string> extensionIndex;
    extensionIndex.build(std::move(cdExtensionIndex));

    LOG_INFO("SearchEngine", "COW compaction Phase 3: swapping data, compacted "
             << snapSize << " -> " << cdLiveCount << " records");

    // ── Phase 3: Swap + replay under unique_lock ──
    // This lock is held only long enough to move data and replay the small
    // number of mutations that occurred during Phase 2 (~milliseconds).
    {
        std::unique_lock lock(mutex_);

        // Move out current (mutated-during-Phase2) state
        auto oldTypes = std::move(types_);
        auto oldSizes = std::move(sizes_);
        auto oldModTimes = std::move(modTimes_);
        auto oldInodes = std::move(inodes_);
        auto oldDevIds = std::move(devIds_);
        auto oldOrigNamePool = std::move(origNamePool_);
        auto oldNamePool = std::move(namePool_);
        auto oldPathIndices = std::move(pathIndices_);
        auto oldPathPool = std::move(pathPool_);
        auto oldPathLookup = std::move(pathLookup_);
        auto oldLowerPathLookup = std::move(lowerPathLookup_);
        auto oldPathIndex = std::move(pathIndex_);

        // Install compacted data
        types_ = std::move(cdTypes);
        sizes_ = std::move(cdSizes);
        modTimes_ = std::move(cdModTimes);
        inodes_ = std::move(cdInodes);
        devIds_ = std::move(cdDevIds);
        origNamePool_ = std::move(cdOrigNamePool);
        namePool_ = std::move(cdNamePool);
        pathIndices_ = std::move(cdPathIndices);
        pathPool_ = std::move(cdPathPool);
        lowerPathPool_ = std::move(cdLowerPathPool);
        pathLookup_ = std::move(cdPathLookup);
        lowerPathLookup_ = std::move(cdLowerPathLookup);
        pathIndex_ = std::move(cdPathIndex);
        nameTrigramIndex_ = std::move(nameTrigramIndex);
        pathTrigramIndex_ = std::move(pathTrigramIndex);
        pathIdxToRecords_ = std::move(cdPathIdxToRecords);
        extensionIndex_ = std::move(extensionIndex);
        recentCache_ = std::move(cdRecentCache);

        // Replay new records appended during Phase 2
        uint32_t replayedAdds = 0;
        for (size_t i = snapSize; i < oldTypes.size(); i++) {
            if (oldTypes[i] == 0) continue;
            uint32_t newIdx = static_cast<uint32_t>(types_.size());
            std::string origPath = oldPathPool.str(oldPathIndices[i]);
            uint32_t pIdx = internPath(origPath);
            if (pIdx >= pathIdxToRecords_.size()) {
                ensurePathTrigramsForPathIdx(pIdx);
            }
            // Copy name from old pool into current pool
            origNamePool_.append(oldOrigNamePool.data(i), oldOrigNamePool.length(i));
            namePool_.append(oldNamePool.data(i), oldNamePool.length(i));
            pathIndices_.push_back(pIdx);
            indexRecordByIdxUnlocked(newIdx);
            addTrigramsForRecord(newIdx, namePool_.data(newIdx), namePool_.length(newIdx));
            addToRecentCache(newIdx, static_cast<time_t>(oldModTimes[i]));
            // Push SoA columns for replayed record
            types_.push_back(oldTypes[i]);
            sizes_.push_back(oldSizes[i]);
            modTimes_.push_back(oldModTimes[i]);
            inodes_.push_back(oldInodes[i]);
            devIds_.push_back(oldDevIds[i]);
            addPathTrigramsForRecord(newIdx);
            addExtensionForRecord(newIdx);
            cdLiveCount++;
            replayedAdds++;
        }

        // Replay tombstones: compacted records whose path disappeared while the
        // snapshot was being built. Membership is checked against the pre-swap
        // table by hash, which the table already keys on.
        uint32_t replayedDeletes = 0;
        for (const auto& [snapIdx, newIdx] : remap) {
            if (newIdx >= types_.size() || types_[newIdx] == 0) continue;
            const uint64_t pathHash = loweredPathHashUnlocked(newIdx);
            if (oldPathIndex.contains(pathHash)) continue;
            removeTrigramsForRecord(newIdx);
            removePathTrigramsForRecord(newIdx);
            removeExtensionForRecord(newIdx);
            time_t oldMod = static_cast<time_t>(modTimes_[newIdx]);
            tombstoneAt(newIdx);
            pathIndex_.eraseByHash(pathHash);
            removeFromRecentCache(newIdx, oldMod);
            cdLiveCount--;
            replayedDeletes++;
        }

        liveCount_.store(cdLiveCount, std::memory_order_relaxed);
        compactionGen_.fetch_add(1, std::memory_order_relaxed);

        LOG_INFO("SearchEngine", "COW compaction done: replayed " << replayedAdds
                 << " adds, " << replayedDeletes << " deletes, live=" << cdLiveCount);
    }

    fullRewriteNeeded_.store(true, std::memory_order_relaxed);
    phase2Pending_.store(false, std::memory_order_release);

    return remap;
}

void SearchEngine::attachWAL(std::shared_ptr<IndexWAL> wal) {
    std::unique_lock lock(mutex_);
    wal_ = std::move(wal);
}

void SearchEngine::detachWAL() {
    std::unique_lock lock(mutex_);
    wal_.reset();
}

std::vector<uint32_t> SearchEngine::recentIndices(uint32_t count) const {
    std::shared_lock lock(mutex_);
    std::vector<uint32_t> result;
    uint32_t n = std::min(count, static_cast<uint32_t>(recentCache_.size()));
    result.reserve(n);
    auto it = recentCache_.begin();
    for (uint32_t i = 0; i < n; ++i, ++it) {
        result.push_back(it->index);
    }
    return result;
}

std::set<SearchEngine::RecentEntry>
SearchEngine::buildRecentCacheFromData(const std::vector<uint8_t>& types,
                                       const std::vector<int64_t>& modTimes,
                                       uint32_t cacheSize) {
    struct TimePair { time_t modTime; uint32_t index; };
    std::vector<TimePair> pairs;
    pairs.reserve(types.size());
    for (size_t i = 0; i < types.size(); i++) {
        if (types[i] == 0) continue;
        pairs.push_back({static_cast<time_t>(modTimes[i]), static_cast<uint32_t>(i)});
    }
    std::set<RecentEntry> cache;
    size_t k = std::min(pairs.size(), static_cast<size_t>(cacheSize));
    if (k > 0) {
        std::partial_sort(pairs.begin(), pairs.begin() + k, pairs.end(),
                          [](const TimePair& a, const TimePair& b) { return a.modTime > b.modTime; });
        for (size_t i = 0; i < k; i++) {
            cache.insert({pairs[i].modTime, pairs[i].index});
        }
    }
    return cache;
}

void SearchEngine::rebuildRecentCache() {
    recentCache_ = buildRecentCacheFromData(types_, modTimes_, kRecentCacheSize);
}

void SearchEngine::addToRecentCache(uint32_t idx, time_t modTime) {
    recentCache_.insert({modTime, idx});
    if (recentCache_.size() > kRecentCacheSize) {
        recentCache_.erase(std::prev(recentCache_.end()));
    }
}

void SearchEngine::removeFromRecentCache(uint32_t idx, time_t modTime) {
    recentCache_.erase({modTime, idx});
}

uint32_t SearchEngine::indexForPath(const std::string& fullPath) const {
    std::shared_lock lock(mutex_);
    return findRecordByLoweredPathUnlocked(me::toLower(fullPath));
}

namespace {
template <typename T>
size_t vecBytes(const std::vector<T>& v) {
    return v.capacity() * sizeof(T);
}

double MiB(size_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}
}  // namespace

std::string SearchEngine::memoryProfile() const {
    std::shared_lock lock(mutex_);
    return memoryProfileUnlocked();
}

void SearchEngine::trimCapacity() {
    std::unique_lock lock(mutex_);

    // Append-by-doubling leaves up to 2x the needed capacity behind. Reclaim it,
    // but keep a slack window: shrinking to the exact size makes the very next
    // append double the whole array again, so the idle cycle would free and
    // re-consume the same memory forever.
    auto trimVec = [](auto& v, size_t slack) {
        const size_t target = v.size() + std::max(v.size() / 16, slack);
        if (v.capacity() <= target + target / 8) return;
        std::vector<typename std::remove_reference_t<decltype(v)>::value_type> shrunk;
        shrunk.reserve(target);
        shrunk.insert(shrunk.end(), v.begin(), v.end());
        v.swap(shrunk);
    };

    // 4 MiB of slack is ~3 days of appends on a quiet volume, and under 1% of a
    // multi-hundred-megabyte pool.
    origNamePool_.shrinkWithSlack(4 << 20);
    namePool_.shrinkWithSlack(4 << 20);
    pathPool_.shrinkWithSlack(4 << 20);
    lowerPathPool_.shrinkWithSlack(4 << 20);

    trimVec(types_, 65536);
    trimVec(sizes_, 65536);
    trimVec(modTimes_, 65536);
    trimVec(inodes_, 65536);
    trimVec(devIds_, 65536);
    trimVec(pathIndices_, 65536);
    trimVec(dirtyPages_, 4096);
    for (auto& list : pathIdxToRecords_) trimVec(list, 16);

    // Hash tables outlive the entry counts they were sized for after deletions.
    pathIndex_.trim();
    pathLookup_.trim();
    lowerPathLookup_.trim();

    // Post-build mutations queue up in each index's delta layer. Left alone it
    // regrows exactly the per-key vector overhead the compact arena exists to
    // avoid, so fold it back once it stops being negligible.
    auto foldDeltaIfLarge = [&](auto& index, const char* label) {
        const size_t pending = index.deltaPostings();
        const size_t base = index.basePostings();
        if (pending < 262144 && !(base > 0 && pending * 4 >= base)) return;
        const size_t folded = index.foldDelta();
        if (folded) {
            LOG_INFO("SearchEngine", "folded " << folded << " delta postings into " << label);
        }
    };
    foldDeltaIfLarge(nameTrigramIndex_, "name trigrams");
    foldDeltaIfLarge(pathTrigramIndex_, "path trigrams");
    foldDeltaIfLarge(extensionIndex_, "extensions");

    // The inverted indexes hold one byte arena each, so there is no per-list
    // slack left to trim — only the arena's growth headroom and the key tables.
    nameTrigramIndex_.trim();
    pathTrigramIndex_.trim();
    extensionIndex_.trim();
}

std::string SearchEngine::memoryProfileUnlocked() const {
    size_t pools = origNamePool_.bufferCapacity() + origNamePool_.entryCapacityBytes() +
                   namePool_.bufferCapacity() + namePool_.entryCapacityBytes() +
                   pathPool_.bufferCapacity() + pathPool_.entryCapacityBytes() +
                   lowerPathPool_.bufferCapacity() + lowerPathPool_.entryCapacityBytes();
    size_t columns = vecBytes(types_) + vecBytes(sizes_) + vecBytes(modTimes_) +
                     vecBytes(inodes_) + vecBytes(devIds_) + vecBytes(pathIndices_);
    size_t pathLookup = pathLookup_.memoryBytes();
    size_t lowerPathLookup = lowerPathLookup_.memoryBytes();
    size_t pathIndex = pathIndex_.memoryBytes();
    size_t nameTrigrams = nameTrigramIndex_.memoryBytes();
    size_t pathTrigrams = pathTrigramIndex_.memoryBytes();
    size_t pathIdxToRecords = vecBytes(pathIdxToRecords_);
    for (const auto& posting : pathIdxToRecords_) pathIdxToRecords += vecBytes(posting);
    size_t extensions = extensionIndex_.memoryBytes();
    size_t dirty = vecBytes(dirtyPages_);

    size_t total = pools + columns + pathLookup + lowerPathLookup + pathIndex +
                   nameTrigrams + pathTrigrams + pathIdxToRecords + extensions + dirty;

    char line[160];
    std::string out;
    auto append = [&](const char* label, size_t bytes) {
        std::snprintf(line, sizeof(line), "  %-18s %8.1f MiB\n", label, MiB(bytes));
        out += line;
    };
    std::snprintf(line, sizeof(line),
                  "records=%u live=%u paths=%u\n",
                  static_cast<uint32_t>(types_.size()),
                  liveCount_.load(std::memory_order_relaxed),
                  pathPool_.entryCount());
    out += line;
    auto pool = [&](const char* label, const StringPool& p) {
        std::snprintf(line, sizeof(line), "  %-18s %8.1f MiB (chars %.1f/%.1f, entries %u)\n",
                      label,
                      MiB(p.bufferCapacity() + p.entryCapacityBytes()),
                      MiB(p.rawSize()), MiB(p.bufferCapacity()), p.entryCount());
        out += line;
    };
    pool("pool:orig-name", origNamePool_);
    pool("pool:name", namePool_);
    pool("pool:path", pathPool_);
    pool("pool:lower-path", lowerPathPool_);
    auto col = [&](const char* label, size_t cap, size_t sz, size_t width) {
        std::snprintf(line, sizeof(line), "  %-18s %8.1f MiB (%zu/%zu)\n",
                      label, MiB(cap * width), sz, cap);
        out += line;
    };
    // Diagnostic for posting-list pruning: how much of the payload sits in lists
    // so common that they barely narrow a candidate set (df > 2% of live records).
    const size_t hotCutoff = std::max<size_t>(16, liveCount_.load(std::memory_order_relaxed) / 50);
    size_t hotNameBytes = 0, hotNameLists = 0, hotPathBytes = 0, hotPathLists = 0;
    nameTrigramIndex_.hotPostings(hotCutoff, hotNameBytes, hotNameLists);
    pathTrigramIndex_.hotPostings(hotCutoff, hotPathBytes, hotPathLists);
    col("col:types", types_.capacity(), types_.size(), 1);
    col("col:sizes", sizes_.capacity(), sizes_.size(), 8);
    col("col:modTimes", modTimes_.capacity(), modTimes_.size(), 8);
    col("col:inodes", inodes_.capacity(), inodes_.size(), 8);
    col("col:devIds", devIds_.capacity(), devIds_.size(), 4);
    col("col:pathIndices", pathIndices_.capacity(), pathIndices_.size(), 4);
    std::snprintf(line, sizeof(line), "  %-18s %zu trigrams\n", "name-trigram-keys:",
                  nameTrigramIndex_.keyCount());
    out += line;
    std::snprintf(line, sizeof(line), "  %-18s %zu trigrams\n", "path-trigram-keys:",
                  pathTrigramIndex_.keyCount());
    out += line;
    std::snprintf(line, sizeof(line), "  %-18s %zu delta postings\n", "name-trig-delta:",
                  nameTrigramIndex_.deltaPostings());
    out += line;
    std::snprintf(line, sizeof(line), "  %-18s %zu delta postings\n", "path-trig-delta:",
                  pathTrigramIndex_.deltaPostings());
    out += line;
    std::snprintf(line, sizeof(line), "  %-18s %8.1f MiB in %zu hot lists\n", "hot-name-trigrams:",
                  MiB(hotNameBytes), hotNameLists);
    out += line;
    std::snprintf(line, sizeof(line), "  %-18s %8.1f MiB in %zu hot lists\n", "hot-path-trigrams:",
                  MiB(hotPathBytes), hotPathLists);
    out += line;
    append("path-index", pathIndex);
    append("path-lookup", pathLookup);
    append("lower-path-lookup", lowerPathLookup);
    append("name-trigrams", nameTrigrams);
    append("path-trigrams", pathTrigrams);
    append("path-to-records", pathIdxToRecords);
    append("extension-index", extensions);
    append("dirty-pages", dirty);
    std::snprintf(line, sizeof(line), "  %-18s %8.1f MiB\n", "TOTAL", MiB(total));
    out += line;
    return out;
}

std::vector<FileRecord> SearchEngine::exportRecords() const {
    std::shared_lock lock(mutex_);
    std::vector<FileRecord> result;
    result.reserve(liveCount_.load(std::memory_order_relaxed));
    for (size_t i = 0; i < types_.size(); i++) {
        if (types_[i] == 0) continue; // skip tombstones
        FileRecord r;
        r.name = origNamePool_.str(i);
        r.type = types_[i];
        r.size = sizes_[i];
        r.modTime = static_cast<time_t>(modTimes_[i]);
        r.inode = inodes_[i];
        r.devId = devIds_[i];
        r.path = pathPool_.str(pathIndices_[i]);
        result.push_back(std::move(r));
    }
    return result;
}

void SearchEngine::replayWALEntries(std::vector<WALEntry>&& entries) {
    if (entries.empty()) return;

    std::unique_lock lock(mutex_);
    invalidateQueryCache();   // WAL replay mutates the index

    for (auto& e : entries) {
        std::string lowerFull = me::toLower(e.fullPath);

        switch (e.op) {
        case WALOp::Add: {
            // If path already exists (duplicate Add), tombstone old record first
            uint32_t oldIdx = findRecordByLoweredPathUnlocked(lowerFull);
            if (oldIdx != PathIndexTable::kNotFound) {
                removeTrigramsForRecord(oldIdx);
                removePathTrigramsForRecord(oldIdx);
                removeExtensionForRecord(oldIdx);
                unindexRecordUnlocked(lowerFull);
                tombstoneAt(oldIdx);
                liveCount_.fetch_sub(1, std::memory_order_relaxed);
            }

            uint32_t idx = static_cast<uint32_t>(types_.size());
            std::string lower = me::toLower(e.record.name);
            uint32_t pIdx = internPath(e.record.path);
            if (pIdx >= pathIdxToRecords_.size()) {
                ensurePathTrigramsForPathIdx(pIdx);
            }
            e.record.path.clear();
            e.record.path.shrink_to_fit();

            origNamePool_.append(e.record.name);
            pushRecord(std::move(e.record));
            namePool_.append(lower);
            pathIndices_.push_back(pIdx);
            indexRecordUnlocked(lowerFull, idx);
            addTrigramsForRecord(idx, namePool_.data(idx), namePool_.length(idx));
            addPathTrigramsForRecord(idx);
            addExtensionForRecord(idx);
            if (idx / kRecordsPerPage >= dirtyPages_.size()) {
                dirtyPages_.resize(idx / kRecordsPerPage + 1, false);
            }
            markPageDirty(idx);
            liveCount_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case WALOp::Remove: {
            uint32_t idx = findRecordByLoweredPathUnlocked(lowerFull);
            if (idx == PathIndexTable::kNotFound) break; // silently ignore
            removeTrigramsForRecord(idx);
            removePathTrigramsForRecord(idx);
            removeExtensionForRecord(idx);
            unindexRecordUnlocked(lowerFull);
            tombstoneAt(idx);
            liveCount_.fetch_sub(1, std::memory_order_relaxed);
            break;
        }
        case WALOp::Update: {
            // Remove old record if exists
            uint32_t oldIdx = findRecordByLoweredPathUnlocked(lowerFull);
            if (oldIdx != PathIndexTable::kNotFound) {
                removeTrigramsForRecord(oldIdx);
                removePathTrigramsForRecord(oldIdx);
                removeExtensionForRecord(oldIdx);
                unindexRecordUnlocked(lowerFull);
                tombstoneAt(oldIdx);
                liveCount_.fetch_sub(1, std::memory_order_relaxed);
            }
            // Add updated record
            uint32_t newIdx = static_cast<uint32_t>(types_.size());
            std::string lower = me::toLower(e.record.name);
            uint32_t pIdx = internPath(e.record.path);
            if (pIdx >= pathIdxToRecords_.size()) {
                ensurePathTrigramsForPathIdx(pIdx);
            }
            e.record.path.clear();
            e.record.path.shrink_to_fit();

            origNamePool_.append(e.record.name);
            pushRecord(std::move(e.record));
            namePool_.append(lower);
            pathIndices_.push_back(pIdx);
            indexRecordUnlocked(lowerFull, newIdx);
            addTrigramsForRecord(newIdx, namePool_.data(newIdx), namePool_.length(newIdx));
            addPathTrigramsForRecord(newIdx);
            addExtensionForRecord(newIdx);
            if (newIdx / kRecordsPerPage >= dirtyPages_.size()) {
                dirtyPages_.resize(newIdx / kRecordsPerPage + 1, false);
            }
            markPageDirty(newIdx);
            liveCount_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        }
    }

    rebuildRecentCache();
}
