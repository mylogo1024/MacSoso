#pragma once
#include <string>
#include <vector>
#include <cctype>
#include <string_view>
#include <unordered_set>
#include "StringUtils.h"
#include "QuerySort.h"

/// Token types produced by the query tokenizer.
enum class TokenType {
    WORD,       // plain text token
    QUOTED,     // text inside double quotes
    PIPE,       // | (OR operator)
    BANG,       // ! (NOT operator)
    LANGLE,     // < (group open)
    RANGLE,     // > (group close)
    LPAREN,     // reserved — '(' is literal text in Everything
    RPAREN,     // reserved — ')' is literal text in Everything
    AND_KW,     // reserved — AND is not an operator in Everything
    FILTER,     // known_function:value (e.g., ext:cpp)
    END,        // end of input
};

/// A single token from the query string.
struct Token {
    TokenType type;
    std::string value;       // text content (for WORD, QUOTED, FILTER)
    std::string filterName;  // filter name portion (for FILTER type only)
    std::string filterArg;   // filter argument portion (for FILTER type only)
    bool global = false;     // ?modifier: — applies to the remainder of the query
};

/// Lexer for Everything query syntax.
///
/// Everything operators are only: space (AND), | (OR), ! (NOT) and < > for
/// grouping. Double quotes make their contents plain text. AND / OR / NOT are
/// ordinary words, and parentheses are ordinary filename characters.
class QueryTokenizer {
public:
    static std::vector<Token> tokenize(const std::string& input) {
        std::vector<Token> tokens;
        size_t i = 0;
        size_t len = input.size();

        while (i < len) {
            char c = input[i];

            // Skip whitespace
            if (c == ' ' || c == '\t') {
                i++;
                continue;
            }

            // Single-character operators
            if (c == '|') {
                tokens.push_back({TokenType::PIPE, "|"});
                i++;
                continue;
            }
            if (c == '!') {
                tokens.push_back({TokenType::BANG, "!"});
                i++;
                continue;
            }
            if (c == '<') {
                tokens.push_back({TokenType::LANGLE, "<"});
                i++;
                continue;
            }
            if (c == '>') {
                tokens.push_back({TokenType::RANGLE, ">"});
                i++;
                continue;
            }

            // Quoted string
            if (c == '"') {
                i++; // skip opening quote
                std::string text;
                while (i < len && input[i] != '"') {
                    text += input[i];
                    i++;
                }
                if (i < len) i++; // skip closing quote
                tokens.push_back({TokenType::QUOTED, text});
                continue;
            }

            // Word or filter: collect until whitespace or operator.
            std::string word;
            size_t wordStart = i;
            while (i < len) {
                char ch = input[i];
                if (ch == ' ' || ch == '\t' || ch == '|' || ch == '!' ||
                    ch == '<' || ch == '>' || ch == '"') {
                    break;
                }
                word += ch;
                i++;
            }
            if (word.empty()) continue;

            bool global = false;
            std::string nameForLookup;
            auto colonPos = word.find(':');
            if (colonPos != std::string::npos && colonPos > 0) {
                std::string name = word.substr(0, colonPos);
                if (!name.empty() && name[0] == '?') {
                    global = true;
                    name.erase(name.begin());
                }
                std::string lowerName = me::toLower(name);

                if (!lowerName.empty() && isKnownFilter(lowerName)) {
                    // Re-collect the argument from the original input so that
                    // quoted sections may contain spaces (Everything allows
                    // parent-name:"Program Files").
                    size_t argStart = wordStart + colonPos + 1;
                    i = argStart;
                    std::string arg;
                    while (i < len) {
                        char ch = input[i];
                        if (ch == '"') {
                            i++;
                            while (i < len && input[i] != '"') {
                                arg += input[i];
                                i++;
                            }
                            if (i < len) i++;
                            continue;
                        }
                        if (ch == ' ' || ch == '\t' || ch == '|') break;
                        // < > ! are grouping/negation operators, but they are
                        // also valid at the start of a numeric comparison
                        // (size:>1mb, size:!1kb).
                        if ((ch == '<' || ch == '>' || ch == '!') && !arg.empty()) break;
                        arg += ch;
                        i++;
                    }
                    Token t;
                    t.type = TokenType::FILTER;
                    t.value = lowerName + ":" + arg;
                    t.filterName = lowerName;
                    t.filterArg = arg;
                    t.global = global;
                    tokens.push_back(std::move(t));
                    continue;
                }
            }
            tokens.push_back({TokenType::WORD, word});
        }

        tokens.push_back({TokenType::END, ""});
        return tokens;
    }

