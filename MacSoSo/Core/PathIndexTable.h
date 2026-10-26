#pragma once
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

/// Open-addressed index mapping a record's 64-bit lowercase-path hash to its
/// record index.
///
/// This replaces an `unordered_map<std::string, uint32_t>` that owned a copy of
/// every record's full path. That map cost roughly 200 bytes per record on a
/// multi-million record index; a slot here is 16 bytes, and the caller verifies
/// a hit against the record it already holds, so no path text is duplicated.
class PathIndexTable {
public:
    static constexpr uint32_t kNotFound = UINT32_MAX;

    /// Hash used as the key. Must match `hashPath` below so callers can recompute
    /// it for verification-free membership tests.
    static uint64_t hashSeed() { return 1469598103934665603ULL; }

    /// FNV-1a 64-bit, exposed as a fold so a key built from several pieces
    /// (directory + separator + name) can be hashed without joining them first.
    static uint64_t hashFeed(uint64_t hash, std::string_view part) {
        for (unsigned char byte : part) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static uint64_t hashPath(std::string_view path) {
        return hashFeed(hashSeed(), path);
    }

    uint32_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    size_t slotCount() const { return slots_.size(); }
    size_t memoryBytes() const { return slots_.capacity() * sizeof(Slot); }

    void clear() {
        slots_.clear();
        slots_.shrink_to_fit();
        count_ = 0;
    }

    /// Allocate for `expected` entries without rehashing.
    void reserve(uint32_t expected) {
        if (expected <= loadLimit()) return;
        uint32_t capacity = slots_.empty() ? 1024u : static_cast<uint32_t>(slots_.size());
        while (capacity < expected + expected / 2) capacity <<= 1;
        rehashTo(capacity);
    }

    /// Give back capacity left over from deletions.
    void trim() {
        if (slots_.empty()) return;
        uint32_t wanted = 1024u;
        while (wanted < count_ + count_ / 2) wanted <<= 1;
        if (wanted < slots_.size()) rehashTo(wanted);
    }

    /// Look up `pathHash`. `verify(recordIdx)` confirms the candidate really is
    /// the requested record, which makes hash collisions harmless.
    template <typename Verify>
    uint32_t get(uint64_t pathHash, Verify&& verify) const {
        if (slots_.empty()) return kNotFound;
        const uint32_t mask = static_cast<uint32_t>(slots_.size() - 1);
        uint32_t slot = idealSlot(pathHash) & mask;
        for (uint32_t probe = 0; probe <= mask; ++probe) {
            const Slot& entry = slots_[slot];
            if (entry.recordIdx == kEmpty) return kNotFound;
            if (entry.hash == pathHash && verify(entry.recordIdx)) return entry.recordIdx;
            slot = (slot + 1) & mask;
        }
        return kNotFound;
    }

    /// Membership by hash alone, for callers that no longer hold the path text.
    bool contains(uint64_t pathHash) const {
        if (slots_.empty()) return false;
        const uint32_t mask = static_cast<uint32_t>(slots_.size() - 1);
        uint32_t slot = idealSlot(pathHash) & mask;
        for (uint32_t probe = 0; probe <= mask; ++probe) {
            const Slot& entry = slots_[slot];
            if (entry.recordIdx == kEmpty) return false;
            if (entry.hash == pathHash) return true;
            slot = (slot + 1) & mask;
        }
        return false;
    }

    /// Insert or replace. `verify(recordIdx)` identifies an existing entry.
    template <typename Verify>
    void put(uint64_t pathHash, uint32_t recordIdx, Verify&& verify) {
        if (count_ + 1 > loadLimit()) grow();
        const uint32_t mask = static_cast<uint32_t>(slots_.size() - 1);
        uint32_t slot = idealSlot(pathHash) & mask;
        for (uint32_t probe = 0; probe <= mask; ++probe) {
            Slot& entry = slots_[slot];
            if (entry.recordIdx == kEmpty) {
                entry = Slot{pathHash, recordIdx};
                count_++;
                return;
            }
            if (entry.hash == pathHash && verify(entry.recordIdx)) {
                entry.recordIdx = recordIdx;
                return;
            }
            slot = (slot + 1) & mask;
        }
        // Unreachable while the load factor stays below 1.
        grow();
        put(pathHash, recordIdx, verify);
    }

    /// Erase by hash + verification. Returns false when absent.
    template <typename Verify>
    bool erase(uint64_t pathHash, Verify&& verify) {
        if (slots_.empty()) return false;
        const uint32_t mask = static_cast<uint32_t>(slots_.size() - 1);
        uint32_t slot = idealSlot(pathHash) & mask;
        for (uint32_t probe = 0; probe <= mask; ++probe) {
            const Slot& entry = slots_[slot];
            if (entry.recordIdx == kEmpty) return false;
            if (entry.hash == pathHash && verify(entry.recordIdx)) {
                eraseAt(slot);
                return true;
            }
            slot = (slot + 1) & mask;
        }
        return false;
    }

    /// Erase the entry whose hash matches, without verifying the record.
    /// Used when the record itself is already gone and only the key is known.
    bool eraseByHash(uint64_t pathHash) {
        if (slots_.empty()) return false;
        const uint32_t mask = static_cast<uint32_t>(slots_.size() - 1);
        uint32_t slot = idealSlot(pathHash) & mask;
        for (uint32_t probe = 0; probe <= mask; ++probe) {
            const Slot& entry = slots_[slot];
            if (entry.recordIdx == kEmpty) return false;
            if (entry.hash == pathHash) {
                eraseAt(slot);
                return true;
            }
            slot = (slot + 1) & mask;
        }
        return false;
    }

    /// Visit every live entry as fn(hash, recordIdx).
    template <typename Fn>
    void forEach(Fn&& fn) const {
        for (const Slot& entry : slots_) {
            if (entry.recordIdx != kEmpty) fn(entry.hash, entry.recordIdx);
        }
    }

private:
    static constexpr uint32_t kEmpty = UINT32_MAX;

    struct Slot {
        uint64_t hash = 0;
        uint32_t recordIdx = kEmpty;
        uint32_t padding_ = 0;
    };
    static_assert(sizeof(Slot) == 16, "PathIndexTable slots must stay 16 bytes");

    static uint64_t idealSlot(uint64_t hash) {
        // Fibonacci hashing: spread the high bits across the table.
        return (hash * 11400714819323198485ULL) >> 32;
    }

    uint32_t loadLimit() const {
        return static_cast<uint32_t>(slots_.size() - slots_.size() / 3);
    }

    void grow() {
        uint32_t capacity = slots_.empty() ? 1024u : static_cast<uint32_t>(slots_.size() * 2);
        rehashTo(capacity);
    }

    void rehashTo(uint32_t capacity) {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(capacity, Slot{});
        count_ = 0;
        for (const Slot& entry : old) {
            if (entry.recordIdx == kEmpty) continue;
            insertRaw(entry.hash, entry.recordIdx);
        }
    }

    void insertRaw(uint64_t hash, uint32_t recordIdx) {
        const uint32_t mask = static_cast<uint32_t>(slots_.size() - 1);
        uint32_t slot = idealSlot(hash) & mask;
        while (slots_[slot].recordIdx != kEmpty) slot = (slot + 1) & mask;
        slots_[slot] = Slot{hash, recordIdx, 0};
        count_++;
    }

    /// Backward-shift deletion so probe chains stay intact without tombstones.
    void eraseAt(uint32_t hole) {
        const uint32_t mask = static_cast<uint32_t>(slots_.size() - 1);
        uint32_t cursor = (hole + 1) & mask;
        while (slots_[cursor].recordIdx != kEmpty) {
            const uint32_t ideal = static_cast<uint32_t>(idealSlot(slots_[cursor].hash)) & mask;
            if (((cursor - hole) & mask) <= ((cursor - ideal) & mask)) {
                slots_[hole] = slots_[cursor];
                hole = cursor;
            }
            cursor = (cursor + 1) & mask;
        }
        slots_[hole] = Slot{};
        count_--;
    }

    std::vector<Slot> slots_;
    uint32_t count_ = 0;
};
