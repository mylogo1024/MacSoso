#pragma once
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

/// Memory-compact inverted index: key -> ascending, deduplicated uint32 postings.
///
/// A plain `unordered_map<Key, std::vector<uint32_t>>` costs 4 bytes per posting
/// plus up to 2x capacity slack from append-by-doubling, and it spreads the data
/// over one small allocation per key, which keeps hundreds of megabytes of
/// libmalloc regions permanently dirty. Here the whole payload lives in a single
/// byte arena as delta + LEB128 varint runs (typically 1-2 bytes per posting),
/// and the map only holds `{arena offset, byte length, count}` descriptors.
///
/// Post-build mutations go into a small `delta_` map so live filesystem events
/// stay cheap; reads merge the two sorted runs. The delta is folded back into the
/// arena by the next rebuild (Phase 2 completion or full compaction). Removals
/// are applied to the delta only — postings left in the arena point at
/// tombstoned records, which every candidate loop already filters out.
template <class Key>
class PostingIndex {
public:
    using Owned = std::unordered_map<Key, std::vector<uint32_t>>;

    PostingIndex() = default;
    PostingIndex(PostingIndex&&) = default;
    PostingIndex& operator=(PostingIndex&&) = default;
    PostingIndex(const PostingIndex&) = default;
    PostingIndex& operator=(const PostingIndex&) = default;

    /// Replace the contents with `src`, releasing the source as it encodes so the
    /// caller never holds base + source + arena fully resident at the same time.
    void build(Owned&& src) {
        clear();
        ids_.reserve(src.size());
        descs_.reserve(src.size());
        arena_.reserve(byteSizeOf(src));
        for (auto& entry : src) {
            std::vector<uint32_t>& list = entry.second;
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
            if (!list.empty()) {
                const uint32_t offset = static_cast<uint32_t>(arena_.size());
                uint32_t prev = 0;
                for (uint32_t value : list) {
                    appendVarint(value - prev);
                    prev = value;
                }
                descs_.push_back({offset,
                                  static_cast<uint32_t>(arena_.size() - offset),
                                  static_cast<uint32_t>(list.size())});
                ids_.emplace(entry.first, static_cast<uint32_t>(descs_.size() - 1));
                basePostings_ += list.size();
            }
            std::vector<uint32_t>().swap(list);   // hand the source block back now
        }
        Owned().swap(src);
    }

    /// Fold the queued delta back into the arena and forget it. Runs without a
    /// delta are copied byte for byte, so the cost is one arena copy plus the runs
    /// that really changed. Without this, a daemon that lives through weeks of
    /// filesystem churn would re-grow the exact per-key vector overhead this
    /// representation exists to avoid. Returns the number of postings folded.
    size_t foldDelta() {
        if (delta_.empty()) return 0;
        const size_t folded = deltaPostings();
        std::vector<uint8_t> next;
        next.reserve(arena_.size() + folded * 2 + 4096);
        std::vector<uint32_t> run;
        size_t postings = 0;

        for (auto& keyed : ids_) {
            Desc& desc = descs_[keyed.second];
            const Desc previous = desc;
            auto dit = delta_.find(keyed.first);
            if (dit == delta_.end()) {
                desc.offset = static_cast<uint32_t>(next.size());
                next.insert(next.end(), arena_.begin() + previous.offset,
                            arena_.begin() + previous.offset + previous.bytes);
                postings += previous.count;
                continue;
            }
            decode(keyed.first, run);
            delta_.erase(dit);
            desc.offset = static_cast<uint32_t>(next.size());
            uint32_t prev = 0;
            for (uint32_t value : run) {
                appendVarintTo(next, value - prev);
                prev = value;
            }
            desc.bytes = static_cast<uint32_t>(next.size() - desc.offset);
            desc.count = static_cast<uint32_t>(run.size());
            postings += run.size();
        }

        // Keys that only the delta knows about become new base runs.
        for (auto& keyed : delta_) {
            if (keyed.second.empty()) continue;
            descs_.push_back({static_cast<uint32_t>(next.size()), 0,
                              static_cast<uint32_t>(keyed.second.size())});
            uint32_t prev = 0;
            for (uint32_t value : keyed.second) {
                appendVarintTo(next, value - prev);
                prev = value;
            }
            descs_.back().bytes = static_cast<uint32_t>(next.size() - descs_.back().offset);
            ids_.emplace(keyed.first, static_cast<uint32_t>(descs_.size() - 1));
            postings += keyed.second.size();
        }

        arena_.swap(next);
        Owned().swap(delta_);
        basePostings_ = postings;
        return folded;
    }

    /// Postings held in the arena, i.e. everything except the pending delta.
    size_t basePostings() const { return basePostings_; }

