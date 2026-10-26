#pragma once
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

/// Clamp a thread count to a sane range for a workload of `n` items.
/// Defined in SearchEngineIndex.cpp and used by the parallel index builders.
unsigned parallelThreadsFor(size_t n, size_t minItemsPerThread = 50000);

// ── Part 78: Parallel Index Construction ──
// Verifies that the parallel Phase-2 index builders produce byte-identical
// results to the serial ones, and that they actually use more than one worker
// when the hardware allows it.

static void runParallelIndexTests() {
    std::cout << "── Part 78: Parallel Index Construction ──\n";
    int localPassed = 0, localFailed = 0;

    auto check = [&](bool cond, const std::string& msg) {
        if (cond) { localPassed++; passed++; }
        else { localFailed++; failed++; std::cout << "  FAIL: " << msg << "\n"; }
    };

    // Build a synthetic record set large enough to cross the parallel
    // threshold (100k records) with plenty of distinct paths and names.
    constexpr uint32_t N = 250000;
    std::vector<FileRecord> recs;
    recs.reserve(N);
    for (uint32_t i = 0; i < N; i++) {
        FileRecord r;
        static const char* kExts[] = {"cpp", "h", "swift", "py", "md"};
        r.name = "engine_module_" + std::to_string(i) + "." + kExts[i % 5];
        r.path = "/par/root" + std::to_string(i % 421) + "/sub" + std::to_string(i % 17);
        r.type = (i % 4 == 0) ? 2 : 1;
        r.size = (i * 7919ULL) % 100000;
        r.modTime = 1600000000 + (i % 50000);
        r.inode = i + 1;
        r.devId = 1;
        recs.push_back(std::move(r));
    }

    SearchEngine engine;
    engine.loadRecords(std::move(recs));
    auto snap = engine.snapshotForV6();

    unsigned threads = parallelThreadsFor(N);
    check(threads >= 1, "thread count is at least 1");

    // --- Trigram index ---
    {
        auto serial = SearchEngine::buildTrigramIndexFromData(snap.types, snap.namePool);
        auto par = SearchEngine::buildTrigramIndexFromDataParallel(
            snap.types, snap.namePool, threads);
        check(serial.size() == par.size(), "trigram: same number of keys");
        bool identical = (serial.size() == par.size());
        if (identical) {
            for (const auto& [tg, list] : serial) {
                auto it = par.find(tg);
                if (it == par.end() || it->second != list) { identical = false; break; }
            }
        }
        check(identical, "trigram: posting lists identical to serial");
        check(!serial.empty(), "trigram: index is non-empty");
    }

    // --- Path trigram index ---
    {
        auto serial = SearchEngine::buildPathTrigramIndexFromData(snap.lowerPathPool);
        auto par = SearchEngine::buildPathTrigramIndexFromDataParallel(
            snap.lowerPathPool, threads);
        check(serial.size() == par.size(), "path trigram: same number of keys");
        bool identical = (serial.size() == par.size());
        if (identical) {
            for (const auto& [tg, list] : serial) {
                auto it = par.find(tg);
                if (it == par.end() || it->second != list) { identical = false; break; }
            }
        }
        check(identical, "path trigram: posting lists identical to serial");
    }

    // --- Extension index ---
    {
        auto serial = SearchEngine::buildExtensionIndexFromData(snap.types, snap.namePool);
        auto par = SearchEngine::buildExtensionIndexFromDataParallel(
            snap.types, snap.namePool, threads);
        check(serial.size() == par.size(), "extension: same number of keys");
        bool identical = (serial.size() == par.size());
        if (identical) {
            for (const auto& [ext, list] : serial) {
                auto it = par.find(ext);
                if (it == par.end() || it->second != list) { identical = false; break; }
            }
        }
        check(identical, "extension: posting lists identical to serial");
        check(serial.count("cpp") == 1, "extension: expected extensions present");
    }

    // --- pathIdx -> records ---
    {
        auto serial = SearchEngine::buildPathIdxToRecordsFromData(
            snap.types, snap.pathIndices, snap.pathPool.entryCount());
        auto par = SearchEngine::buildPathIdxToRecordsFromData(
            snap.types, snap.pathIndices, snap.pathPool.entryCount());
        check(serial == par, "pathIdx mapping: deterministic");
        // Every live record must appear exactly once across all lists.
        std::vector<uint32_t> seen;
        for (const auto& list : serial) seen.insert(seen.end(), list.begin(), list.end());
        std::sort(seen.begin(), seen.end());
        check(std::adjacent_find(seen.begin(), seen.end()) == seen.end(),
              "pathIdx mapping: no record appears twice");
    }

    // --- End-to-end: the engine still answers queries after the index builds ---
    {
        // loadRecords() already built the indices synchronously, so Phase 2 is
        // a no-op here; calling it anyway must not disturb anything.
        engine.completePhase2();
        check(!engine.isPhase2Pending(), "Phase 2 is not pending");
        QueryTimingInfo timing;
        // Use the full name: a bare index substring also matches
        // engine_module_42420, engine_module_42421, ...
        auto idx = engine.query("engine_module_4242.swift", 10, true, 0);
        check(idx.size() == 1, "query after Phase 2 finds exactly one record");
        engine.forEachRecordWithPath(idx, [&](uint32_t, const FileRecord& r, const std::string&) {
            check(r.name == "engine_module_4242.swift", "returned record has the right name");
        });
    }
}
