#pragma once
#include "QueryAST.h"
#include <algorithm>
#include <memory>

/// Recursively walk an AST and retarget TERM nodes that contain a path
/// separator to the full path.
///
/// Everything treats '/' (and '\') as a path separator: a search term such as
/// "/usr/local" or "\downloads" is matched against the full path instead of
/// the filename. A trailing separator naturally restricts the match to items
/// *inside* that folder, because a folder's own full path has no trailing
/// separator (e.g. "/usr/local/" matches "/usr/local/src/main.cpp" but not
/// the full path "/usr/local").
///
/// Backslashes are normalized to '/' because macOS paths use '/'; this lets
/// Everything users keep their Windows-style habits ("\downloads").
inline std::unique_ptr<QueryNode> transformSlashTerms(std::unique_ptr<QueryNode> node) {
    if (!node) return nullptr;

    for (auto& child : node->children) {
        child = transformSlashTerms(std::move(child));
    }

    if (node->type != QueryNodeType::TERM) return node;
    // Regex escapes use '\' as a metacharacter — never normalize those.
    if (node->mode != MatchMode::SUBSTRING && node->mode != MatchMode::GLOB) return node;
    if (node->target != MatchTarget::NAME || node->matchPath || node->nameOnly) return node;

    bool hasSep = node->text.find('/') != std::string::npos ||
                  node->text.find('\\') != std::string::npos;
    if (!hasSep) return node;

    std::replace(node->text.begin(), node->text.end(), '\\', '/');
    std::replace(node->textLower.begin(), node->textLower.end(), '\\', '/');
    if (node->mode == MatchMode::GLOB) {
        node->compiledGlob = compileGlob(node->textLower);
    }
    node->target = MatchTarget::FULL_PATH;
    node->matchPath = true;
    node->nameOnly = false;
    return node;
}
