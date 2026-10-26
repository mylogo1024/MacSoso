#pragma once
// Part 47: Path Trigram Index

static void runPathTrigramTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 47: Path Trigram Index\n";
    std::cout << "========================================\n\n";

    // -- Test 1: buildPathTrigramIndex correctness --
    std::cout << "  --- Test 1: Path trigram index build ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"file1.txt", "/usr/local/homebrew/bin", 1, 100, 1000});
        records.push_back({"file2.txt", "/usr/local/homebrew/lib", 1, 200, 2000});
        records.push_back({"file3.txt", "/Users/admin/Desktop", 1, 300, 3000});
        records.push_back({"file4.txt", "/tmp", 1, 400, 4000});
        engine.loadRecords(std::move(records));

        // Query "homebrew" — should match via path trigram, not name
        auto res = engine.query("path:homebrew");
        check(res.size() == 2, "Path trigram: 'path:homebrew' matches 2 files in homebrew paths");

        // Query "Desktop" — path match
        res = engine.query("path:Desktop");
        check(res.size() == 1, "Path trigram: 'path:Desktop' matches 1 file");
    }

    // -- Test 2: pathIdxToRecords correctness --
    std::cout << "\n  --- Test 2: pathIdx -> records mapping ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        // Multiple files in same directory
        records.push_back({"a.txt", "/shared/dir", 1, 100, 1000});
        records.push_back({"b.txt", "/shared/dir", 1, 200, 2000});
        records.push_back({"c.txt", "/shared/dir", 1, 300, 3000});
        records.push_back({"d.txt", "/other/place", 1, 400, 4000});
        engine.loadRecords(std::move(records));

        // "shared" only in path — should find all 3 files under /shared/dir
        auto res = engine.query("path:shared");
        check(res.size() == 3, "Path trigram: 'path:shared' finds all 3 files in /shared/dir");

        // "other" only in path — should find 1 file
        res = engine.query("path:other");
        check(res.size() == 1, "Path trigram: 'path:other' finds 1 file in /other/place");
    }

    // -- Test 3: Incremental add/remove consistency --
    std::cout << "\n  --- Test 3: Incremental add/remove ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"init.txt", "/project/alpha", 1, 100, 1000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("path:alpha");
        check(res.size() == 1, "Path trigram: initial 'path:alpha' match");

        // Add a record in a new path
        engine.addRecord({"new.txt", "/project/beta", 1, 200, 2000});
        res = engine.query("path:beta");
        check(res.size() == 1, "Path trigram: 'path:beta' found after addRecord");

        // Add another record in same path as first
        engine.addRecord({"extra.txt", "/project/alpha", 1, 300, 3000});
        res = engine.query("path:alpha");
        check(res.size() == 2, "Path trigram: 'path:alpha' matches 2 after second add");

        // Remove one record from alpha
        engine.removeByPath("/project/alpha/init.txt");
        res = engine.query("path:alpha");
        check(res.size() == 1, "Path trigram: 'path:alpha' matches 1 after remove");

        // Update record to a different path
        engine.updateByPath("/project/beta/new.txt", {"new.txt", "/project/gamma", 1, 200, 2000});
        res = engine.query("path:beta");
        check(res.empty(), "Path trigram: 'path:beta' empty after update moved record away");
        res = engine.query("path:gamma");
        check(res.size() == 1, "Path trigram: 'path:gamma' found after update");
    }

    // -- Test 4: Path-only query via trigram (keyword not in filename) --
    std::cout << "\n  --- Test 4: Path-only matching ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"Makefile", "/Users/dev/DerivedData/Build", 1, 100, 1000});
        records.push_back({"output.o", "/Users/dev/DerivedData/Build", 1, 200, 2000});
        records.push_back({"main.cpp", "/Users/dev/src", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "DerivedData" is only in path, not filename
        auto res = engine.query("path:DerivedData");
        check(res.size() == 2, "Path trigram: 'path:DerivedData' matches 2 files (path-only)");

        // Verify priority: these should be priority 3 (path-only match)
        // "main.cpp" should NOT match
        for (uint32_t idx : res) {
            auto rec = engine.getRecord(idx);
            check(rec.name == "Makefile" || rec.name == "output.o",
                  "Path trigram: matched files are under DerivedData path");
        }
    }

    // -- Test 5: Keywords with '/' match the full path --
    std::cout << "\n  --- Test 5: Slash in keyword matches full path ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"test.py", "/home/user/project", 1, 100, 1000});
        records.push_back({"test.py", "/var/log", 1, 200, 2000});
        engine.loadRecords(std::move(records));

        // "home/user" is matched against full paths → only the first record
        auto res = engine.query("home/user");
        check(res.size() == 1, "Slash keyword: 'home/user' matches 1 full path");
        if (!res.empty())
            check(engine.getRecord(res[0]).path == "/home/user/project",
                  "Slash keyword: correct record for 'home/user'");
    }

    // -- Test 6: Compaction preserves path trigram index --
    std::cout << "\n  --- Test 6: Compaction consistency ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        for (int i = 0; i < 20; i++) {
            records.push_back({"file_" + std::to_string(i) + ".txt",
                              "/comptest/subdir_" + std::to_string(i % 4), 1,
                              (uint64_t)(i * 100), (time_t)i});
        }
        engine.loadRecords(std::move(records));

        // Remove half
        for (int i = 0; i < 10; i++) {
            engine.removeByPath("/comptest/subdir_" + std::to_string(i % 4) +
                               "/file_" + std::to_string(i) + ".txt");
        }

        // Query before compaction
        auto resBefore = engine.query("path:comptest");
        size_t countBefore = resBefore.size();

        // Compact
        engine.compactRecords();

        // Query after compaction — should get same count
        auto resAfter = engine.query("path:comptest");
        check(resAfter.size() == countBefore,
              "Path trigram: same result count after compaction");
        check(resAfter.size() == 10, "Path trigram: 10 live records after compaction");
    }

    // -- Test 7: Short keyword (< 3 chars) falls back correctly --
    std::cout << "\n  --- Test 7: Short keyword fallback ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"readme.md", "/ab/cd", 1, 100, 1000});
        engine.loadRecords(std::move(records));

        // "ab" is 2 chars — cannot use trigram, must fall back to linear scan
        auto res = engine.query("path:ab");
        check(res.size() == 1, "Short keyword: 'path:ab' finds match via linear scan fallback");
    }

    // -- Test 8: batchRescanPrefix maintains path trigram --
    std::cout << "\n  --- Test 8: batchRescanPrefix ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"old1.txt", "/rescan/target", 1, 100, 1000});
        records.push_back({"old2.txt", "/rescan/target", 1, 200, 2000});
        records.push_back({"keep.txt", "/rescan/other", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // Batch rescan replaces files under /rescan/target
        std::vector<FileRecord> freshRecords;
        freshRecords.push_back({"new1.txt", "/rescan/target", 1, 400, 4000});
        freshRecords.push_back({"new2.txt", "/rescan/target", 1, 500, 5000});
        freshRecords.push_back({"new3.txt", "/rescan/target", 1, 600, 6000});
        engine.batchRescanPrefix("/rescan/target", std::move(freshRecords));

        auto res = engine.query("path:target");
        check(res.size() == 3, "batchRescan: 'path:target' matches 3 new files");

        res = engine.query("path:rescan");
        check(res.size() == 4, "batchRescan: 'path:rescan' matches all 4 files (3 new + 1 kept)");
    }

    std::cout << "\n  Part 47 complete.\n\n";
}
