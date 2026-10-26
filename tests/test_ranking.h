#pragma once
// ═══════════════════════════════════════════════════════
//  Part 3e: Search Ranking Tests
// ═══════════════════════════════════════════════════════

static void runSearchRankingTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 3e: Search Ranking Tests\n";
    std::cout << "========================================\n\n";

    // ── Test 1: Space-split AND query — scoring term is first TERM ──
    // "Alfred 5.app" is parsed as AND(TERM("alfred"), TERM("5.app"))
    // Scoring term = "alfred", so exact name "Alfred" gets priority 0
    std::cout << "  --- Space-split AND ranking ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"Alfred 5.app", "/Applications", 5, 0, 1000});
        records.push_back({"Info.plist", "/Applications/Alfred 5.app/Contents", 1, 100, 2000});
        records.push_back({"Alfred", "/Applications/Alfred 5.app/Contents/MacOS", 1, 5000, 3000});
        records.push_back({"alfred_helper", "/usr/local/bin", 1, 200, 4000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("Alfred 5.app");
        check(!res.empty(), "Ranking: 'Alfred 5.app' has results");
        // Terms match filenames only, so the "Alfred" binary (whose path, not
        // name, contains "5.app") is no longer part of the result set.
        check(engine.getRecord(res[0]).name == "Alfred 5.app",
              "Ranking: filename matching requires both terms in the name");
    }

    // ── Test 2: Prefix match before contains match ──
    std::cout << "\n  --- Prefix vs contains priority ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        // "alfred_helper" starts with "alfred" -> priority 1
        // "Alfred 5.app" lowercased is "alfred 5.app", starts with "alfred" -> priority 1
        // "xalfred" contains "alfred" but doesn't start with it -> priority 2
        records.push_back({"xalfred", "/tmp", 1, 100, 1000});
        records.push_back({"Alfred 5.app", "/Applications", 5, 0, 2000});
        records.push_back({"alfred_helper", "/bin", 1, 200, 3000});  // short path
        records.push_back({"Alfred", "/opt", 1, 5000, 4000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("alfred");
        check(res.size() == 4, "Ranking: 'alfred' matches all 4 records");

        // "Alfred" is exact match (case-insensitive) -> priority 0
        check(engine.getRecord(res[0]).name == "Alfred",
              "Ranking: exact match 'Alfred' is first");

        // Both "alfred_helper" and "Alfred 5.app" are prefix matches (priority 1)
        // "alfred_helper" at /bin (path len 18) < "Alfred 5.app" at /Applications (path len 25)
        check(engine.getRecord(res[1]).name == "alfred_helper",
              "Ranking: prefix match 'alfred_helper' is second (shorter path)");

        // "xalfred" contains "alfred" but doesn't start with it -> priority 2
    }

    // ── Test 3: Default search matches filenames, path: opts in ──
    std::cout << "\n  --- Filename-only vs path: search ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"main.cpp", "/usr/local/src", 1, 100, 1000});
        records.push_back({"local.conf", "/etc", 1, 200, 2000});
        records.push_back({"readme.md", "/usr/local/docs", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("local");
        check(res.size() == 1, "Ranking: 'local' matches 1 filename");
        if (!res.empty())
            check(engine.getRecord(res[0]).name == "local.conf",
                  "Ranking: filename match 'local.conf'");

        res = engine.query("location:local");
        check(res.size() == 2, "Ranking: 'location:local' matches 2 parent paths");
        for (auto idx : res) {
            std::string name = engine.getRecord(idx).name;
            check(name.find("local") == std::string::npos,
                  "Ranking: path-only matches do not contain 'local' in the name");
        }
    }

    // ── Test 4: Same priority sorted by path length ──
    std::cout << "\n  --- Same priority: shorter path first ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"test.txt", "/a/b/c/d/e", 1, 100, 1000});       // deep path
        records.push_back({"test.txt", "/tmp", 1, 100, 2000});              // shallow path
        records.push_back({"test.txt", "/a/b/c", 1, 100, 3000});            // medium path
        engine.loadRecords(std::move(records));

        auto res = engine.query("test.txt");
        check(res.size() == 3, "Ranking: 3 exact matches");

        // All are exact name matches (priority 0), sorted by full path length
        auto r0 = engine.getRecord(res[0]);
        auto r1 = engine.getRecord(res[1]);
        auto r2 = engine.getRecord(res[2]);

        std::string p0 = r0.path + "/" + r0.name;
        std::string p1 = r1.path + "/" + r1.name;
        std::string p2 = r2.path + "/" + r2.name;

        check(p0.size() <= p1.size() && p1.size() <= p2.size(),
              "Ranking: results sorted by ascending path length");
        check(r0.path == "/tmp", "Ranking: shortest path '/tmp/test.txt' is first");
    }

    // ── Test 5: maxResults truncates after sort ──
    std::cout << "\n  --- maxResults truncates after sort ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        // Add many name-matching records (all contain "target" in name)
        for (int i = 0; i < 100; i++) {
            records.push_back({"target_" + std::to_string(i) + ".dat",
                               "/deep/nested/dir", 1, 100, 1000});
        }
        // Add exact name match last (highest index)
        records.push_back({"target", "/usr", 1, 0, 2000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("target", 5);
        check(!res.empty(), "Ranking: maxResults=5 has results");
        check(engine.getRecord(res[0]).name == "target",
              "Ranking: exact match at high index still first with maxResults=5");
        check(res.size() == 5, "Ranking: maxResults=5 returns exactly 5");
    }

    // ── Test 6: Glob patterns work (no ranking, just match) ──
    std::cout << "\n  --- Glob pattern matching ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"hello.cpp", "/src", 1, 100, 1000});
        records.push_back({"hello.h", "/src", 1, 50, 2000});
        records.push_back({"world.cpp", "/src", 1, 200, 3000});
        records.push_back({"test.py", "/src", 1, 150, 4000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("*.cpp");
        check(res.size() == 2, "Glob '*.cpp': 2 matches");

        res = engine.query("hello.*");
        check(res.size() == 2, "Glob 'hello.*': 2 matches");

        res = engine.query("?orld.cpp");
        check(res.size() == 1, "Glob '?orld.cpp': 1 match");
    }

    std::cout << "\n";
}
