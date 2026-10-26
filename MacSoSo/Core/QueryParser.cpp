#include "QueryParser.h"
#include "QueryFilterParser.h"
#include <algorithm>

// ---------------------------------------------------------------------------
// Query-level modifier extraction
// ---------------------------------------------------------------------------

namespace {

/// Recursively pull `sort:` / `sorted:` FILTER nodes out of the AST and fold
/// them into a single query-level SortSpec stored on the root node.
/// Returns true when at least one sort modifier was found (and removed).
bool extractSortModifier(QueryNode& node, SortSpec& out) {
    if (node.type == QueryNodeType::FILTER && SortSpec::isSortKeyword(node.filterName)) {
        out = SortSpec::parse(node.filterArg);
        return true;   // signal caller to drop this node
    }

    bool found = false;
    for (auto& child : node.children) {
        if (!child) continue;
        if (extractSortModifier(*child, out)) {
            child.reset();
            found = true;
        }
    }

    if (found) {
        // Drop the removed nodes.
        node.children.erase(
            std::remove_if(node.children.begin(), node.children.end(),
                           [](const std::unique_ptr<QueryNode>& c) { return !c; }),
            node.children.end());
        // A NOT node with no children would match everything (wrong); give it
        // a match-all child so the negation yields nothing instead.
        if (node.type == QueryNodeType::NOT && node.children.empty()) {
            node.children.push_back(QueryNode::makeAnd({}));
        }
    }
    return found;
}

/// Pull `count:` / `distinct:` / `unique:` FILTER nodes off the AST and fold
/// them into query-level result controls on the root. Returns true when any
/// were found (and removed).
bool extractResultModifiers(QueryNode& node,
                            uint64_t& limit, std::string& distinctBy,
                            bool& uniqueMode, bool& foundDistinct) {
    auto isResult = [](const std::string& f) {
        return f == "count" || f == "distinct" || f == "unique";
    };
    if (node.type == QueryNodeType::FILTER && isResult(node.filterName)) {
        if (node.filterName == "count") {
            size_t i = 0;
            const auto& a = node.filterArg;
            while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) i++;
            if (i > 0) limit = static_cast<uint64_t>(std::strtoull(a.substr(0, i).c_str(), nullptr, 10));
        } else {
            uniqueMode = (node.filterName == "unique");
            distinctBy = node.filterArg;   // property list; empty = name
            foundDistinct = true;
        }
        return true;
    }
    bool found = false;
    for (auto& child : node.children) {
        if (!child) continue;
        if (extractResultModifiers(*child, limit, distinctBy, uniqueMode, foundDistinct)) {
            child.reset();
            found = true;
        }
    }
    if (found) {
        node.children.erase(
            std::remove_if(node.children.begin(), node.children.end(),
                           [](const std::unique_ptr<QueryNode>& c) { return !c; }),
            node.children.end());
        if (node.type == QueryNodeType::NOT && node.children.empty()) {
            node.children.push_back(QueryNode::makeAnd({}));
        }
    }
    return found;
}

/// Pull `sort-ascending:` / `sort-descending:` / `folders-first:` controls off
/// the AST and fold them into `spec`.
bool extractSortControls(QueryNode& node, SortSpec& spec, bool& foundAny) {
    if (node.type == QueryNodeType::FILTER) {
        const std::string& f = node.filterName;
        if (f == "sort-ascending" || f == "sort-descending") {
            spec.dir = (f == "sort-ascending") ? SortDir::Asc : SortDir::Desc;
            foundAny = true;
            return true;
        }
        if (f == "folders-first" || f == "folders-first-ascending") {
            spec.foldersFirst = true;
            foundAny = true;
            return true;
        }
        if (f == "folders-first-descending") {
            spec.foldersFirst = true;
            spec.dir = SortDir::Desc;
            foundAny = true;
            return true;
        }
        if (f == "no-folders-first") {
            spec.foldersFirst = false;
            foundAny = true;
            return true;
        }
    }
    bool found = false;
    for (auto& child : node.children) {
        if (!child) continue;
        if (extractSortControls(*child, spec, foundAny)) {
            child.reset();
            found = true;
        }
    }
    if (found) {
        node.children.erase(
            std::remove_if(node.children.begin(), node.children.end(),
                           [](const std::unique_ptr<QueryNode>& c) { return !c; }),
            node.children.end());
        if (node.type == QueryNodeType::NOT && node.children.empty()) {
            node.children.push_back(QueryNode::makeAnd({}));
        }
    }
    return found;
}

