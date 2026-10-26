#pragma once
#include <string>
#include <vector>
#include <memory>
#include <sstream>
#include "StructuredQueryParser.h"
#include "StringUtils.h"
#include "CompiledGlob.h"
#include "QuerySort.h"
#include <optional>

/// AST node types for the Everything-style query parser.
enum class QueryNodeType {
    TERM,       // text match (substring / glob / regex / wholeword / wholefilename)
    AND,        // all children must match
    OR,         // any child matches
    NOT,        // negate a single child
    FILTER,     // function filter (ext:, size:, dm:, file:, folder:, etc.)
};

/// Comparison operator for filter nodes.
enum class CompareOp {
    EQ,        // equality (granularity-aware / exact depending on context)
    LT, LE, GT, GE, RANGE,
    EQ_EXACT,  // == : exact value match
    NE,        // != / ! : value does not match
    NOT_RANGE, // !1kb : value outside the unit bucket
};

/// How a TERM node matches against text.
enum class MatchMode {
    SUBSTRING, GLOB, REGEX, WHOLEWORD, WHOLEFILENAME, WHOLEPATH,
    STARTSWITH,  // start-with: / startwith: / ^:
    ENDSWITH,    // end-with: / endwith: / $:
    STEM,        // stem: — match the filename without its extension
    WORDPREFIX,  // prefix: — match the start of a word
    WORDSUFFIX,  // suffix: — match the end of a word
};

/// Which text a TERM node is matched against. Everything 1.5 exposes these
/// as search functions (name:, stem:, extension:, full-path:, parent-name:,
/// location:, ancestor-name:) and as modifiers (path:).
enum class MatchTarget : uint8_t {
    NAME,           // file / folder name (the Everything default)
    STEM,           // name without its extension
    EXTENSION,      // text after the final dot
    FULL_PATH,      // parent path + "/" + name
    PARENT_NAME,    // name of the immediate parent folder
    LOCATION,       // parent folder path
    ANCESTOR_NAME,  // any ancestor folder name
    ANCESTOR_PATH,  // exact ancestor folder path
};

/// A single node in the parsed query AST.
struct QueryNode {
    QueryNodeType type;

    // --- TERM fields ---
    std::string text;
    std::string textLower;  // pre-computed lowercase of text (avoids per-record toLower)
    MatchMode mode = MatchMode::SUBSTRING;
    MatchTarget target = MatchTarget::NAME;
    bool caseSensitive = false;
    bool matchPath = false;  // path: modifier (target == FULL_PATH)
    bool nameOnly = false;   // match name only (set by transformSlashTerms)
    bool quoted = false;     // "..." — plain text, no wildcard/slash transforms
    bool ignorePunct = false;   // ignore-punctuation: modifier
    bool ignoreSpace = false;   // ignore-whitespace: modifier
    bool stripDiacritics = false; // diacritics: modifier
    bool literalWild = false;   // no-wildcards: — treat * ? as literal
    uint8_t typeConstraint = 0; // 0=any, 1=files only, 2=folders only
    std::string regexFlags;     // regex option flags (dot-all/multi-line/ungreedy)
    std::optional<CompiledGlob> compiledGlob;  // pre-compiled glob for zero-alloc matching

    // --- AND / OR / NOT: children ---
    std::vector<std::unique_ptr<QueryNode>> children;

    // --- FILTER fields ---
    std::string filterName;  // "ext", "size", "dm", etc.
    std::string filterArg;   // raw argument string
    CompareOp op = CompareOp::EQ;
    uint64_t numVal1 = 0, numVal2 = 0;
    std::vector<std::string> extList;  // for ext: filter
    std::optional<int> dateSegment;    // -1=none; see DateSegmentKind in QueryFilterParser.h

    // --- Path segment constraint (for __pathseg internal filter) ---
    std::vector<PathSegment> pathSegments;
    QueryMode structuredMode = QueryMode::PLAIN;

    // --- Query-level sort request (populated on the root node by `sort:`) ---
    // A single query may contain one sort modifier; it applies to the whole
    // result set, so it lives on the root rather than on individual terms.
    std::optional<SortSpec> sort;

    // --- Query-level result controls ---
    std::optional<uint64_t> limit;       // count: — cap the number of results
    std::optional<std::string> distinctBy; // distinct:/unique: — dedupe key property
    bool uniqueMode = false;             // unique: (true) vs dupe-style distinct: (false)

    // --- Factory helpers ---

