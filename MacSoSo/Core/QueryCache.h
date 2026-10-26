#pragma once
// ═══════════════════════════════════════════════════════
//  QueryResultCache — thread-safe LRU cache for rendered
//  search results (an "index cache" for query answers).
//
//  Why: a repeated, identical query (re-run via Cmd+R, UI
//  refresh, history re-query, or simply the same token the
//  user types again) should not pay for the trigram scan,
//  name verification, and sort all over again. On a hit we
//  return the cached record-id vector directly — no engine
//  lock, no index walk.
//
//  Correctness: the owner (SearchEngine) clears the cache on
//  every index mutation (load / mutate / replay / compact).
//  The cache itself never reasons about data versions; it is
//  an MRU of (key -> results) that is wholesale-invalidated
//  whenever the underlying data could have changed.
// ═══════════════════════════════════════════════════════

#include <string>
#include <vector>
#include <unordered_map>
#include <list>
#include <mutex>
#include <cstdint>
#include <cstddef>

class QueryResultCache {
public:
    explicit QueryResultCache(size_t capacity = 1024,
                              size_t maxEntryBytes = 256 * 1024 * 1024)
        : capacity_(capacity)
        , maxEntryBytes_(maxEntryBytes)
    {}

    /// Look up `key`. Returns true and fills `out` on hit; false on miss or
    /// stale (superseded) entry. `out` is a copy so the caller owns it.
    bool lookup(const std::string& key, std::vector<uint32_t>& out) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) return false;
        out = it->second.results;
        // MRU: move touched key to the front of the recency list.
        lru_.splice(lru_.begin(), lru_, it->second.lruIt);
        return true;
    }

    /// Insert/replace an entry. Very large result sets are skipped so the
    /// cache can never balloon the process's memory on a broad query.
    void insert(const std::string& key, std::vector<uint32_t> results) {
        if (results.size() * sizeof(uint32_t) > maxEntryBytes_) return;
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second.results = std::move(results);
            lru_.splice(lru_.begin(), lru_, it->second.lruIt);
            return;
        }
        lru_.push_front(key);
        map_.emplace(key, Entry{std::move(results), lru_.begin()});
        // Evict least-recently-used until within capacity. Copy the key by
        // value BEFORE pop_back() — a reference into the list would dangle
        // once the node is removed.
        while (map_.size() > capacity_) {
            std::string back = lru_.back();
            lru_.pop_back();
            map_.erase(back);
        }
    }

    /// Drop everything. Called by the owner on any index mutation.
    void clear() {
        std::lock_guard<std::mutex> lk(mutex_);
        map_.clear();
        lru_.clear();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return map_.size();
    }

private:
    struct Entry {
        std::vector<uint32_t> results;
        std::list<std::string>::iterator lruIt;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> map_;
    mutable std::list<std::string> lru_; // front = most-recently-used
    size_t capacity_;
    size_t maxEntryBytes_;
};
