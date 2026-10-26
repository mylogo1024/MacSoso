#pragma once
#include "QueryAST.h"
#include "QueryParser.h"
#include "ASTStructuredTransform.h"
#include "ASTGlobTransform.h"
#include <vector>
#include <string>
#include <cstdlib>

enum class HintField : uint8_t {
    NAME = 0,
    PATH = 1,
    ANY  = 2,
};

struct HighlightHint {
    std::string text;
    HintField field = HintField::ANY;
    MatchMode mode = MatchMode::SUBSTRING;
    bool caseSensitive = false;
};

/// Walk the AST, collecting non-negated TERM nodes as highlight hints.
/// Skips NOT subtrees entirely (negated terms should not be highlighted).
inline void collectHighlightHints(const QueryNode& node,
                                  std::vector<HighlightHint>& hints) {
    switch (node.type) {
        case QueryNodeType::TERM: {
            if (node.text.empty()) break;
            // A full-path term can be split at its last separator: the
            // directory part highlights in the path, the trailing part in the
            // filename (Everything matches '/' terms against the full path).
            if (node.target == MatchTarget::FULL_PATH) {
                size_t slash = node.text.find_last_of('/');
                if (slash != std::string::npos) {
                    std::string dir = node.text.substr(0, slash);
                    std::string base = node.text.substr(slash + 1);
                    if (!dir.empty()) {
                        HighlightHint h;
                        h.text = std::move(dir);
                        h.field = HintField::PATH;
                        h.mode = node.mode;
                        h.caseSensitive = node.caseSensitive;
                        hints.push_back(std::move(h));
                    }
                    if (!base.empty()) {
                        HighlightHint h;
                        h.text = std::move(base);
                        h.field = HintField::NAME;
                        h.mode = node.mode;
                        h.caseSensitive = node.caseSensitive;
                        hints.push_back(std::move(h));
                    }
                    break;
                }
            }
            HighlightHint h;
            h.text = node.text;
            h.mode = node.mode;
            h.caseSensitive = node.caseSensitive;
            switch (node.target) {
                case MatchTarget::STEM:
                case MatchTarget::EXTENSION:
                    h.field = HintField::NAME;
                    break;
                case MatchTarget::LOCATION:
                case MatchTarget::PARENT_NAME:
                case MatchTarget::ANCESTOR_PATH:
                case MatchTarget::ANCESTOR_NAME:
                    h.field = HintField::PATH;
                    break;
                default:
                    if (node.nameOnly || node.typeConstraint == 1)
                        h.field = HintField::NAME;
                    else if (node.matchPath)
                        h.field = HintField::PATH;
                    else
                        h.field = HintField::ANY;
                    break;
            }
            hints.push_back(std::move(h));
            break;
        }
        case QueryNodeType::NOT:
            // Do not recurse — negated terms should not be highlighted
            break;
        case QueryNodeType::AND:
        case QueryNodeType::OR:
            for (auto& child : node.children) {
                collectHighlightHints(*child, hints);
            }
            break;
        case QueryNodeType::FILTER:
            if (node.filterName == "path" && !node.filterArg.empty()) {
                HighlightHint h;
                h.text = node.filterArg;
                h.field = HintField::PATH;
                h.mode = MatchMode::SUBSTRING;
                h.caseSensitive = false;
                hints.push_back(std::move(h));
            }
            if (node.filterName == "file" && !node.filterArg.empty()) {
                HighlightHint h;
                h.text = node.filterArg;
                h.field = HintField::NAME;
                h.mode = MatchMode::SUBSTRING;
                h.caseSensitive = false;
                hints.push_back(std::move(h));
            }
            break;
    }
}

/// Parse a query string and extract highlight hints.
/// Replicates the same pipeline as queryAdvanced():
///   preprocess → parse → transformSlashTerms → transformGlobTerms
inline std::vector<HighlightHint> extractHighlightHints(const std::string& query) {
    if (query.empty()) return {};

    // Strip whitespace
    auto start = query.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = query.find_last_not_of(" \t\r\n");
    std::string input = query.substr(start, end - start + 1);

    // Expand ~
    if (!input.empty() && input[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            if (input.size() == 1) input = home;
            else if (input[1] == '/') input = std::string(home) + input.substr(1);
        }
    }

    auto ast = QueryParser::parse(input);
    if (!ast) return {};

    ast = transformSlashTerms(std::move(ast));
    ast = transformGlobTerms(std::move(ast));

    std::vector<HighlightHint> hints;
    collectHighlightHints(*ast, hints);
    return hints;
}
