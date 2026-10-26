#pragma once
// ═══════════════════════════════════════════════════════
//  Part 81: PostingIndex (CSR + delta-varint) Tests
// ═══════════════════════════════════════════════════════

#include "PostingIndex.h"
#include <algorithm>
#include <iterator>
#include <set>

static void runPostingIndexTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 81: PostingIndex Tests\n";
    std::cout << "========================================\n\n";

    // ── Build / decode round-trip ──
    std::cout << "  --- Build and decode ---\n";
    {
        PostingIndex<uint32_t> index;
        PostingIndex<uint32_t>::Owned src;
        src[10] = {1, 2, 3, 4, 5};
        src[20] = {7, 7, 9, 100};      // duplicate collapses
        src[30] = {};                   // empty list is dropped
        src[40] = {500000, 4000, 90000000, 12};  // unsorted input
        index.build(std::move(src));

        check(index.keyCount() == 3, "posting: three live keys");
        check(!index.contains(30), "posting: empty list not indexed");

        std::vector<uint32_t> out;
        check(index.decode(10, out) && out == std::vector<uint32_t>({1, 2, 3, 4, 5}),
              "posting: sorted run decodes unchanged");
        check(index.decode(20, out) && out == std::vector<uint32_t>({7, 9, 100}),
              "posting: duplicates removed on build");
        check(index.decode(40, out) && out == std::vector<uint32_t>({12, 4000, 500000, 90000000}),
              "posting: input sorted before encoding");
        check(!index.decode(30, out) && out.empty(), "posting: absent key decodes empty");
        check(index.count(10) == 5 && index.count(999) == 0, "posting: count reflects presence");
    }

    // ── Wide deltas still round-trip ──
    std::cout << "  --- Wide and narrow deltas ---\n";
    {
        PostingIndex<uint32_t> index;
        PostingIndex<uint32_t>::Owned src;
        std::vector<uint32_t> sparse;
        for (uint32_t i = 0; i < 1000; i++) sparse.push_back(i * 1000003u);
        src[1] = sparse;
        std::vector<uint32_t> dense;
        for (uint32_t i = 0; i < 1000; i++) dense.push_back(i);
        src[2] = dense;
        src[3] = {0, UINT32_MAX};
        index.build(std::move(src));

        std::vector<uint32_t> out;
        check(index.decode(1, out) && out == sparse, "posting: large deltas round-trip");
        check(index.decode(2, out) && out == dense, "posting: unit deltas round-trip");
        check(index.decode(3, out) && out == std::vector<uint32_t>({0, UINT32_MAX}),
              "posting: full 32-bit range round-trips");
    }

    // ── Compression actually happens ──
    std::cout << "  --- Arena is smaller than raw postings ---\n";
    {
        PostingIndex<uint32_t> index;
        PostingIndex<uint32_t>::Owned src;
        for (uint32_t key = 0; key < 200; key++) {
            std::vector<uint32_t> list;
            for (uint32_t i = key; i < 200000; i += 200) list.push_back(i);
            src[key] = std::move(list);
        }
        size_t postings = 0;
        for (const auto& entry : src) postings += entry.second.size();
        index.build(std::move(src));
        check(postings == 200000, "posting: fixture has 200k postings");
        check(index.memoryBytes() < postings * sizeof(uint32_t) * 3 / 4,
              "posting: encoded payload well under the raw uint32 size");

        std::vector<uint32_t> out;
        check(index.decode(7, out) && out.size() == 1000 && out.front() == 7 && out.back() == 199807,
              "posting: decoded run intact after compression");
    }

    // ── Delta layer ──
    std::cout << "  --- Delta merge ---\n";
    {
        PostingIndex<uint32_t> index;
        PostingIndex<uint32_t>::Owned src;
        src[5] = {10, 20, 30};
        index.build(std::move(src));

        index.add(5, 25);
        index.add(5, 5);
        index.add(5, 25);                 // duplicate add ignored
        std::vector<uint32_t> out;
        check(index.decode(5, out) && out == std::vector<uint32_t>({5, 10, 20, 25, 30}),
              "posting: delta merged into ascending order");
        check(index.count(5) == 5, "posting: count includes delta");

        index.add(9, 1);                  // key absent from the base
        check(index.contains(9) && index.decode(9, out) && out == std::vector<uint32_t>({1}),
              "posting: delta-only key decodes");
        check(!index.empty(), "posting: non-empty with content");

        index.remove(9, 1);
        check(!index.contains(9), "posting: delta-only key removed entirely");
        index.remove(5, 25);
        check(index.decode(5, out) && out == std::vector<uint32_t>({5, 10, 20, 30}),
              "posting: delta entry removed");
        index.remove(5, 20);              // base-only value: stays visible until rebuild
        check(index.decode(5, out) && out == std::vector<uint32_t>({5, 10, 20, 30}),
              "posting: base postings survive removal (tombstone filter handles it)");

        std::vector<uint32_t> many = {1, 2, 4, 7, 8};
        PostingIndex<uint32_t>::Owned src2;
        src2[1] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        index.build(std::move(src2));
        for (uint32_t v : many) index.add(1, v);
        index.removeMany(1, many);
        check(index.decode(1, out) && out == std::vector<uint32_t>({1, 2, 3, 4, 5, 6, 7, 8, 9}),
              "posting: batch removal clears the delta only");
        check(index.deltaPostings() == 0, "posting: delta empty after batch removal");
    }

    // ── Rebuild replaces everything ──
    std::cout << "  --- Rebuild clears the old state ---\n";
    {
        PostingIndex<uint32_t> index;
        PostingIndex<uint32_t>::Owned src;
        src[1] = {1, 2, 3};
        index.build(std::move(src));
        index.add(2, 77);
        index.add(7, 3);

        PostingIndex<uint32_t>::Owned src2;
        src2[2] = {4, 5};
        index.build(std::move(src2));
        std::vector<uint32_t> out;
        check(!index.contains(1) && !index.contains(7), "posting: old keys gone after rebuild");
        check(index.decode(2, out) && out == std::vector<uint32_t>({4, 5}),
              "posting: stale delta not carried into rebuild");

        index.clear();
        check(index.empty() && index.keyCount() == 0, "posting: clear releases storage");
    }

    // ── trim() keeps data and hot stats work ──
    std::cout << "  --- Trim and hot-posting stats ---\n";
    {
        PostingIndex<uint32_t> index;
        PostingIndex<uint32_t>::Owned src;
        for (uint32_t key = 0; key < 50; key++) {
            std::vector<uint32_t> list;
            for (uint32_t i = 0; i < 1000; i++) list.push_back(key * 100000u + i);
            src[key] = std::move(list);
        }
        index.build(std::move(src));
        index.trim();
        std::vector<uint32_t> out;
        check(index.decode(13, out) && out.size() == 1000 && out.front() == 1300000u,
              "posting: decode still correct after trim");

        size_t hotBytes = 0, hotLists = 0;
        index.hotPostings(500, hotBytes, hotLists);
        check(hotLists == 50 && hotBytes > 0, "posting: hot-posting diagnostic counts lists");
        index.hotPostings(100000, hotBytes, hotLists);
        check(hotLists == 0 && hotBytes == 0, "posting: hot-posting respects the cutoff");
    }

    // ── String keys (the extension-index shape) ──
    std::cout << "  --- String keys ---\n";
    {
        PostingIndex<std::string> index;
        PostingIndex<std::string>::Owned src;
        src["txt"] = {2, 4, 6};
        src["md"] = {1, 3};
        index.build(std::move(src));
        index.add("txt", 5);
        std::vector<uint32_t> out;
        check(index.decode("txt", out) && out == std::vector<uint32_t>({2, 4, 5, 6}),
              "posting: string key merges delta");
        check(index.decode("md", out) && out == std::vector<uint32_t>({1, 3}),
              "posting: string key decodes base");
        check(!index.contains("rs"), "posting: absent string key");
    }

    // ── foldDelta() folds the mutation layer back into the arena ──
    std::cout << "  --- Delta fold back into the arena ---\n";
    {
        PostingIndex<uint32_t> index;
        PostingIndex<uint32_t>::Owned src;
        for (uint32_t key = 0; key < 100; key++) {
            std::vector<uint32_t> list;
            for (uint32_t i = 0; i < 100; i++) list.push_back(key * 1000u + i);
            src[key] = std::move(list);
        }
        index.build(std::move(src));
        check(index.basePostings() == 10000 && index.deltaPostings() == 0,
              "posting: base count tracks the arena");
        check(index.foldDelta() == 0, "posting: folding an empty delta is a no-op");

        index.add(50, 50999);          // new tail value on an existing key
        index.add(7, 7100);            // value just past an existing run
        index.add(999, 1u);            // key the arena has never seen
        index.add(999, 5u);
        index.remove(50, 50999);       // queued then cancelled again
        check(index.deltaPostings() == 3, "posting: delta counted before fold");

        const size_t folded = index.foldDelta();
        check(folded == 3, "posting: fold reports the folded posting count");
        check(index.deltaPostings() == 0, "posting: delta is gone after fold");
        check(index.basePostings() == 10000 + 3, "posting: base grew by the folded postings");

        std::vector<uint32_t> out;
        check(index.decode(50, out) && out.size() == 100 && out.back() == 50099u,
              "posting: removed delta value did not reach the arena");
        check(index.decode(7, out) && out.size() == 101 && out.back() == 7100u,
              "posting: folded value landed in sorted position");
        check(index.decode(999, out) && out == std::vector<uint32_t>({1u, 5u}),
              "posting: delta-only key becomes a base run");
        check(index.contains(999) && index.count(999) == 2,
              "posting: folded key is queryable");

        bool everyRunIntact = true;
        for (uint32_t key = 0; key < 100; key++) {
            if (key == 7) continue;    // already checked, it grew by one
            std::vector<uint32_t> run;
            if (!index.decode(key, run) || run.size() != 100 ||
                run.front() != key * 1000u || run.back() != key * 1000u + 99u) {
                everyRunIntact = false;
                break;
            }
        }
        check(everyRunIntact, "posting: untouched runs survive the fold byte for byte");

        index.trim();
        check(index.decode(999, out) && out.size() == 2,
              "posting: still readable after trimming the folded arena");
    }
    {
        PostingIndex<std::string> exts;
        PostingIndex<std::string>::Owned src;
        src["txt"] = {2, 4, 6};
        exts.build(std::move(src));
        exts.add("md", 1);
        exts.add("md", 3);
        check(exts.foldDelta() == 2, "posting: string keys fold too");
        std::vector<uint32_t> out;
        check(exts.decode("md", out) && out == std::vector<uint32_t>({1, 3}) &&
              exts.decode("txt", out) && out == std::vector<uint32_t>({2, 4, 6}),
              "posting: string fold keeps both shapes");
    }

    {
        // Production-shaped fold: thousands of keys, a long stream of mutations,
        // and the arena compared against a literal reference run per key.
        constexpr uint32_t kKeys = 2000;
        constexpr uint32_t kPerKey = 300;
        PostingIndex<uint32_t> index;
        std::vector<std::vector<uint32_t>> reference(kKeys);
        PostingIndex<uint32_t>::Owned src;
        for (uint32_t key = 0; key < kKeys; key++) {
            auto& list = src[key];
            list.reserve(kPerKey);
            for (uint32_t i = 0; i < kPerKey; i++) list.push_back(key * 1000u + i * 3u);
            reference[key] = list;
        }
        index.build(std::move(src));
        check(index.basePostings() == static_cast<size_t>(kKeys) * kPerKey,
              "posting: bulk base tracks the reference size");

        uint64_t rng = 0x9E3779B97F4A7C15ull;
        auto next = [&](uint32_t bound) {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            return static_cast<uint32_t>(rng % bound);
        };
        for (uint32_t step = 0; step < 20000; step++) {
            const uint32_t key = next(kKeys);
            const uint32_t value = next(kKeys * 1000u + 900u);
            auto& list = reference[key];
            auto pos = std::lower_bound(list.begin(), list.end(), value);
            if (next(3) == 0) {
                if (pos != list.end() && *pos == value) {
                    list.erase(pos);
                    index.remove(key, value);
                }
            } else if (pos == list.end() || *pos != value) {
                list.insert(pos, value);
                index.add(key, value);
            }
        }
        const size_t pending = index.deltaPostings();
        check(pending > 0, "posting: bulk mutations queued in the delta");
        check(index.foldDelta() == pending, "posting: bulk fold reports the queued count");
        check(index.deltaPostings() == 0, "posting: bulk delta is empty after fold");

        bool everyRunMatches = true;
        size_t total = 0;
        for (uint32_t key = 0; key < kKeys; key++) {
            std::vector<uint32_t> run;
            if (!index.decode(key, run) || run != reference[key]) {
                everyRunMatches = false;
                break;
            }
            if (index.count(key) != reference[key].size()) {
                everyRunMatches = false;
                break;
            }
            total += run.size();
        }
        check(everyRunMatches, "posting: every folded run equals the reference");
        check(total == index.basePostings(), "posting: base count matches the folded runs");

        bool intersectStillExact = true;
        for (uint32_t attempt = 0; attempt < 200; attempt++) {
            const uint32_t key = next(kKeys);
            std::vector<uint32_t> probe;
            for (uint32_t i = 0; i < 8; i++) {
                probe.push_back(next(kKeys * 1000u + 900u));
                if (key < kKeys && i % 2 == 0 && !reference[key].empty())
                    probe.push_back(reference[key][next(static_cast<uint32_t>(reference[key].size()))]);
            }
            std::sort(probe.begin(), probe.end());
            probe.erase(std::unique(probe.begin(), probe.end()), probe.end());
            std::vector<uint32_t> expected;
            std::set_intersection(probe.begin(), probe.end(), reference[key].begin(),
                                  reference[key].end(), std::back_inserter(expected));
            if (index.intersectWith(probe, key) != expected) {
                intersectStillExact = false;
                break;
            }
        }
        check(intersectStillExact, "posting: intersectWith stays exact after the bulk fold");
    }

    std::cout << "\n";
}