/// Collect and remove `?modifier:` FILTER nodes (global modifiers).
/// Returns true when `node` itself was removed.
bool collectGlobalModifiers(QueryNode& node, std::vector<std::string>& mods) {
    if (node.type == QueryNodeType::FILTER &&
        !node.filterName.empty() && node.filterName[0] == '?') {
        mods.push_back(node.filterName.substr(1));
        return true;
    }
    bool found = false;
    for (auto& child : node.children) {
        if (!child) continue;
        if (collectGlobalModifiers(*child, mods)) {
            child.reset();
            found = true;
        }
    }
    if (found) {
        node.children.erase(
            std::remove_if(node.children.begin(), node.children.end(),
                           [](const std::unique_ptr<QueryNode>& c) { return !c; }),
            node.children.end());
        // Collapse single-child AND/OR groups left behind by removal.
        while ((node.type == QueryNodeType::AND || node.type == QueryNodeType::OR) &&
               node.children.size() == 1) {
            auto only = std::move(node.children[0]);
            node = std::move(*only);
        }
        if (node.type == QueryNodeType::NOT && node.children.empty()) {
            node.children.push_back(QueryNode::makeAnd({}));
        }
        if ((node.type == QueryNodeType::AND || node.type == QueryNodeType::OR) &&
            node.children.empty()) {
            auto replacement = QueryNode::makeAnd({});
            node = std::move(*replacement);
        }
    }
    return false;
}

