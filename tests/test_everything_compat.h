#pragma once
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <algorithm>

// ── Part 77: Everything Compatibility Tests ──
// Covers the voidtools Everything behaviours added to the query language:
//   * sort: / sorted: modifier (all fields + directions)
//   * OR / AND / NOT keywords, () grouping, <> NOT operator
//   * ext: with comma / pipe separators, ext:.cpp with a leading dot
//   * size: N-M hyphen ranges
//   * fn:, fs:, fu:, noext: filters
//   * stackable modifiers (case:regex:..., ww:case:...)
//   * type: media macros
//   * empty:, childcount:, child:, dupe: aggregate filters

static void runEverythingCompatTests() {
    std::cout << "── Part 77: Everything Compatibility ──\n";
    int localPassed = 0, localFailed = 0;

    auto check = [&](bool cond, const std::string& msg) {
        if (cond) { localPassed++; passed++; }
        else { localFailed++; failed++; std::cout << "  FAIL: " << msg << "\n"; }
    };

    // ═══════════════════════════════════════════
    // Section A: sort: argument parsing
    // ═══════════════════════════════════════════

    {
        SortSpec s = SortSpec::parse("name");
        check(s.field == SortField::Name && s.dir == SortDir::Asc,
              "A1: sort:name → Name ascending");
    }
    {
        SortSpec s = SortSpec::parse("size-descending");
        check(s.field == SortField::Size && s.dir == SortDir::Desc,
              "A2: sort:size-descending → Size descending");
    }
    {
        SortSpec s = SortSpec::parse("-size");
        check(s.field == SortField::Size && s.dir == SortDir::Desc,
              "A3: sort:-size → Size descending (leading minus)");
    }
    {
        SortSpec s = SortSpec::parse("+size");
        check(s.field == SortField::Size && s.dir == SortDir::Asc,
              "A4: sort:+size → Size ascending (explicit plus)");
    }
    {
        SortSpec s = SortSpec::parse("size:descending");
        check(s.field == SortField::Size && s.dir == SortDir::Desc,
              "A5: sort:size:descending → Size descending (colon separator)");
    }
    {
        SortSpec s = SortSpec::parse("dm:descending");
        check(s.field == SortField::DateModified && s.dir == SortDir::Desc,
              "A6: sort:dm:descending → DateModified descending");
    }
    {
        SortSpec s = SortSpec::parse("datemodified_descending");
        check(s.field == SortField::DateModified && s.dir == SortDir::Desc,
              "A7: sort:datemodified_descending (underscore separator)");
    }
    {
        SortSpec s = SortSpec::parse("ext");
        check(s.field == SortField::Extension && s.dir == SortDir::Asc,
              "A8: sort:ext → Extension ascending");
    }
    {
        SortSpec s = SortSpec::parse("dc-asc");
        check(s.field == SortField::DateCreated && s.dir == SortDir::Asc,
              "A9: sort:dc-asc → DateCreated ascending");
    }
    {
        SortSpec s = SortSpec::parse("bogusfield");
        check(s.field == SortField::Relevance,
              "A10: unknown field falls back to Relevance");
    }
    {
        SortSpec s = SortSpec::parse("");
        check(s.isDefault(), "A11: empty sort arg keeps default ordering");
    }
    {
        check(SortSpec::isSortKeyword("sort") && SortSpec::isSortKeyword("sorted") &&
              SortSpec::isSortKeyword("sortby") && !SortSpec::isSortKeyword("size"),
              "A12: sort keyword recognition");
    }
    {
        SortSpec s = SortSpec::parse("size;name");
        check(s.field == SortField::Size && s.dir == SortDir::Asc &&
              s.extraKeys.size() == 1 &&
              s.extraKeys[0].first == SortField::Name &&
              s.extraKeys[0].second == SortDir::Asc,
              "A13: sort:size;name parses a secondary column");
    }
    {
        SortSpec s = SortSpec::parse("name;size-descending;-path");
        check(s.field == SortField::Name && s.dir == SortDir::Asc &&
              s.extraKeys.size() == 2 &&
              s.extraKeys[0].first == SortField::Size &&
              s.extraKeys[0].second == SortDir::Desc &&
              s.extraKeys[1].first == SortField::Path &&
              s.extraKeys[1].second == SortDir::Desc,
              "A14: per-column directions in multi-column sort:");
    }

    // ═══════════════════════════════════════════
    // Section B: tokenizer / parser syntax
    // ═══════════════════════════════════════════

    {
        auto toks = QueryTokenizer::tokenize("foo OR bar");
        check(toks.size() == 4 && toks[1].type == TokenType::WORD &&
              toks[1].value == "OR",
              "B1: 'OR' is plain text in Everything");
    }
    {
        auto toks = QueryTokenizer::tokenize("foo AND bar");
        check(toks.size() == 4 && toks[1].type == TokenType::WORD &&
              toks[1].value == "AND",
              "B2: 'AND' is plain text in Everything");
    }
    {
        auto toks = QueryTokenizer::tokenize("foo NOT bar");
        check(toks.size() == 4 && toks[1].type == TokenType::WORD &&
              toks[1].value == "NOT",
              "B3: 'NOT' is plain text in Everything");
    }
    {
        auto toks = QueryTokenizer::tokenize("<>foo");
        check(toks.size() == 4 && toks[0].type == TokenType::LANGLE &&
              toks[1].type == TokenType::RANGLE &&
              toks[2].type == TokenType::WORD && toks[2].value == "foo",
              "B4: '<>' is an empty group, not NOT");
    }
    {
        auto toks = QueryTokenizer::tokenize("(a|b) c");
        check(toks.size() == 5 && toks[0].type == TokenType::WORD &&
              toks[0].value == "(a" && toks[1].type == TokenType::PIPE &&
              toks[2].value == "b)" && toks[3].value == "c",
              "B5: parentheses are literal text");
    }
    {
        auto toks = QueryTokenizer::tokenize("a)b");
        check(toks.size() == 2 && toks[0].type == TokenType::WORD &&
              toks[0].value == "a)b",
              "B5b: ')' inside a word is kept literal");
    }
    {
        auto toks = QueryTokenizer::tokenize("foo(1).txt");
        check(toks.size() == 2 && toks[0].type == TokenType::WORD &&
              toks[0].value == "foo(1).txt",
              "B6: parentheses inside a file name stay literal");
    }
    {
        auto toks = QueryTokenizer::tokenize("report (final).pdf");
        bool hasWord = false;
        for (auto& t : toks)
            if (t.type == TokenType::WORD && t.value == "report") hasWord = true;
        check(hasWord, "B7: trailing parens are split off words");
    }
    {
        // OR must not be recognised in lower case, so "or" still matches names.
        auto toks = QueryTokenizer::tokenize("or");
        check(toks.size() == 2 && toks[0].type == TokenType::WORD,
              "B8: lowercase 'or' is a plain word");
    }
    {
        auto ast = QueryParser::parse("foo OR bar");
        check(ast && ast->type == QueryNodeType::AND && ast->children.size() == 3,
              "B9: 'foo OR bar' searches for three literal words");
    }
    {
        auto ast = QueryParser::parse("foo AND bar");
        check(ast && ast->type == QueryNodeType::AND && ast->children.size() == 3,
              "B10: 'foo AND bar' searches for three literal words");
    }
    {
        auto ast = QueryParser::parse("foo NOT bar");
        check(ast && ast->type == QueryNodeType::AND && ast->children.size() == 3,
              "B11: 'foo NOT bar' searches for three literal words");
    }
    {
        auto ast = QueryParser::parse("(foo|bar) baz");
        check(ast && ast->type == QueryNodeType::AND && ast->children.size() == 2 &&
              ast->children[0]->type == QueryNodeType::OR,
              "B12: '(foo|bar) baz' groups the OR under AND");
    }
    {
        auto ast = QueryParser::parse("<>foo");
        check(ast && ast->type == QueryNodeType::TERM && ast->text == "foo",
              "B13: '<>foo' is an empty group followed by foo");
    }
    {
        auto ast = QueryParser::parse("<foo bar>");
        check(ast && ast->type == QueryNodeType::AND,
              "B14: legacy '<...>' grouping still works");
    }

    // ═══════════════════════════════════════════
    // Section C: filter argument parsing
    // ═══════════════════════════════════════════

    {
        auto n = QueryNode::makeFilter("ext", "swift,h");
        QueryFilterParser::parse(*n);
        check(n->extList.size() == 2 && n->extList[0] == "swift" && n->extList[1] == "h",
              "C1: ext:swift,h splits on comma");
    }
    {
        auto n = QueryNode::makeFilter("ext", "cpp|h|hpp");
        QueryFilterParser::parse(*n);
        check(n->extList.size() == 3 && n->extList[2] == "hpp",
              "C2: ext:cpp|h|hpp splits on pipe");
    }
    {
        auto n = QueryNode::makeFilter("ext", ".cpp");
        QueryFilterParser::parse(*n);
        check(n->extList.size() == 1 && n->extList[0] == "cpp",
              "C3: ext:.cpp tolerates a leading dot");
    }
    {
        auto n = QueryNode::makeFilter("size", "1mb-5mb");
        QueryFilterParser::parse(*n);
        check(n->op == CompareOp::RANGE &&
              n->numVal1 == 1024ULL * 1024 && n->numVal2 == 5 * 1024ULL * 1024,
              "C4: size:1mb-5mb is a hyphen range");
    }
    {
        auto n = QueryNode::makeFilter("size", ">1mb");
        QueryFilterParser::parse(*n);
        check(n->op == CompareOp::GT && n->numVal1 == 1024ULL * 1024,
              "C5: size:>1mb still works");
    }
    {
        auto n = QueryNode::makeFilter("size", "100kb..5mb");
        QueryFilterParser::parse(*n);
        check(n->op == CompareOp::RANGE,
              "C6: size:100kb..5mb still works (dotdot range)");
    }
    {
        auto n = QueryNode::makeFilter("fs", ">10kb");
        QueryFilterParser::parse(*n);
        check(n->op == CompareOp::GT && n->numVal1 == 10 * 1024ULL,
              "C7: fs: is an alias for size:");
    }
    {
        auto n = QueryNode::makeFilter("fn", "Makefile");
        QueryFilterParser::parse(*n);
        check(n->type == QueryNodeType::TERM && n->mode == MatchMode::WHOLEFILENAME,
              "C8: fn: matches the whole file name");
    }
    {
        auto n = QueryNode::makeFilter("fu", "/usr/bin/ls");
        QueryFilterParser::parse(*n);
        check(n->type == QueryNodeType::TERM &&
              n->mode == MatchMode::WHOLEFILENAME &&
              n->target == MatchTarget::FULL_PATH,
              "C9: fu: matches the whole path");
    }
    {
        auto n = QueryNode::makeFilter("noext", "tmp,log");
        QueryFilterParser::parse(*n);
        check(n->type == QueryNodeType::NOT && n->children.size() == 1 &&
              n->children[0]->type == QueryNodeType::FILTER &&
              n->children[0]->filterName == "ext" &&
              n->children[0]->extList.size() == 2,
              "C10: noext: becomes NOT(ext:)");
    }
    {
        // Stackable modifiers: case:regex:^test_
        auto n = QueryNode::makeFilter("case", "regex:^test_");
        QueryFilterParser::parse(*n);
        check(n->type == QueryNodeType::TERM && n->mode == MatchMode::REGEX &&
              n->caseSensitive == true && n->text == "^test_",
              "C11: case:regex:^test_ stacks both modifiers");
    }
    {
        // ww:case:foo — order must not matter
        auto n = QueryNode::makeFilter("ww", "case:foo");
        QueryFilterParser::parse(*n);
        check(n->type == QueryNodeType::TERM && n->mode == MatchMode::WHOLEWORD &&
              n->caseSensitive == true && n->text == "foo",
              "C12: ww:case:foo stacks both modifiers");
    }
    {
        auto n = QueryNode::makeFilter("image", "");
        QueryFilterParser::parse(*n);
        check(n->filterName == "ext" && !n->extList.empty(),
              "C13: image: macro expands to an extension list");
    }
    {
        auto n = QueryNode::makeFilter("archive", "");
        QueryFilterParser::parse(*n);
        check(n->filterName == "ext" && !n->extList.empty(),
              "C14: archive: macro expands to an extension list");
    }
    {
        auto n = QueryNode::makeFilter("infile", "TODO");
        QueryFilterParser::parse(*n);
        check(n->filterName == "content" && n->filterArg == "TODO",
              "C15: infile: is an alias for content:");
    }
    {
        check(QueryFilterParser::isAggregateFilter("empty") &&
              QueryFilterParser::isAggregateFilter("dupe") &&
              QueryFilterParser::isAggregateFilter("dupe-min") &&
              QueryFilterParser::isAggregateFilter("dupe-max") &&
              QueryFilterParser::isAggregateFilter("childcount") &&
              QueryFilterParser::isAggregateFilter("child") &&
              !QueryFilterParser::isAggregateFilter("ext"),
              "C16: aggregate filter recognition");
    }
    {
        auto toks = QueryTokenizer::tokenize("name-length:>5");
        check(!toks.empty() && toks[0].type == TokenType::FILTER &&
              toks[0].filterName == "name-length",
              "C17: name-length: tokenizes as a filter");
    }
    {
        auto n = QueryNode::makeFilter("path-part", "/usr/local");
        QueryFilterParser::parse(*n);
        check(n->type == QueryNodeType::TERM &&
              n->target == MatchTarget::FULL_PATH,
              "C18: path-part: matches the full path");
    }

    // ═══════════════════════════════════════════
    // Section D: sort: reaches the AST
    // ═══════════════════════════════════════════

    {
        auto ast = QueryParser::parse("readme sort:size-descending");
        check(ast && ast->sort.has_value() &&
              ast->sort->field == SortField::Size &&
              ast->sort->dir == SortDir::Desc,
              "D1: trailing sort: lands on the AST root");
        if (ast && ast->sort) {
            // The sort modifier must not remain as a searchable term.
            bool hasSortFilter = false;
            std::function<void(const QueryNode&)> walk = [&](const QueryNode& n) {
                if (n.type == QueryNodeType::FILTER &&
                    SortSpec::isSortKeyword(n.filterName)) hasSortFilter = true;
                for (auto& c : n.children) if (c) walk(*c);
            };
            walk(*ast);
            check(!hasSortFilter, "D2: sort: is stripped from the searchable AST");
        }
    }
    {
        auto ast = QueryParser::parse("sort:name");
        check(ast && ast->sort.has_value() && ast->sort->field == SortField::Name,
              "D3: a bare sort: still yields a match-all query");
        if (ast) {
            check(ast->type == QueryNodeType::AND && ast->children.empty(),
                  "D4: bare sort: becomes an empty AND (match everything)");
        }
    }
    {
        auto ast = QueryParser::parse("foo sort:size sort:name");
        check(ast && ast->sort.has_value(),
              "D5: multiple sort: modifiers are tolerated (last wins)");
    }

    // ═══════════════════════════════════════════
    // Section E: end-to-end sorting over real records
    // ═══════════════════════════════════════════

    auto tmpDir = std::filesystem::temp_directory_path() / "me_everything_tests";
    std::filesystem::remove_all(tmpDir);
    std::filesystem::create_directories(tmpDir / "alpha");
    std::filesystem::create_directories(tmpDir / "beta");

    auto writeFile = [](const std::filesystem::path& p, size_t size) {
        std::ofstream f(p, std::ios::binary);
        std::string data(size, 'x');
        f.write(data.data(), data.size());
    };

    writeFile(tmpDir / "banana.txt", 300);
    writeFile(tmpDir / "apple.txt", 100);
    writeFile(tmpDir / "cherry.txt", 200);
    writeFile(tmpDir / "alpha" / "empty.txt", 0);
    writeFile(tmpDir / "alpha" / "dup.txt", 100);
    writeFile(tmpDir / "beta" / "dup.txt", 100);
    writeFile(tmpDir / "beta" / "large.log", 5000);
    // A directory with no children at all
    std::filesystem::create_directories(tmpDir / "beta" / "hollow");

    SearchEngine engine;
    {
        DirectoryScanner scanner;
        scanner.scan(tmpDir.string());
        auto records = scanner.takeResults();
        engine.loadRecords(std::move(records));
    }

    auto queryNames = [&](const std::string& q) -> std::vector<std::string> {
        QueryTimingInfo timing;
        auto indices = engine.queryAdvanced(q, 1000, false, timing);
        std::vector<std::string> names;
        engine.forEachRecordWithPath(indices, [&](uint32_t, const FileRecord& rec, const std::string&) {
            names.push_back(rec.name);
        });
        return names;
    };

    auto isSortedBy = [](const std::vector<uint64_t>& v) {
        return std::is_sorted(v.begin(), v.end());
    };

    // E1: sort:name ascending
    {
        auto names = queryNames("ext:txt sort:name");
        auto sorted = names;
        std::sort(sorted.begin(), sorted.end());
        check(names == sorted, "E1: ext:txt sort:name is alphabetical");
    }

    // E2: sort:-name descending
    {
        auto names = queryNames("ext:txt sort:-name");
        check(std::is_sorted(names.begin(), names.end(), std::greater<std::string>()),
              "E2: ext:txt sort:-name is reverse alphabetical");
    }

    // E3: sort:size ascending
    {
        QueryTimingInfo timing;
        auto idx = engine.queryAdvanced("ext:txt sort:size", 1000, false, timing);
        std::vector<uint64_t> sizes;
        engine.forEachRecordWithPath(idx, [&](uint32_t, const FileRecord& r, const std::string&) {
            sizes.push_back(r.size);
        });
        check(isSortedBy(sizes), "E3: ext:txt sort:size is ascending by size");
    }

    // E4: sort:size descending
    {
        QueryTimingInfo timing;
        auto idx = engine.queryAdvanced("ext:txt sort:size-descending", 1000, false, timing);
        std::vector<uint64_t> sizes;
        engine.forEachRecordWithPath(idx, [&](uint32_t, const FileRecord& r, const std::string&) {
            sizes.push_back(r.size);
        });
        check(std::is_sorted(sizes.begin(), sizes.end(), std::greater<uint64_t>()),
              "E4: ext:txt sort:size-descending is descending by size");
    }

    // E5: sort:size truncation keeps the largest N
    {
        QueryTimingInfo timing;
        auto idx = engine.queryAdvanced("ext:txt sort:-size", 2, false, timing);
        std::vector<uint64_t> sizes;
        engine.forEachRecordWithPath(idx, [&](uint32_t, const FileRecord& r, const std::string&) {
            sizes.push_back(r.size);
        });
        check(sizes.size() == 2 && sizes[0] >= sizes[1],
              "E5: sort:-size with limit 2 returns the two largest");
    }

    // E5b: multi-column sort:size;name
    {
        QueryTimingInfo timing;
        auto idx = engine.queryAdvanced("ext:txt sort:size;name", 1000, false, timing);
        struct Row { std::string name; uint64_t size; };
        std::vector<Row> rows;
        engine.forEachRecordWithPath(idx, [&](uint32_t, const FileRecord& r, const std::string&) {
            rows.push_back({r.name, r.size});
        });
        bool ordered = true;
        for (size_t i = 1; i < rows.size(); i++) {
            if (rows[i - 1].size > rows[i].size) ordered = false;
            else if (rows[i - 1].size == rows[i].size &&
                     rows[i - 1].name > rows[i].name) ordered = false;
        }
        check(ordered, "E5b: sort:size;name orders by size then name");
    }

    // E5c: per-column descending direction
    {
        SearchEngine multi;
        std::vector<FileRecord> recs;
        recs.push_back({"same.txt", "/m", 1, 100, 1000});
        recs.push_back({"same.txt", "/n", 1, 500, 2000});
        recs.push_back({"alpha.txt", "/m", 1, 300, 3000});
        multi.loadRecords(std::move(recs));

        QueryTimingInfo timing;
        auto idx = multi.queryAdvanced("sort:name;size-descending", 100, false, timing);
        std::vector<std::string> names;
        std::vector<uint64_t> sizes;
        multi.forEachRecordWithPath(idx, [&](uint32_t, const FileRecord& r, const std::string&) {
            names.push_back(r.name);
            sizes.push_back(r.size);
        });
        check(names.size() == 3 && names[0] == "alpha.txt" &&
              names[1] == "same.txt" && names[2] == "same.txt" &&
              sizes[1] == 500 && sizes[2] == 100,
              "E5c: sort:name;size-descending applies per-column direction");
    }

    // E5d: folders-first is applied before maxResults truncation
    {
        QueryTimingInfo timing;
        auto idx = engine.queryAdvanced("sort:name folders-first:", 2, false, timing);
        bool allFolders = !idx.empty();
        for (uint32_t i : idx) {
            if (engine.getRecord(i).type != 2) allFolders = false;
        }
        check(idx.size() == 2 && allFolders,
              "E5d: folders-first: leads the truncated window");
    }

    // E5e: bare folders-first: keeps relevance ordering but groups folders
    {
        QueryTimingInfo timing;
        auto idx = engine.queryAdvanced("folders-first:", 1, false, timing);
        check(idx.size() == 1 && engine.getRecord(idx[0]).type == 2,
              "E5e: folders-first: leads even with relevance ordering");
    }

    // E6: sort:path
    {
        QueryTimingInfo timing;
        auto idx = engine.queryAdvanced("ext:txt sort:path", 1000, false, timing);
        std::vector<std::string> paths;
        engine.forEachRecordWithPath(idx, [&](uint32_t, const FileRecord& r, const std::string& p) {
            paths.push_back(p + "/" + r.name);
        });
        auto sorted = paths;
        std::sort(sorted.begin(), sorted.end());
        check(paths == sorted, "E6: ext:txt sort:path is ordered by full path");
    }

    // E7: sort:type puts folders first
    {
        auto names = queryNames("sort:type");
        check(!names.empty(), "E7a: sort:type returns results");
        QueryTimingInfo timing;
        auto idx = engine.queryAdvanced("sort:type", 1000, false, timing);
        std::vector<uint8_t> types;
        engine.forEachRecordWithPath(idx, [&](uint32_t, const FileRecord& r, const std::string&) {
            types.push_back(r.type);
        });
        // Folders (type 2) must all precede files (type 1).
        bool foldersFirst = true;
        bool seenFile = false;
        for (uint8_t t : types) {
            if (t == 1) seenFile = true;
            if (t == 2 && seenFile) foldersFirst = false;
        }
        check(foldersFirst, "E7b: sort:type lists folders before files");
    }

    // E8: sort:ext
    {
        auto names = queryNames("sort:ext");
        check(!names.empty(), "E8: sort:ext returns results");
    }

    // E9: OR keyword end-to-end
    {
        auto names = queryNames("banana | apple");
        bool hasBanana = false, hasApple = false;
        for (auto& n : names) {
            if (n == "banana.txt") hasBanana = true;
            if (n == "apple.txt") hasApple = true;
        }
        check(hasBanana && hasApple, "E9: 'banana | apple' matches both");
    }

    // E10: () grouping end-to-end
    {
        auto names = queryNames("<banana|cherry> ext:txt");
        bool hasBanana = false, hasCherry = false, hasApple = false;
        for (auto& n : names) {
            if (n == "banana.txt") hasBanana = true;
            if (n == "cherry.txt") hasCherry = true;
            if (n == "apple.txt") hasApple = true;
        }
        check(hasBanana && hasCherry && !hasApple,
              "E10: '<banana|cherry> ext:txt' restricts correctly");
    }

    // E11: <> NOT end-to-end
    {
        auto names = queryNames("!banana ext:txt");
        bool hasBanana = false, hasApple = false;
        for (auto& n : names) {
            if (n == "banana.txt") hasBanana = true;
            if (n == "apple.txt") hasApple = true;
        }
        check(!hasBanana && hasApple, "E11: '!banana ext:txt' excludes banana");
    }

    // E12: ext: comma separator end-to-end
    {
        auto names = queryNames("ext:txt,log");
        bool hasTxt = false, hasLog = false;
        for (auto& n : names) {
            if (n.find(".txt") != std::string::npos) hasTxt = true;
            if (n == "large.log") hasLog = true;
        }
        check(hasTxt && hasLog, "E12: ext:txt,log matches both extensions");
    }

    // E13: noext: end-to-end
    {
        auto names = queryNames("ext:txt noext:txt");
        check(names.empty(), "E13: noext: excludes the extension");
    }
    {
        auto names = queryNames("sort:name noext:log");
        bool hasLog = false;
        for (auto& n : names) if (n == "large.log") hasLog = true;
        check(!hasLog, "E14: noext:log removes .log files");
    }

    // E15: size: hyphen range end-to-end
    {
        auto names = queryNames("size:150-250 ext:txt");
        bool ok = true;
        QueryTimingInfo timing;
        auto idx = engine.queryAdvanced("size:150-250 ext:txt", 1000, false, timing);
        engine.forEachRecordWithPath(idx, [&](uint32_t, const FileRecord& r, const std::string&) {
            if (r.size < 150 || r.size > 250) ok = false;
        });
        check(ok, "E15: size:150-250 only returns files in range");
    }

    // E16: fn: whole filename
    {
        auto names = queryNames("fn:apple.txt");
        check(names.size() == 1 && names[0] == "apple.txt",
              "E16: fn:apple.txt matches exactly one file");
    }

    // E17: fs: alias
    {
        auto names = queryNames("fs:>150 ext:txt");
        bool hasBanana = false, hasApple = false;
        for (auto& n : names) {
            if (n == "banana.txt") hasBanana = true;   // 300 bytes
            if (n == "apple.txt") hasApple = true;     // 100 bytes — excluded
        }
        check(hasBanana && !hasApple, "E17: fs:>150 behaves like size:>150");
    }

    // E18: case:regex: stacking end-to-end
    {
        auto names = queryNames("case:regex:^apple");
        bool hasApple = false;
        for (auto& n : names) if (n == "apple.txt") hasApple = true;
        check(hasApple, "E18: case:regex:^apple matches apple.txt");
    }

    // E19: empty: — zero-length files and childless folders
    {
        auto names = queryNames("empty:");
        bool hasEmptyTxt = false, hasHollow = false;
        for (auto& n : names) {
            if (n == "empty.txt") hasEmptyTxt = true;
            if (n == "hollow") hasHollow = true;
        }
        check(!hasEmptyTxt, "E19a: empty: does not match zero-byte files");
        check(hasHollow, "E19b: empty: finds the childless directory");
    }

    // E20: childcount:
    {
        QueryTimingInfo timing;
        auto idx = engine.queryAdvanced("childcount:0", 1000, false, timing);
        bool hasHollow = false, hasBeta = false;
        engine.forEachRecordWithPath(idx, [&](uint32_t, const FileRecord& r, const std::string&) {
            if (r.name == "hollow") hasHollow = true;
            if (r.name == "beta") hasBeta = true;   // beta has children
        });
        check(hasHollow && !hasBeta, "E20: childcount:0 finds only childless folders");
    }

    // E21: child:<name>
    {
        auto names = queryNames("child:dup.txt");
        bool hasAlpha = false, hasBeta = false;
        for (auto& n : names) {
            if (n == "alpha") hasAlpha = true;
            if (n == "beta") hasBeta = true;
        }
        check(hasAlpha && hasBeta, "E21: child:dup.txt finds folders containing it");
    }

    // E22: dupe:name
    {
        auto names = queryNames("dupe:name");
        int dupCount = 0;
        for (auto& n : names) if (n == "dup.txt") dupCount++;
        check(dupCount == 2, "E22: dupe:name finds both copies of dup.txt");
    }

    // E23: dupe: (default property = name)
    {
        auto names = queryNames("dupe:");
        int dupCount = 0;
        for (auto& n : names) if (n == "dup.txt") dupCount++;
        check(dupCount == 2, "E23: dupe: finds duplicates by name");
    }

    // E23b: dupe-min:/dupe-max: group-size filters
    {
        auto names = queryNames("dupe-min:2");
        int dupCount = 0;
        bool hasUnique = false;
        for (auto& n : names) {
            if (n == "dup.txt") dupCount++;
            if (n == "apple.txt") hasUnique = true;
        }
        check(dupCount == 2 && !hasUnique,
              "E23b: dupe-min:2 keeps only groups with two or more copies");
    }
    {
        auto names = queryNames("dupe-max:1");
        bool hasDup = false, hasUnique = false;
        for (auto& n : names) {
            if (n == "dup.txt") hasDup = true;
            if (n == "apple.txt") hasUnique = true;
        }
        check(!hasDup && hasUnique,
              "E23c: dupe-max:1 keeps only unique names");
    }

    // E23d: dupe-count: caps shown members per duplicate group
    {
        auto names = queryNames("dupe:name dupe-count:1");
        int dupCount = 0;
        for (auto& n : names) if (n == "dup.txt") dupCount++;
        check(dupCount == 1,
              "E23d: dupe-count:1 shows one member per duplicate group");
    }

    // E23e: dupe-from: keeps the whole group when one member is in the folder
    {
        auto names = queryNames("dupe:name dupe-from:" +
                                (tmpDir / "alpha").string());
        int dupCount = 0;
        for (auto& n : names) if (n == "dup.txt") dupCount++;
        check(dupCount == 2,
              "E23e: dupe-from: keeps every member of a matching group");
    }

    // E23f: dupe-from: wildcards and unrelated folders
    {
        auto names = queryNames("dupe:name dupe-from:" +
                                (tmpDir / "b*").string());
        int dupCount = 0;
        for (auto& n : names) if (n == "dup.txt") dupCount++;
        auto none = queryNames("dupe:name dupe-from:" +
                               (tmpDir / "hollow").string());
        check(dupCount == 2 && none.empty(),
              "E23f: dupe-from: supports wildcards and rejects unrelated folders");
    }

    // E24: type: media macro
    {
        auto names = queryNames("type:doc");
        bool hasTxt = false;
        for (auto& n : names) if (n.find(".txt") != std::string::npos) hasTxt = true;
        check(hasTxt, "E24: type:doc matches document extensions");
    }

    // E25: relevance ordering is unchanged without sort:
    {
        auto names = queryNames("apple");
        check(names.size() == 1 && names[0] == "apple.txt",
              "E25: plain queries still return relevance-ordered results");
    }

    // E26: sort: combined with a term filter
    {
        auto names = queryNames("txt sort:-size");
        check(!names.empty(), "E26: 'txt sort:-size' returns matches");
    }

    std::filesystem::remove_all(tmpDir);
}