    /// Decode the merged postings for `key` into `out`.
    /// Returns false when the key is unknown, in which case `out` is left empty.
    bool decode(const Key& key, std::vector<uint32_t>& out) const {
        out.clear();
        auto id = ids_.find(key);
        auto delta = delta_.find(key);
        if (id == ids_.end()) {
            if (delta == delta_.end()) return false;
            out = delta->second;
            return true;
        }
        const Desc& desc = descs_[id->second];
        out.resize(desc.count);
        decodeRun(desc, out.data());
        if (delta != delta_.end() && !delta->second.empty()) {
            out.insert(out.end(), delta->second.begin(), delta->second.end());
            std::inplace_merge(out.begin(), out.begin() + desc.count, out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
        }
        return true;
    }

    /// Posting count for `key`, or 0 when the key is unknown.
    uint32_t count(const Key& key) const {
        uint32_t total = 0;
        auto id = ids_.find(key);
        if (id != ids_.end()) total += descs_[id->second].count;
        auto delta = delta_.find(key);
        if (delta != delta_.end()) total += static_cast<uint32_t>(delta->second.size());
        return total;
    }

    /// Intersect the ascending `sorted` run with this key's postings and return
    /// the common values. The arena run is decoded on the fly and the delta is
    /// walked in place, so narrowing a query never materialises a posting list:
    /// a probe against a 2-million-posting key costs no allocation at all.
    std::vector<uint32_t> intersectWith(const std::vector<uint32_t>& sorted,
                                        const Key& key) const {
        std::vector<uint32_t> out;
        auto id = ids_.find(key);
        auto delta = delta_.find(key);
        if (id == ids_.end() && delta == delta_.end()) return out;

        const Desc* desc = id == ids_.end() ? nullptr : &descs_[id->second];
        const uint8_t* p = desc ? arena_.data() + desc->offset : nullptr;
        uint32_t left = desc ? desc->count : 0;
        uint32_t prev = 0;

        const std::vector<uint32_t>* extra =
            delta == delta_.end() ? nullptr : &delta->second;
        size_t extraPos = 0;

        uint32_t baseValue = 0;
        bool haveBase = false;
        // The arena is written by `build()`/`foldDelta()`, so every run ends on a
        // varint terminator: no per-byte bound check is needed while walking.
        auto pullBase = [&]() {
            if (left == 0) { haveBase = false; return; }
            uint32_t step = 0;
            unsigned shift = 0;
            for (;;) {
                const uint8_t byte = *p++;
                step |= static_cast<uint32_t>(byte & 0x7F) << shift;
                if (!(byte & 0x80)) break;
                shift += 7;
            }
            prev += step;
            baseValue = prev;
            left--;
            haveBase = true;
        };
        pullBase();

        const size_t probeSpan = (desc ? desc->count : 0) +
                                 (extra ? extra->size() : 0);
        out.reserve(std::min(sorted.size(), probeSpan));

        bool haveExtra = extra && !extra->empty();
        size_t i = 0;
        while (i < sorted.size() && (haveBase || haveExtra)) {
            uint32_t probe;
            if (haveBase && (!haveExtra || baseValue <= (*extra)[extraPos])) {
                probe = baseValue;
                pullBase();
            } else {
                probe = (*extra)[extraPos];
                extraPos++;
                haveExtra = extraPos < extra->size();
            }
            while (i < sorted.size() && sorted[i] < probe) i++;
            if (i == sorted.size()) break;
            if (sorted[i] == probe) {
                // A value can sit in both the arena and the delta if a caller
                // re-added something the base already had; never emit it twice.
                if (out.empty() || out.back() != probe) out.push_back(probe);
                i++;
            }
        }
        return out;
    }

    bool contains(const Key& key) const {
        return ids_.find(key) != ids_.end() || delta_.find(key) != delta_.end();
    }

    bool empty() const { return ids_.empty() && delta_.empty(); }
    size_t keyCount() const { return ids_.size() + delta_.size(); }
    size_t deltaPostings() const {
        size_t n = 0;
        for (const auto& entry : delta_) n += entry.second.size();
        return n;
    }

    /// Insert into the delta layer, keeping the run ascending and duplicate-free.
    void add(const Key& key, uint32_t value) {
        std::vector<uint32_t>& list = delta_[key];
        auto pos = std::lower_bound(list.begin(), list.end(), value);
        if (pos != list.end() && *pos == value) return;
        list.insert(pos, value);
    }

    void remove(const Key& key, uint32_t value) {
        auto it = delta_.find(key);
        if (it == delta_.end()) return;
        std::vector<uint32_t>& list = it->second;
        auto pos = std::lower_bound(list.begin(), list.end(), value);
        if (pos != list.end() && *pos == value) list.erase(pos);
        if (list.empty()) delta_.erase(it);
    }

    /// Batch removal against an ascending `values` run (one pass per key).
    void removeMany(const Key& key, const std::vector<uint32_t>& values) {
        auto it = delta_.find(key);
        if (it == delta_.end() || values.empty()) return;
        std::vector<uint32_t>& list = it->second;
        size_t out = 0, k = 0;
        for (size_t i = 0; i < list.size(); i++) {
            while (k < values.size() && values[k] < list[i]) k++;
            if (k < values.size() && values[k] == list[i]) continue;
            list[out++] = list[i];
        }
        list.resize(out);
        if (list.empty()) delta_.erase(it);
    }

    void clear() {
        std::vector<uint8_t>().swap(arena_);
        std::vector<Desc>().swap(descs_);
        std::unordered_map<Key, uint32_t>().swap(ids_);
        Owned().swap(delta_);
        basePostings_ = 0;
    }

    /// Give back arena headroom and bucket arrays that no longer pay for
    /// themselves. The arena is one large block, so shrinking it returns real
    /// pages to the kernel — unlike a forest of per-key vectors.
    void trim() {
        ids_.rehash(0);
        delta_.rehash(0);
        const size_t target = arena_.size() + std::max(arena_.size() / 16, size_t(65536));
        if (arena_.capacity() > target + target / 8) {
            std::vector<uint8_t> shrunk;
            shrunk.reserve(target);
            shrunk.insert(shrunk.end(), arena_.begin(), arena_.end());
            arena_.swap(shrunk);
        }
        const size_t descTarget = descs_.size() + std::max(descs_.size() / 16, size_t(1024));
        if (descs_.capacity() > descTarget + descTarget / 8) {
            std::vector<Desc> shrunk;
            shrunk.reserve(descTarget);
            shrunk.insert(shrunk.end(), descs_.begin(), descs_.end());
            descs_.swap(shrunk);
        }
    }

    size_t memoryBytes() const {
        size_t bytes = arena_.capacity() + descs_.capacity() * sizeof(Desc);
        bytes += ids_.bucket_count() * (sizeof(uint32_t) + sizeof(void*)) +
                 ids_.size() * (sizeof(Key) + sizeof(uint32_t) + 2 * sizeof(void*));
        for (const auto& entry : delta_) {
            bytes += entry.second.capacity() * sizeof(uint32_t) + 3 * sizeof(void*);
        }
        bytes += delta_.bucket_count() * (sizeof(Key) + sizeof(void*) + 2 * sizeof(void*));
        return bytes;
    }

    /// How much of the payload sits in lists longer than `cutoff` postings.
    /// Only used by the memory profile to price hypothetical high-df pruning.
    void hotPostings(size_t cutoff, size_t& hotBytes, size_t& hotLists) const {
        hotBytes = 0;
        hotLists = 0;
        for (const Desc& desc : descs_) {
            if (desc.count > cutoff) {
                hotBytes += desc.bytes;
                hotLists++;
            }
        }
    }

private:
    struct Desc {
        uint32_t offset;   // byte offset into arena_
        uint32_t bytes;    // encoded length
        uint32_t count;    // number of postings
    };

    static size_t varintBytes(uint32_t v) {
        size_t n = 1;
        while (v >= 0x80) { v >>= 7; n++; }
        return n;
    }

    static size_t byteSizeOf(const Owned& src) {
        size_t bytes = 0;
        for (const auto& entry : src) {
            uint32_t prev = 0;
            for (uint32_t value : entry.second) {
                bytes += varintBytes(value - prev);
                prev = value;
            }
        }
        return bytes;
    }

    void appendVarint(uint32_t v) { appendVarintTo(arena_, v); }

    static void appendVarintTo(std::vector<uint8_t>& out, uint32_t v) {
        while (v >= 0x80) {
            out.push_back(static_cast<uint8_t>(v) | 0x80);
            v >>= 7;
        }
        out.push_back(static_cast<uint8_t>(v));
    }

    void decodeRun(const Desc& desc, uint32_t* out) const {
        const uint8_t* p = arena_.data() + desc.offset;
        uint32_t value = 0;
        for (uint32_t i = 0; i < desc.count; i++) {
            uint32_t delta = 0;
            unsigned shift = 0;
            for (;;) {
                const uint8_t byte = *p++;
                delta |= static_cast<uint32_t>(byte & 0x7F) << shift;
                if (!(byte & 0x80)) break;
                shift += 7;
            }
            value += delta;
            out[i] = value;
        }
    }

    std::vector<uint8_t> arena_;
    std::vector<Desc> descs_;
    std::unordered_map<Key, uint32_t> ids_;       // key -> descriptor index
    Owned delta_;                                   // key -> ascending additions
    size_t basePostings_ = 0;                       // postings encoded in arena_
};
