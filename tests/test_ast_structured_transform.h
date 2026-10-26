#pragma once
#include <cassert>
#include <functional>
#include <iostream>
#include <string>
#include "../MacSoSo/Core/ASTStructuredTransform.h"
#include "../MacSoSo/Core/QueryParser.h"

// ── Part 64: AST Slash (Full-Path) Transform Tests ──

static void runASTStructuredTransformTests() {
    std::cout << "── Part 64: AST Structured Transform ──\n";
    int localPassed = 0, localFailed = 0;

    auto check = [&](bool cond, const std::string& msg) {
        if (cond) { localPassed++; passed++; }
        else { localFailed++; failed++; std::cout << "  FAIL: " << msg << "\n"; }
    };

    // 64.1 A term containing '/' is retargeted to the full path
    {
        auto node = QueryNode::makeTerm("/usr/local/test", MatchMode::SUBSTRING);
        auto result = transformSlashTerms(std::move(node));
        check(result != nullptr, "64.1 non-null");
        check(result->type == QueryNodeType::TERM, "64.1 stays TERM");
        check(result->target == MatchTarget::FULL_PATH, "64.1 target FULL_PATH");
        check(result->matchPath == true, "64.1 matchPath=true");
        check(result->text == "/usr/local/test", "64.1 text unchanged");
    }

    // 64.2 No path separator: unchanged
    {
        auto node = QueryNode::makeTerm("test", MatchMode::SUBSTRING);
        auto result = transformSlashTerms(std::move(node));
        check(result != nullptr, "64.2 non-null");
        check(result->type == QueryNodeType::TERM, "64.2 type TERM");
        check(result->text == "test", "64.2 text unchanged");
        check(result->target == MatchTarget::NAME, "64.2 target NAME");
    }

    // 64.3 Trailing separator preserved (matches folder contents)
    {
        auto node = QueryNode::makeTerm("/usr/local/", MatchMode::SUBSTRING);
        auto result = transformSlashTerms(std::move(node));
        check(result != nullptr, "64.3 non-null");
        check(result->type == QueryNodeType::TERM, "64.3 type TERM");
        check(result->text == "/usr/local/", "64.3 trailing slash kept");
        check(result->target == MatchTarget::FULL_PATH, "64.3 target FULL_PATH");
    }

    // 64.4 Nested AND: only the slash term is retargeted
    {
        std::vector<std::unique_ptr<QueryNode>> kids;
        kids.push_back(QueryNode::makeTerm("/src/main", MatchMode::SUBSTRING));
        kids.push_back(QueryNode::makeFilter("ext", "cpp"));
        auto andNode = QueryNode::makeAnd(std::move(kids));

        auto result = transformSlashTerms(std::move(andNode));
        check(result != nullptr, "64.4 non-null");
        check(result->type == QueryNodeType::AND, "64.4 type AND");
        check(result->children.size() == 2, "64.4 two children");
        check(result->children[0]->type == QueryNodeType::TERM, "64.4 child0 TERM");
        check(result->children[0]->target == MatchTarget::FULL_PATH,
              "64.4 child0 target FULL_PATH");
        check(result->children[1]->type == QueryNodeType::FILTER, "64.4 child1 FILTER");
        check(result->children[1]->filterName == "ext", "64.4 child1 ext filter");
    }

    // 64.5 OR: both slash terms retargeted
    {
        std::vector<std::unique_ptr<QueryNode>> kids;
        kids.push_back(QueryNode::makeTerm("/lib/foo", MatchMode::SUBSTRING));
        kids.push_back(QueryNode::makeTerm("/src/bar", MatchMode::SUBSTRING));
        auto orNode = QueryNode::makeOr(std::move(kids));

        auto result = transformSlashTerms(std::move(orNode));
        check(result != nullptr, "64.5 non-null");
        check(result->type == QueryNodeType::OR, "64.5 type OR");
        check(result->children.size() == 2, "64.5 two children");
        for (int i = 0; i < 2; i++) {
            auto& child = result->children[i];
            std::string prefix = "64.5 child" + std::to_string(i);
            check(child->type == QueryNodeType::TERM, prefix + " type TERM");
            check(child->target == MatchTarget::FULL_PATH, prefix + " target FULL_PATH");
        }
    }

    // 64.6 Glob term with separators: target full path, mode preserved
    {
        auto node = QueryNode::makeTerm("/usr/*/test", MatchMode::SUBSTRING);
        auto result = transformSlashTerms(std::move(node));
        check(result != nullptr, "64.6 non-null");
        check(result->type == QueryNodeType::TERM, "64.6 type TERM");
        check(result->target == MatchTarget::FULL_PATH, "64.6 target FULL_PATH");
        check(result->text == "/usr/*/test", "64.6 wildcard text unchanged");
    }

    // 64.7 GLOB mode term: retargeted and glob recompiled after normalization
    {
        auto node = QueryNode::makeTerm("\\usr\\*\\test", MatchMode::GLOB);
        auto result = transformSlashTerms(std::move(node));
        check(result != nullptr, "64.7 non-null");
        check(result->type == QueryNodeType::TERM, "64.7 type TERM");
        check(result->mode == MatchMode::GLOB, "64.7 mode still GLOB");
        check(result->target == MatchTarget::FULL_PATH, "64.7 target FULL_PATH");
        check(result->text == "/usr/*/test", "64.7 backslashes normalized");
    }

    // 64.8 Leading separator, single segment
    {
        auto node = QueryNode::makeTerm("/test", MatchMode::SUBSTRING);
        auto result = transformSlashTerms(std::move(node));
        check(result != nullptr, "64.8 non-null");
        check(result->type == QueryNodeType::TERM, "64.8 type TERM");
        check(result->text == "/test", "64.8 text '/test'");
        check(result->target == MatchTarget::FULL_PATH, "64.8 target FULL_PATH");
    }

    // 64.9 Case sensitivity preserved
    {
        auto node = QueryNode::makeTerm("/usr/local/Test", MatchMode::SUBSTRING);
        node->caseSensitive = true;
        auto result = transformSlashTerms(std::move(node));
        check(result != nullptr, "64.9 non-null");
        check(result->caseSensitive == true, "64.9 caseSensitive preserved");
        check(result->text == "/usr/local/Test", "64.9 text case preserved");
        check(result->textLower == "/usr/local/test", "64.9 lower text normalized");
    }

    // 64.10 nullptr input → nullptr output
    {
        auto result = transformSlashTerms(nullptr);
        check(result == nullptr, "64.10 nullptr in → nullptr out");
    }

    // 64.11 Backslash habit: "\downloads" behaves like "/downloads"
    {
        auto node = QueryNode::makeTerm("\\downloads", MatchMode::SUBSTRING);
        auto result = transformSlashTerms(std::move(node));
        check(result != nullptr, "64.11 non-null");
        check(result->type == QueryNodeType::TERM, "64.11 type TERM");
        check(result->text == "/downloads", "64.11 backslash normalized");
        check(result->target == MatchTarget::FULL_PATH, "64.11 target FULL_PATH");
    }

    // 64.12 Integration with QueryParser: parse "/usr/local ext:cpp"
    {
        auto ast = QueryParser::parse("/usr/local ext:cpp");
        check(ast != nullptr, "64.12 parse non-null");
        ast = transformSlashTerms(std::move(ast));
        check(ast != nullptr, "64.12 transform non-null");
        check(ast->type == QueryNodeType::AND, "64.12 type AND");
        bool hasFullPath = false, hasExt = false;
        std::function<void(const QueryNode*)> scan;
        scan = [&](const QueryNode* n) {
            if (n->type == QueryNodeType::TERM && n->target == MatchTarget::FULL_PATH)
                hasFullPath = true;
            if (n->type == QueryNodeType::FILTER && n->filterName == "ext")
                hasExt = true;
            for (auto& c : n->children) scan(c.get());
        };
        scan(ast.get());
        check(hasFullPath, "64.12 has full-path TERM");
        check(hasExt, "64.12 has ext filter");
    }

    std::cout << "  Passed: " << localPassed << "  Failed: " << localFailed << "\n\n";
}