    /// Quick check: does the query string contain any advanced syntax?
    /// Used to decide whether to invoke the parser or use the fast path.
    static bool hasAdvancedSyntax(const std::string& q) {
        for (size_t i = 0; i < q.size(); i++) {
            char c = q[i];
            if (c == '|' || c == '!' || c == '<' || c == '>' || c == '"') {
                return true;
            }
            // Check for known filter prefix: word followed by colon
            if (c == ':' && i > 0) {
                // Extract the word before the colon
                size_t start = i;
                while (start > 0 && !std::isspace(static_cast<unsigned char>(q[start - 1]))
                       && q[start - 1] != '|' && q[start - 1] != '!'
                       && q[start - 1] != '<' && q[start - 1] != '>') {
                    start--;
                }
                std::string name;
                for (size_t j = start; j < i; j++) {
                    char ch = q[j];
                    if (ch == '?' && name.empty()) continue;   // global modifier
                    name += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
                if (isKnownFilter(name)) return true;
            }
        }
        return false;
    }

private:
    static bool isKnownFilter(const std::string& name) {
        static const std::unordered_set<std::string> filters = {
            // Phase 2 filters
            "ext", "size", "file", "folder",
            "path", "nopath", "parent", "depth", "len",
            // Phase 3 filters
            "dm", "dc", "da",
            "datemodified", "datecreated", "dateaccessed",
            "date-modified", "date-created", "date-accessed",
            // Phase 4 modifiers (treated as filters by tokenizer)
            "case", "nocase", "regex", "ww", "wfn",
            "wholeword", "wholefilename", "whole-words",
            // Phase 4 macros
            "audio", "video", "pic", "doc", "exe", "zip",
            "image", "archive",
            // Content
            "content", "infile",
            // Type shorthand
            "type",
            // Sorting (Everything: sort: / sorted: / sortby:)
            "sort", "sorted", "sortby", "orderby",
            "sort-ascending", "sort-descending",
            "folders-first", "no-folders-first",
            "folders-first-ascending", "folders-first-descending",
            // Everything 1.5 modifier aliases
            "no-case", "no-diacritics", "diac", "diacritics",
            "ignore-punctuation", "ignorepunctuation", "nopunc",
            "no-ignore-punctuation", "no-ignore-whitespace",
            "ignore-whitespace", "ignorewhitespace", "nows",
            "wildcards", "no-wildcards", "nowildcards",
            "whole", "whole-filename", "entire",
            "whole-words", "no-whole-words", "nowholewords",
            "prefix", "no-prefix", "suffix", "no-suffix",
            "start-with", "startwith", "^",
            "end-with", "endwith", "$",
            "path", "no-path", "\\",
            "dot-all", "dotall", "multi-line", "multiline", "ungreedy",
            // Everything filename / full-path / file-size aliases
            "fn", "fu", "fs",
            // Everything: exclusion and emptiness filters
            "noext", "empty",
            // Everything: duplicate detection
            "dupe", "dupe-count", "dupe-min", "dupe-max", "dupe-from",
            // Everything: child / child-count filters
            "child", "childcount",
            "child-count", "child-file", "child-folder",
            "child-file-count", "child-folder-count",
            // Everything: run history
            "rc", "run", "dr", "daterun",
            // Everything: filename / full-path variants & hierarchy
            "name",
            "stem",
            "extension", "extcontains",
            "full-path", "fullpath",
            "path-part", "pathpart", "parent-name", "parentname",
            "location", "loc",
            "ancestor", "ancestor-name", "root",
            // Everything: length aliases
            "name-length", "namelen", "full-path-length", "fullpathlen",
            // Everything: segmented date filters
            "dmdate", "dmday", "dmdow", "dmhour", "dmminute",
            "dmmonth", "dmtime", "dmyear",
            "dcdate", "dcday", "dcdow", "dchour", "dcminute",
            "dcmonth", "dctime", "dcyear",
            "dadate", "daday", "dadow", "dahour", "daminute",
            "damonth", "datime", "dayear",
            "date-modified-date", "date-modified-day",
            "date-modified-day-of-week", "date-modified-hour",
            "date-modified-minute", "date-modified-month",
            "date-modified-time", "date-modified-year",
            "date-created-date", "date-created-day",
            "date-created-day-of-week", "date-created-hour",
            "date-created-minute", "date-created-month",
            "date-created-time", "date-created-year",
            "date-accessed-date", "date-accessed-day",
            "date-accessed-day-of-week", "date-accessed-hour",
            "date-accessed-minute", "date-accessed-month",
            "date-accessed-time", "date-accessed-year",
            // Everything: result controls
            "count", "distinct", "unique", "distinct-sort",
        };
        return filters.count(name) > 0;
    }
};
