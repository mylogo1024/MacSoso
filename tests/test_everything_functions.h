#pragma once
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>

// ── Part 80: Everything Functions / Modifiers / Date Syntax ──
// Covers the additional voidtools Everything behaviours:
//   * functions:   name:, stem:, extension:, full-path:, parent-name:,
//                  ancestor:, root:, child-file:, child-folder:,
//                  dm-hour:, dm-month:, dm-year:, dm-dow:,
//                  count:, distinct:, unique:
//   * modifiers:   prefix:/start-with:, suffix:/end-with:, wildcards:,
//                  no-wildcards:, ignore-punctuation:, ignore-whitespace:,
//                  diacritics:, dot-all:, multi-line:, ungreedy:
//   * numeric:     == / != / .. ranges
//   * dates:       relative durations (10mins, 2hours)

static void runEverythingFunctionsTests() {
    std::cout << "── Part 80: Everything Functions & Modifiers ──\n";
    int localPassed = 0, localFailed = 0;

    auto check = [&](bool cond, const std::string& msg) {
        if (cond) { localPassed++; passed++; }
        else { localFailed++; failed++; std::cout << "  FAIL: " << msg << "\n"; }
    };

    // ═══════════════════════════════════════════
    // Section A: modifier parsing
    // ═══════════════════════════════════════════

    auto parseMod = [](const std::string& name, const std::string& arg) {
        auto node = QueryNode::makeFilter(name, arg);
        QueryFilterParser::parse(*node);
        return node;
    };

    { // prefix:/start-with: → STARTSWITH
        auto n = parseMod("prefix", "abc");
        check(n->type == QueryNodeType::TERM, "A1: prefix: → TERM");
        check(n->mode == MatchMode::STARTSWITH, "A1: prefix: → STARTSWITH");
    }
    { // suffix:/end-with: → ENDSWITH
        auto n = parseMod("end-with", "xyz");
        check(n->mode == MatchMode::ENDSWITH, "A2: end-with: → ENDSWITH");
    }
    { // stem: → target STEM, substring match within the stem
        auto n = parseMod("stem", "make");
        check(n->type == QueryNodeType::TERM, "A3: stem: → TERM");
        check(n->target == MatchTarget::STEM, "A3: stem: → STEM target");
    }
    { // wildcards: enables glob mode
        auto n = parseMod("wildcards", "a*c");
        check(n->mode == MatchMode::GLOB, "A4: wildcards: → GLOB");
    }
    { // no-wildcards: flags literal wildcards
        auto n = parseMod("no-wildcards", "a*c");
        check(n->literalWild == true, "A5: no-wildcards: sets literalWild");
        check(n->mode == MatchMode::SUBSTRING, "A5: no-wildcards: keeps SUBSTRING");
    }
    { // ignore-punctuation: / ignore-whitespace: flags
        auto n = parseMod("ignore-punctuation", "spider man");
        check(n->ignorePunct, "A6: ignore-punctuation flag");
        auto m = parseMod("ignore-whitespace", "spider man");
        check(m->ignoreSpace, "A6: ignore-whitespace flag");
        auto d = parseMod("diacritics", "resume");
        // diacritics: switches from folding to exact accent matching.
        check(!d->stripDiacritics, "A6: diacritics flag");
    }
    { // dot-all: / multi-line: / ungreedy: set regex flags
        auto n = parseMod("dot-all", "regex:^a");
        check(n->regexFlags.find('s') != std::string::npos, "A7: dot-all → s flag");
        auto m = parseMod("multi-line", "regex:^a");
        check(m->regexFlags.find('m') != std::string::npos, "A7: multi-line → m flag");
        auto u = parseMod("ungreedy", "regex:a.*b");
        check(u->regexFlags.find('U') != std::string::npos, "A7: ungreedy → U flag");
    }
    { // stackable: dotall:regex:^a
        auto n = parseMod("dot-all", "regex:^foo$");
        check(n->mode == MatchMode::REGEX, "A8: dot-all:regex: → REGEX");
        check(n->regexFlags.find('s') != std::string::npos, "A8: dot-all flag preserved");
    }

    // ═══════════════════════════════════════════
    // Section B: function parsing
    // ═══════════════════════════════════════════

    { // extension: is a term over the extension, lowercased for matching
        auto n = parseMod("extension", "JPG");
        check(n->type == QueryNodeType::TERM &&
              n->target == MatchTarget::EXTENSION, "B1: extension: target");
        check(n->textLower == "jpg", "B1: extension: lowercased match text");
        check(n->typeConstraint == 1, "B1: extension: files only");
    }
    { // full-path: matches against the complete path
        auto n = parseMod("full-path", "usr");
        check(n->type == QueryNodeType::TERM &&
              n->target == MatchTarget::FULL_PATH, "B2: full-path: → full path");
    }
    { // parent-name: matches the immediate parent folder name
        auto n = parseMod("parent-name", "Downloads");
        check(n->type == QueryNodeType::TERM &&
              n->target == MatchTarget::PARENT_NAME, "B3: parent-name target");
    }
    { // ancestor: matches a specified ancestor path
        auto n = parseMod("ancestor", "projects");
        check(n->type == QueryNodeType::TERM &&
              n->target == MatchTarget::ANCESTOR_PATH, "B4: ancestor target");
    }
    { // root: no arg
        auto n = parseMod("root", "");
        check(n->filterName == "root", "B5: root filterName");
    }
    { // dm-hour:14 → dateSegment 0, EQ, numVal1 14
        auto n = parseMod("dm-hour", "14");
        check(n->dateSegment && *n->dateSegment == 0, "B6: dm-hour segment=hour");
        check(n->op == CompareOp::EQ && n->numVal1 == 14, "B6: dm-hour value");
    }
    { // dm-month:december → month segment, value 12
        auto n = parseMod("dm-month", "december");
        check(n->dateSegment && *n->dateSegment ==
              static_cast<int>(QueryFilterParser::DateSegmentKind::MONTH),
              "B7: dm-month segment=month");
        check(n->numVal1 == 12, "B7: december → 12");
    }
    { // dm-month: dec → abbreviated
        auto n = parseMod("dm-month", "dec");
        check(n->numVal1 == 12, "B8: dec → 12");
    }
    { // dm-year:2026 → year segment, value 2026
        auto n = parseMod("dm-year", "2026");
        check(n->dateSegment && *n->dateSegment ==
              static_cast<int>(QueryFilterParser::DateSegmentKind::YEAR),
              "B9: dm-year segment=year");
        check(n->numVal1 == 2026, "B9: year value");
    }
    { // dm-dow:monday → day-of-week segment, value 1
        auto n = parseMod("dm-dow", "monday");
        check(n->dateSegment && *n->dateSegment ==
              static_cast<int>(QueryFilterParser::DateSegmentKind::DOW),
              "B10: dm-dow segment=dow");
        check(n->numVal1 == 1, "B10: monday → 1");
    }
    { // numeric == and !=
        auto n = parseMod("size", "==1024");
        check(n->op == CompareOp::EQ_EXACT && n->numVal1 == 1024, "B11: size:==1024 exact");
        auto m = parseMod("size", "!=1024");
        check(m->op == CompareOp::NE, "B11: size:!=1024 → NE");
    }

    // ═══════════════════════════════════════════
    // Section C: query-level result controls (extracted to root)
    // ═══════════════════════════════════════════

    {
        auto root = QueryParser::parse("count:5");
        check(root && root->limit && *root->limit == 5, "C1: count:5 → root limit");
    }
    {
        auto root = QueryParser::parse("distinct:name;size");
        check(root && root->distinctBy && *root->distinctBy == "name;size",
              "C2: distinct: → root distinctBy");
        check(root && !root->uniqueMode, "C2: distinct is not uniqueMode");
    }
    {
        auto root = QueryParser::parse("unique:size");
        check(root && root->uniqueMode, "C3: unique: sets uniqueMode");
    }

    // ═══════════════════════════════════════════
    // Section D: engine-level behaviour
    // ═══════════════════════════════════════════

    // Build a small file tree directly via FileRecord (name, path, type, size, mtime).
    SearchEngine engine;
    {
        std::vector<FileRecord> recs;
        recs.push_back({"top.txt", "", 1, 100, 1000});           // root: (no parent)
        recs.push_back({"readme.txt", "/demo", 1, 100, 1000});
        recs.push_back({"Makefile", "/demo", 1, 100, 1000});
        recs.push_back({"doc1.PDF", "/demo", 1, 100, 1000});      // uppercase ext
        recs.push_back({"doc2.PDF", "/demo", 1, 100, 1000});
        recs.push_back({"dup.txt", "/demo", 1, 100, 1000});       // duplicate name
        recs.push_back({"docs", "/demo", 2});                     // folder
        recs.push_back({"a.txt", "/demo/docs", 1, 100, 1000});
        recs.push_back({"intro.md", "/demo/docs", 1, 100, 1000});
        recs.push_back({"sub", "/demo/docs", 2});                 // folder (child of docs)
        recs.push_back({"deep.txt", "/demo/docs/sub", 1, 100, 1000});
        recs.push_back({"dup.txt", "/demo/docs", 1, 100, 1000});  // duplicate name
        engine.loadRecords(std::move(recs));
    }

    auto queryNamesOrdered = [&](const std::string& q) -> std::vector<std::string> {
        QueryTimingInfo timing;
        auto indices = engine.queryAdvanced(q, 1000, false, timing);
        std::vector<std::string> names;
        engine.forEachRecordWithPath(indices, [&](uint32_t, const FileRecord& rec, const std::string&) {
            names.push_back(rec.name);
        });
        return names;
    };
    auto queryNames = [&](const std::string& q) -> std::vector<std::string> {
        auto names = queryNamesOrdered(q);
        std::sort(names.begin(), names.end());
        return names;
    };

    { // stem:make → matches "Makefile" (stem "makefile")
        auto names = queryNames("stem:make");
        check(std::find(names.begin(), names.end(), "Makefile") != names.end(),
              "D1: stem:make matches Makefile");
        check(std::find(names.begin(), names.end(), "readme.txt") == names.end(),
              "D1: stem:make excludes readme.txt");
    }
    { // prefix:read → matches readme.txt
        auto names = queryNames("prefix:read");
        check(std::find(names.begin(), names.end(), "readme.txt") != names.end(),
              "D2: prefix:read matches readme.txt");
    }
    { // extension:pdf (contains) → matches both .PDF files (case-insensitive)
        auto names = queryNames("extension:pdf");
        size_t pdf = std::count(names.begin(), names.end(), "doc1.PDF") +
                     std::count(names.begin(), names.end(), "doc2.PDF");
        check(pdf == 2, "D3: extension:pdf matches 2 uppercase-PDF files");
    }
    { // parent-name:docs → files directly under docs/ (a.txt, intro.md)
        auto names = queryNames("parent-name:docs");
        check(std::find(names.begin(), names.end(), "a.txt") != names.end(),
              "D4: parent-name:docs matches a.txt");
        check(std::find(names.begin(), names.end(), "deep.txt") == names.end(),
              "D4: parent-name:docs excludes nested deep.txt");
    }
    { // ancestor:/demo/docs → matches anything below that ancestor path
        auto names = queryNames("ancestor:/demo/docs");
        check(std::find(names.begin(), names.end(), "deep.txt") != names.end(),
              "D5: ancestor:/demo/docs matches nested deep.txt");
        auto byName = queryNames("ancestor-name:docs");
        check(std::find(byName.begin(), byName.end(), "deep.txt") != byName.end(),
              "D5: ancestor-name:docs matches nested deep.txt");
    }
    { // count:2 → at most 2 results
        auto names = queryNames("count:2");
        check(names.size() <= 2, "D6: count:2 caps results at 2");
    }
    { // distinct:name → dedupes by name (dup.txt appears once)
        auto names = queryNames("distinct:name");
        check(std::count(names.begin(), names.end(), "dup.txt") == 1,
              "D7: distinct:name keeps one dup.txt");
    }
    { // root: → only items with no parent (top.txt)
        auto names = queryNames("root:");
        check(names.size() == 1 && names[0] == "top.txt",
              "D8: root: returns only top-level entries");
    }
    { // child-file:a.txt → the folder "docs" (has a file child named a.txt)
        auto names = queryNames("child-file:a.txt");
        check(std::find(names.begin(), names.end(), "docs") != names.end(),
              "D9: child-file:a.txt matches parent folder docs");
        check(std::find(names.begin(), names.end(), "deep.txt") == names.end(),
              "D9: child-file:a.txt excludes the file itself");
    }
    { // child-folder:sub → the folder "docs" (has a folder child named sub)
        auto names = queryNames("child-folder:sub");
        check(std::find(names.begin(), names.end(), "docs") != names.end(),
              "D10: child-folder:sub matches parent folder docs");
    }
    { // folders-first: folders (docs, sub) precede files (readme.txt, Makefile)
        // Use the ordered result: sorting the names here would erase the very
        // order this test is checking.
        auto names = queryNamesOrdered("sort:folders-first");
        auto pos = [&](const std::string& n) {
            auto it = std::find(names.begin(), names.end(), n);
            return it == names.end() ? names.size() : (size_t)(it - names.begin());
        };
        check(pos("docs") < pos("readme.txt"), "D11: folders-first puts docs before readme.txt");
        check(pos("sub")  < pos("readme.txt"), "D11: folders-first puts sub before readme.txt");
    }

    std::cout << "  Part 80 summary: " << localPassed << " passed, "
              << localFailed << " failed\n";
}