    static std::unique_ptr<QueryNode> makeTerm(const std::string& text,
                                                MatchMode mode = MatchMode::SUBSTRING) {
        auto n = std::make_unique<QueryNode>();
        n->type = QueryNodeType::TERM;
        n->text = text;
        n->textLower = me::toLower(text);
        n->mode = mode;
        return n;
    }

    /// Overload with pre-computed lowercase to avoid redundant me::toLower().
    static std::unique_ptr<QueryNode> makeTerm(const std::string& text,
                                                const std::string& precomputedLower,
                                                MatchMode mode) {
        auto n = std::make_unique<QueryNode>();
        n->type = QueryNodeType::TERM;
        n->text = text;
        n->textLower = precomputedLower;
        n->mode = mode;
        return n;
    }

    static std::unique_ptr<QueryNode> makeAnd(std::vector<std::unique_ptr<QueryNode>>&& kids) {
        auto n = std::make_unique<QueryNode>();
        n->type = QueryNodeType::AND;
        n->children = std::move(kids);
        return n;
    }

    static std::unique_ptr<QueryNode> makeOr(std::vector<std::unique_ptr<QueryNode>>&& kids) {
        auto n = std::make_unique<QueryNode>();
        n->type = QueryNodeType::OR;
        n->children = std::move(kids);
        return n;
    }

    static std::unique_ptr<QueryNode> makeNot(std::unique_ptr<QueryNode> child) {
        auto n = std::make_unique<QueryNode>();
        n->type = QueryNodeType::NOT;
        n->children.push_back(std::move(child));
        return n;
    }

    static std::unique_ptr<QueryNode> makeFilter(const std::string& name,
                                                  const std::string& arg) {
        auto n = std::make_unique<QueryNode>();
        n->type = QueryNodeType::FILTER;
        n->filterName = name;
        n->filterArg = arg;
        return n;
    }

    /// Debug representation of the AST.
    std::string dump(int indent = 0) const {
        std::string pad(indent * 2, ' ');
        std::ostringstream os;
        if (sort && indent == 0) {
            os << pad << "SORT(" << sort->fieldName() << ", "
               << sort->dirName() << ")\n";
        }
        switch (type) {
            case QueryNodeType::TERM:
                os << pad << "TERM(\"" << text << "\")";
                if (mode == MatchMode::GLOB) os << " mode=glob";
                else if (mode == MatchMode::REGEX) os << " mode=regex";
                else if (mode == MatchMode::WHOLEWORD) os << " mode=wholeword";
                else if (mode == MatchMode::WHOLEFILENAME) os << " mode=wholefilename";
                else if (mode == MatchMode::WHOLEPATH) os << " mode=wholepath";
                else if (mode == MatchMode::STARTSWITH) os << " mode=startswith";
                else if (mode == MatchMode::ENDSWITH) os << " mode=endswith";
                else if (mode == MatchMode::WORDPREFIX) os << " mode=wordprefix";
                else if (mode == MatchMode::WORDSUFFIX) os << " mode=wordsuffix";
                if (target != MatchTarget::NAME) {
                    switch (target) {
                        case MatchTarget::STEM: os << " target=stem"; break;
                        case MatchTarget::EXTENSION: os << " target=ext"; break;
                        case MatchTarget::FULL_PATH: os << " target=fullpath"; break;
                        case MatchTarget::PARENT_NAME: os << " target=parentname"; break;
                        case MatchTarget::LOCATION: os << " target=location"; break;
                        case MatchTarget::ANCESTOR_NAME: os << " target=ancestor-name"; break;
                        case MatchTarget::ANCESTOR_PATH: os << " target=ancestor"; break;
                        default: break;
                    }
                }
                if (typeConstraint == 1) os << " file";
                else if (typeConstraint == 2) os << " folder";
                if (caseSensitive) os << " case";
                if (quoted) os << " quoted";
                if (matchPath) os << " path";
                break;
            case QueryNodeType::AND:
                os << pad << "AND(\n";
                for (auto& c : children) os << c->dump(indent + 1) << "\n";
                os << pad << ")";
                break;
            case QueryNodeType::OR:
                os << pad << "OR(\n";
                for (auto& c : children) os << c->dump(indent + 1) << "\n";
                os << pad << ")";
                break;
            case QueryNodeType::NOT:
                os << pad << "NOT(\n";
                if (!children.empty()) os << children[0]->dump(indent + 1) << "\n";
                os << pad << ")";
                break;
            case QueryNodeType::FILTER:
                os << pad << "FILTER(" << filterName << ":" << filterArg << ")";
                break;
        }
        return os.str();
    }
};