void applyGlobalModifiersToTerms(QueryNode& node,
                                 const std::vector<std::string>& mods) {
    if (node.type == QueryNodeType::TERM) {
        for (const auto& m : mods) {
            QueryFilterParser::applyModifierToTerm(node, m);
        }
        return;
    }
    for (auto& child : node.children) {
        if (child) applyGlobalModifiersToTerms(*child, mods);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::unique_ptr<QueryNode> QueryParser::parse(const std::string& input) {
    auto tokens = QueryTokenizer::tokenize(input);
    // If only END token, nothing to parse
    if (tokens.size() <= 1) return nullptr;

    QueryParser parser(std::move(tokens));
    auto root = parser.parseAndExpr();
    if (!root) return nullptr;

    // Global `?modifier:` directives apply to the whole query (Everything
    // applies them from their position onward; a leading directive, which is
    // how they are normally written, is equivalent).
    std::vector<std::string> globalMods;
    if (collectGlobalModifiers(*root, globalMods)) {
        root = QueryNode::makeAnd({});
    }
    if (!globalMods.empty()) {
        applyGlobalModifiersToTerms(*root, globalMods);
    }

    // Pull the sort modifier off the AST. When it was the only term the query
    // degenerates to "match everything" — mirroring Everything, where
    // `sort:size` lists the whole index ordered by size.
    SortSpec spec;
    bool haveSort = extractSortModifier(*root, spec);
    if (haveSort) {
        if (root->type == QueryNodeType::FILTER && SortSpec::isSortKeyword(root->filterName)) {
            root = QueryNode::makeAnd({});
        }
    }
    bool haveSortControls = false;
    extractSortControls(*root, spec, haveSortControls);
    if (haveSortControls) {
        spec.specified = true;
    }
    if (haveSort || haveSortControls) {
        if (spec.isDefault() && spec.foldersFirst) {
            // `folders-first:` alone keeps the relevance ordering but groups
            // folders in front of files.
            spec.specified = true;
            spec.field = SortField::Relevance;
        }
        root->sort = spec;
    }

    // Pull query-level result controls (count:/distinct:/unique:) onto the root.
    uint64_t limit = 0;
    std::string distinctBy;
    bool uniqueMode = false;
    bool foundDistinct = false;
    if (extractResultModifiers(*root, limit, distinctBy, uniqueMode, foundDistinct)) {
        if (root->type == QueryNodeType::FILTER &&
            (root->filterName == "count" || root->filterName == "distinct" ||
             root->filterName == "unique")) {
            root = QueryNode::makeAnd({});
        }
        if (limit > 0) root->limit = limit;
        if (foundDistinct) {
            // An empty argument means "name", per Everything's distinct:/unique:.
            root->distinctBy = distinctBy;
            root->uniqueMode = uniqueMode;
        }
    }

    return root;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

QueryParser::QueryParser(std::vector<Token>&& tokens)
    : tokens_(std::move(tokens)) {}

// ---------------------------------------------------------------------------
// Recursive descent
//
// Everything's default operator precedence is OR before AND:
//   apple banana | orange  ==  apple AND (banana OR orange)
// Space is the AND operator; < > change the evaluation order.
// ---------------------------------------------------------------------------

// and_expr ::= (or_expr)*
std::unique_ptr<QueryNode> QueryParser::parseAndExpr() {
    std::vector<std::unique_ptr<QueryNode>> kids;

    // Keep parsing while the next token can start an atom. An empty group
    // (`<>`) yields no node and is simply skipped.
    while (!atEnd() && peek().type != TokenType::PIPE
           && peek().type != TokenType::RANGLE) {
        auto next = parseOrExpr();
        if (next) kids.push_back(std::move(next));
    }

    if (kids.empty()) return nullptr;
    if (kids.size() == 1) return std::move(kids[0]);
    return QueryNode::makeAnd(std::move(kids));
}

// or_expr ::= not_expr ('|' not_expr)*
std::unique_ptr<QueryNode> QueryParser::parseOrExpr() {
    auto left = parseNotExpr();
    if (peek().type != TokenType::PIPE) return left;

    std::vector<std::unique_ptr<QueryNode>> kids;
    if (left) kids.push_back(std::move(left));
    while (match(TokenType::PIPE)) {
        auto right = parseNotExpr();
        if (right) kids.push_back(std::move(right));
        // A dangling pipe with no right side keeps the left branch.
    }

    if (kids.empty()) return nullptr;
    if (kids.size() == 1) return std::move(kids[0]);
    return QueryNode::makeOr(std::move(kids));
}

// not_expr ::= '!' not_expr | atom
std::unique_ptr<QueryNode> QueryParser::parseNotExpr() {
    if (peek().type == TokenType::BANG) {
        advance();
        auto child = parseNotExpr();
        if (!child) {
            // Bare '!' at end of input — treat as literal text
            return QueryNode::makeTerm("!");
        }
        return QueryNode::makeNot(std::move(child));
    }
    return parseAtom();
}

// atom ::= '<' and_expr '>'
//        | QUOTED
//        | FILTER
//        | WORD
std::unique_ptr<QueryNode> QueryParser::parseAtom() {
    if (atEnd()) return nullptr;

    const Token& tok = peek();

    // Grouping: < and_expr >
    if (tok.type == TokenType::LANGLE) {
        advance();
        auto inner = parseAndExpr();
        // Consume closing '>' if present (tolerant of missing)
        match(TokenType::RANGLE);
        return inner;
    }

    // Stray '>' — end of the current group.
    if (tok.type == TokenType::RANGLE) {
        advance();
        return nullptr;
    }

    // Quoted phrase: everything inside is plain text.
    if (tok.type == TokenType::QUOTED) {
        advance();
        auto node = QueryNode::makeTerm(tok.value);
        node->quoted = true;
        node->literalWild = true;   // no wildcard interpretation
        return node;
    }

    // Filter: ext:cpp, size:>1mb, etc.
    if (tok.type == TokenType::FILTER) {
        advance();
        // `?modifier:` is a global directive. It is represented as a marker
        // FILTER node that is stripped after parsing and applied to every TERM.
        if (tok.global && QueryFilterParser::isTextModifierName(tok.filterName)) {
            auto marker = QueryNode::makeFilter("?" + tok.filterName, "");
            if (tok.filterArg.empty()) return marker;

            // `?path:system32` also carries a normal term.
            auto term = QueryNode::makeFilter(tok.filterName, tok.filterArg);
            QueryFilterParser::parse(*term);
            std::vector<std::unique_ptr<QueryNode>> kids;
            kids.push_back(std::move(marker));
            kids.push_back(std::move(term));
            return QueryNode::makeAnd(std::move(kids));
        }
        auto node = QueryNode::makeFilter(tok.filterName, tok.filterArg);
        QueryFilterParser::parse(*node);
        return node;
    }

    // Plain word
    if (tok.type == TokenType::WORD) {
        advance();
        return QueryNode::makeTerm(tok.value);
    }

    // Unexpected token — skip it
    advance();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Token helpers
// ---------------------------------------------------------------------------

const Token& QueryParser::peek() const {
    return tokens_[pos_];
}

const Token& QueryParser::advance() {
    const Token& tok = tokens_[pos_];
    if (pos_ < tokens_.size() - 1) pos_++;
    return tok;
}

bool QueryParser::match(TokenType type) {
    if (peek().type == type) {
        advance();
        return true;
    }
    return false;
}

bool QueryParser::atEnd() const {
    return tokens_[pos_].type == TokenType::END;
}
