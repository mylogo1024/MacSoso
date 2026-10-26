#pragma once
#include "QueryAST.h"
#include "QueryDateParser.h"
#include "StringUtils.h"
#include <string>
#include <vector>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>

/// Parse filter arguments and populate structured fields on QueryNode.
///
/// Implements the voidtools Everything 1.5 search modifiers and functions:
///   modifiers: case:, no-case:, diacritics:, ignore-punctuation:,
///              ignore-whitespace:, regex:, wildcards:, no-wildcards:,
///              path:/no-path:, file:/folder:, prefix:/suffix:,
///              start-with:/end-with:, whole:/whole-words:, dot-all:,
///              multi-line:, ungreedy:
///   functions: name:, stem:, extension:, ext:, full-path:, parent-name:,
///              location:/loc:, ancestor:, ancestor-name:, parent:, root:
///   numeric:   size:, len:, depth:, child-count: and friends
///   dates:     dm:/dc:/da: plus the segmented date functions
///
/// Modifiers and functions can be chained, e.g.
///   case:regex:extension:^MP
///   folder:case:wholewords:path:FOO
///   whole:extension:c
class QueryFilterParser {
public:
    enum class DateSegmentKind : int {
        HOUR = 0, MINUTE = 1, DAY = 2, DOW = 3,
        MONTH = 4, YEAR = 5, DATE = 6, TIME = 7,
    };

    /// True when the filter name is a query-level modifier that the parser
    /// strips off the AST (sort:). Returns false for normal filters.
    static bool isQueryLevelModifier(const std::string& name) {
        return SortSpec::isSortKeyword(name);
    }

    /// True when the filter needs per-record aggregate data (child counts,
    /// duplicate detection) that is not available during per-record evaluation.
    static bool isAggregateFilter(const std::string& name) {
        return name == "empty" || name == "childcount" ||
               name == "child-count" || name == "child-file-count" ||
               name == "child-folder-count" ||
               name == "child" || name == "child-file" || name == "child-folder" ||
               name == "dupe" || name == "dupe-min" || name == "dupe-max" ||
               name == "dupe-count" || name == "dupe-from";
    }

    /// Text modifiers alter how the attached search term is matched.
    static bool isTextModifierName(const std::string& lower) {
        return lower == "case" || lower == "nocase" || lower == "no-case" ||
               lower == "diacritics" || lower == "diac" ||
               lower == "no-diacritics" || lower == "nodiagnostics" ||
               lower == "ignore-punctuation" || lower == "ignorepunctuation" ||
               lower == "nopunc" ||
               lower == "no-ignore-punctuation" ||
               lower == "ignore-whitespace" || lower == "ignorewhitespace" ||
               lower == "nows" ||
               lower == "no-ignore-whitespace" ||
               lower == "regex" ||
               lower == "wildcards" || lower == "no-wildcards" ||
               lower == "nowildcards" ||
               lower == "path" || lower == "no-path" || lower == "\\" ||
               lower == "file" || lower == "folder" ||
               lower == "prefix" || lower == "no-prefix" ||
               lower == "suffix" || lower == "no-suffix" ||
               lower == "start-with" || lower == "startwith" || lower == "^" ||
               lower == "end-with" || lower == "endwith" || lower == "$" ||
               lower == "whole" || lower == "whole-filename" ||
               lower == "wholefilename" || lower == "wfn" ||
               lower == "entire" || lower == "fn" || lower == "fu" ||
               lower == "whole-words" || lower == "wholeword" ||
               lower == "ww" || lower == "no-whole-words" ||
               lower == "nowholewords" ||
               lower == "dot-all" || lower == "dotall" ||
               lower == "multi-line" || lower == "multiline" ||
               lower == "ungreedy";
    }

    /// Text functions select which part of the record the term is matched
    /// against. ext:/content: are function-like but produce FILTER nodes.
    static bool isTextFunctionName(const std::string& lower) {
        return lower == "name" || lower == "stem" ||
               lower == "extension" || lower == "ext" ||
               lower == "full-path" || lower == "fullpath" ||
               lower == "path-part" || lower == "pathpart" ||
               lower == "parent-name" || lower == "parentname" ||
               lower == "location" || lower == "loc" ||
               lower == "ancestor" || lower == "ancestor-name" ||
               lower == "content" || lower == "infile" ||
               lower == "len" || lower == "size" || lower == "fs" ||
               lower == "depth" || lower == "childcount" ||
               lower == "child-count" || lower == "child-file-count" ||
               lower == "child-folder-count" || lower == "name-length" ||
               lower == "namelen" || lower == "full-path-length" ||
               lower == "fullpathlen" || isDateFilterName(lower) ||
               isDateSegmentName(lower);
    }

    /// Apply a single modifier to an existing TERM node. Used for global
    /// `?modifier:` directives, which apply to every term in the query.
    static void applyModifierToTerm(QueryNode& node, const std::string& modifier) {
        if (node.type != QueryNodeType::TERM) return;
        applyModifier(node, me::toLower(modifier));
    }

    /// Post-process a FILTER node to parse its arguments.
    static void parse(QueryNode& node) {
        if (node.type != QueryNodeType::FILTER) return;

        const std::string name = me::toLower(node.filterName);
        const std::string arg = node.filterArg;

        // --- Query-level sort modifier: the parser strips it later ----------
        if (SortSpec::isSortKeyword(name)) {
            node.filterName = "sort";
            node.sort = SortSpec::parse(arg);
            return;
        }
        // Sort controls are folded into the SortSpec by QueryParser; keep them
        // intact here so extractSortControls can still recognise them.
        if (name == "sort-ascending" || name == "sort-descending" ||
            name == "folders-first" || name == "folders-first-ascending" ||
            name == "folders-first-descending" || name == "no-folders-first") {
            node.filterName = name;
            node.filterArg.clear();
            return;
        }

        // --- Query-level result controls -----------------------------------
        if (name == "count") {
            node.filterArg = arg;
            return;
        }
        if (name == "distinct" || name == "unique") {
            node.filterName = (name == "unique") ? "unique" : "distinct";
            node.filterArg = arg;
            return;
        }

        // --- ext / noext ----------------------------------------------------
        if (name == "ext") {
            parseExt(node, arg);
            return;
        }
        if (name == "noext") {
            auto extNode = QueryNode::makeFilter("ext", arg);
            parseExt(*extNode, arg);
            node.type = QueryNodeType::NOT;
            node.children.push_back(std::move(extNode));
            node.filterName.clear();
            node.filterArg.clear();
            return;
        }

        // --- Numeric filters -------------------------------------------------
        if (name == "size" || name == "fs") {
            parseSize(node, arg);
            return;
        }
        if (name == "len" || name == "depth" || name == "childcount" ||
            name == "child-count" || name == "child-file-count" ||
            name == "child-folder-count" || name == "name-length" ||
            name == "namelen" || name == "full-path-length" ||
            name == "fullpathlen" || name == "dupe-count" ||
            name == "dupe-min" || name == "dupe-max") {
            parseNumeric(node, arg);
            return;
        }

        // --- Dates -----------------------------------------------------------
        if (isDateFilterName(name)) {
            if (arg.empty()) {
                makeTautology(node);
            } else {
                node.filterName = normalizeDateFilterName(name);
                QueryDateParser::parse(node, arg);
            }
            return;
        }
        if (isDateSegmentName(name)) {
            parseDateSegment(node, name, arg);
            return;
        }

        // --- Everything media type macros -----------------------------------
        if (name == "audio" || name == "video" || name == "pic" ||
            name == "doc" || name == "exe" || name == "zip" ||
            name == "image" || name == "archive") {
            expandMacro(node, macroExtensions(name));
            return;
        }

        // --- Legacy structured filters --------------------------------------
        if (name == "type") {
            return;   // engine interprets filterArg directly
        }
        if (name == "nopath") {
            node.filterArg = me::toLower(arg);
            return;
        }
        if (name == "parent") {
            std::string p = me::toLower(arg);
            while (p.size() > 1 && p.back() == '/') p.pop_back();
            node.filterArg = p;
            return;
        }
        if (name == "root" || name == "empty" || name == "child" ||
            name == "child-file" || name == "child-folder" ||
            name == "dupe" || name == "dupe-from") {
            node.filterArg = arg;   // engine interprets it
            return;
        }

        // --- Text modifiers / functions (also file:/folder: and path:) ------
        if (isTextModifierName(name) || isTextFunctionName(name)) {
            parseTextChain(node, name, arg);
            return;
        }

        // Unknown filter — match-all (forward compatible).
        makeTautology(node);
    }

    // -----------------------------------------------------------------------
    // Date segments
    // -----------------------------------------------------------------------

    /// Parse segmented date filters (dm-hour:14, dm-month:december,
    /// dm-day-of-week:monday, dm-day:25, dm-time:10am..2pm).
    static void parseDateSegment(QueryNode& node, const std::string& name,
                                 const std::string& arg) {
        const DateSegmentKind kind = dateSegmentKindOf(name);
        node.dateSegment = static_cast<int>(kind);

        std::string lower = me::toLower(arg);
        auto dotdot = lower.find("..");
        if (dotdot != std::string::npos) {
            node.op = CompareOp::RANGE;
            node.numVal1 = parseSegmentValue(kind, lower.substr(0, dotdot));
            node.numVal2 = parseSegmentValue(kind, lower.substr(dotdot + 2));
            return;
        }

        // Comparison operators (dm-hour:>12, dm-year:>=2020).
        size_t valStart = 0;
        CompareOp op = CompareOp::EQ;
        if (lower.size() >= 2 && lower[0] == '>' && lower[1] == '=') { op = CompareOp::GE; valStart = 2; }
        else if (lower.size() >= 2 && lower[0] == '<' && lower[1] == '=') { op = CompareOp::LE; valStart = 2; }
        else if (lower.size() >= 2 && lower[0] == '!' && lower[1] == '=') { op = CompareOp::NE; valStart = 2; }
        else if (!lower.empty() && lower[0] == '>') { op = CompareOp::GT; valStart = 1; }
        else if (!lower.empty() && lower[0] == '<') { op = CompareOp::LT; valStart = 1; }
        else if (!lower.empty() && lower[0] == '!') { op = CompareOp::NE; valStart = 1; }
        else if (!lower.empty() && lower[0] == '=') { op = CompareOp::EQ_EXACT; valStart = 1; }

        node.op = op;
        node.numVal1 = parseSegmentValue(kind, lower.substr(valStart));
    }

    static uint64_t parsePlainInt(const std::string& s) {
        if (s.empty()) return 0;
        size_t i = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++;
        if (i == 0) return 0;
        return static_cast<uint64_t>(std::strtoull(s.substr(0, i).c_str(), nullptr, 10));
    }

    static uint64_t monthNumber(const std::string& lower) {
        static const char* months[] = {"january","february","march","april","may","june",
            "july","august","september","october","november","december"};
        for (int m = 0; m < 12; m++) {
            if (lower == months[m] ||
                lower == std::string(months[m]).substr(0, 3)) return m + 1;
        }
        uint64_t n = parsePlainInt(lower);
        return (n >= 1 && n <= 12) ? n : 0;
    }

    static uint64_t dayOfWeekNumber(const std::string& lower) {
        static const char* days[] = {"sunday","monday","tuesday","wednesday",
            "thursday","friday","saturday"};
        for (int d = 0; d < 7; d++) {
            if (lower == days[d] ||
                lower == std::string(days[d]).substr(0, 3)) return d; // 0=Sunday
        }
        uint64_t n = parsePlainInt(lower);
        return (n <= 6) ? n : 0;
    }

private:
    // -----------------------------------------------------------------------
    // Small helpers
    // -----------------------------------------------------------------------

    static bool startsWith(const std::string& s, const char* prefix) {
        size_t n = std::strlen(prefix);
        return s.size() >= n && s.compare(0, n, prefix) == 0;
    }

    static bool endsWith(const std::string& s, const char* suffix) {
        size_t n = std::strlen(suffix);
        return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
    }

    static std::string normalizeDateFilterName(const std::string& name) {
        if (name == "date-modified") return "dm";
        if (name == "date-created") return "dc";
        if (name == "date-accessed") return "da";
        return name;
    }

    static bool isDateFilterName(const std::string& name) {
        return name == "dm" || name == "datemodified" || name == "date-modified" ||
               name == "dc" || name == "datecreated" || name == "date-created" ||
               name == "da" || name == "dateaccessed" || name == "date-accessed" ||
               name == "dr" || name == "daterun";
    }

    static bool isDateSegmentName(const std::string& name) {
        return dateSegmentKindOf(name) != DateSegmentKind::HOUR ||
               isHourSegmentName(name);
    }

    static bool isHourSegmentName(const std::string& name) {
        return name.find("hour") != std::string::npos;
    }

    static DateSegmentKind dateSegmentKindOf(const std::string& name) {
        if (name.find("minute") != std::string::npos) return DateSegmentKind::MINUTE;
        if (name.find("hour") != std::string::npos) return DateSegmentKind::HOUR;
        if (name.find("day-of-week") != std::string::npos ||
            name.find("dow") != std::string::npos) return DateSegmentKind::DOW;
        if (name.find("day") != std::string::npos) return DateSegmentKind::DAY;
        if (name.find("month") != std::string::npos) return DateSegmentKind::MONTH;
        if (name.find("year") != std::string::npos) return DateSegmentKind::YEAR;
        if (name.find("time") != std::string::npos) return DateSegmentKind::TIME;
        if (name.find("date") != std::string::npos) return DateSegmentKind::DATE;
        return DateSegmentKind::HOUR;
    }

    static uint64_t parseSegmentValue(DateSegmentKind kind, const std::string& raw) {
        std::string lower = me::toLower(raw);
        switch (kind) {
            case DateSegmentKind::DOW:   return dayOfWeekNumber(lower);
            case DateSegmentKind::MONTH: return monthNumber(lower);
            case DateSegmentKind::DATE:  return parseDateValue(lower);
            case DateSegmentKind::TIME:  return parseTimeValue(lower);
            default:                     return parsePlainInt(lower);
        }
    }

    /// YYYY-MM-DD / YYYY/MM/DD → YYYYMMDD integer for easy comparison.
    static uint64_t parseDateValue(const std::string& s) {
        if (s.empty()) return 0;
        int y = 0, m = 0, d = 0;
        if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) == 3 ||
            std::sscanf(s.c_str(), "%d/%d/%d", &y, &m, &d) == 3) {
            return static_cast<uint64_t>(y) * 10000 +
                   static_cast<uint64_t>(m) * 100 +
                   static_cast<uint64_t>(d);
        }
        return parsePlainInt(s);
    }

    /// 10am / 2:30pm / 17:30 / 17:30:15 → seconds since midnight.
    static uint64_t parseTimeValue(const std::string& s) {
        if (s.empty()) return 0;
        int h = 0, m = 0, sec = 0;
        bool pm = false, am = false;
        std::string t = s;
        if (t.size() >= 2) {
            std::string tail = t.substr(t.size() - 2);
            if (tail == "pm") { pm = true; t = t.substr(0, t.size() - 2); }
            else if (tail == "am") { am = true; t = t.substr(0, t.size() - 2); }
        }
        if (std::sscanf(t.c_str(), "%d:%d:%d", &h, &m, &sec) < 1) {
            if (std::sscanf(t.c_str(), "%d:%d", &h, &m) < 1) {
                h = static_cast<int>(parsePlainInt(t));
            }
        }
        if (pm && h < 12) h += 12;
        if (am && h == 12) h = 0;
        return static_cast<uint64_t>(h) * 3600 +
               static_cast<uint64_t>(m) * 60 +
               static_cast<uint64_t>(sec);
    }

    static void makeTautology(QueryNode& node) {
        node.type = QueryNodeType::FILTER;
        node.filterName = "__tautology";
        node.filterArg.clear();
        node.children.clear();
        node.extList.clear();
    }

    // -----------------------------------------------------------------------
    // Extension lists
    // -----------------------------------------------------------------------

    static bool isExtSeparator(char c) {
        return c == ';' || c == ',' || c == '|';
    }

    /// Parse ext:cpp, ext:cpp;h;hpp, ext:*.txt, ext:mp*.
    /// Leading '.', '*.' and '*' prefixes are ignored, as in Everything.
    /// An empty list means "files without an extension".
    /// Original case is preserved so `case:ext:CPP` can distinguish it; the
    /// evaluator lowercases for the default case-insensitive comparison.
    static void parseExt(QueryNode& node, const std::string& arg) {
        node.op = CompareOp::EQ;
        node.extList.clear();
        size_t start = 0;
        for (size_t i = 0; i <= arg.size(); i++) {
            if (i == arg.size() || isExtSeparator(arg[i])) {
                if (i > start) {
                    std::string e = arg.substr(start, i - start);
                    while (!e.empty() && (e.front() == ' ' || e.front() == '\t')) e.erase(e.begin());
                    while (!e.empty() && (e.back() == ' ' || e.back() == '\t')) e.pop_back();
                    if (e == "*" || e == "*.*") {
                        e = "*";
                    } else {
                        if (endsWith(e, "*.")) e.resize(e.size() - 2);
                        while (!e.empty() && (e[0] == '.' || e[0] == '*')) {
                            e.erase(e.begin());
                            if (!e.empty() && e[0] == '.') e.erase(e.begin());
                        }
                    }
                    if (!e.empty()) node.extList.push_back(std::move(e));
                }
                start = i + 1;
            }
        }
        // ext: with no argument (or only separators) = files with no extension.
    }

    // -----------------------------------------------------------------------
    // Numeric / size parsing
    // -----------------------------------------------------------------------

    static void parseSize(QueryNode& node, const std::string& arg) {
        parseNumericWithUnits(node, arg, true);
    }

    static void parseNumeric(QueryNode& node, const std::string& arg) {
        parseNumericWithUnits(node, arg, false);
    }

    struct NumericValue {
        double value = 0;
        uint64_t multiplier = 1;
        bool hasUnit = false;
        bool ok = false;
    };

    static NumericValue parseNumericValue(const std::string& s) {
        NumericValue n;
        if (s.empty()) return n;
        size_t numEnd = 0;
        while (numEnd < s.size() &&
               (std::isdigit(static_cast<unsigned char>(s[numEnd])) || s[numEnd] == '.')) {
            numEnd++;
        }
        if (numEnd == 0) return n;
        n.value = std::strtod(s.substr(0, numEnd).c_str(), nullptr);
        std::string unit = me::toLower(s.substr(numEnd));
        if (unit == "kb" || unit == "k") n.multiplier = 1024ULL;
        else if (unit == "mb" || unit == "m") n.multiplier = 1024ULL * 1024;
        else if (unit == "gb" || unit == "g") n.multiplier = 1024ULL * 1024 * 1024;
        else if (unit == "tb" || unit == "t") n.multiplier = 1024ULL * 1024 * 1024 * 1024;
        else if (unit.empty() || unit == "b") n.multiplier = 1;
        else return n;   // unknown unit
        n.hasUnit = !unit.empty() && unit != "b";
        n.ok = true;
        return n;
    }

    static uint64_t parseValueWithUnit(const std::string& s) {
        NumericValue n = parseNumericValue(s);
        if (!n.ok) return 0;
        return static_cast<uint64_t>(n.value * static_cast<double>(n.multiplier));
    }

    /// Supports: >N, >=N, <N, <=N, =N, ==N, !=N, !N, N..M, N-M.
    /// With `granularity`, a bare value with a size unit denotes the whole
    /// unit bucket, exactly like Everything (size:1kb == 1024..2047).
    static void parseNumericWithUnits(QueryNode& node, const std::string& arg,
                                      bool granularity) {
        if (arg.empty()) {
            makeTautology(node);
            return;
        }

        auto dotdot = arg.find("..");
        if (dotdot != std::string::npos) {
            node.op = CompareOp::RANGE;
            node.numVal1 = parseValueWithUnit(arg.substr(0, dotdot));
            node.numVal2 = parseValueWithUnit(arg.substr(dotdot + 2));
            return;
        }

        // Everything hyphen range: 100kb-5mb.
        size_t hyphen = arg.find('-', 1);
        if (hyphen != std::string::npos && hyphen + 1 < arg.size()) {
            std::string lo = arg.substr(0, hyphen);
            std::string hi = arg.substr(hyphen + 1);
            if (startsWithDigit(lo) && startsWithDigit(hi)) {
                node.op = CompareOp::RANGE;
                node.numVal1 = parseValueWithUnit(lo);
                node.numVal2 = parseValueWithUnit(hi);
                return;
            }
        }

        size_t valStart = 0;
        CompareOp op = CompareOp::EQ;
        if (arg.size() >= 2 && arg[0] == '=' && arg[1] == '=') {
            op = CompareOp::EQ_EXACT; valStart = 2;
        } else if (arg.size() >= 2 && arg[0] == '>' && arg[1] == '=') {
            op = CompareOp::GE; valStart = 2;
        } else if (arg.size() >= 2 && arg[0] == '<' && arg[1] == '=') {
            op = CompareOp::LE; valStart = 2;
        } else if (arg.size() >= 2 && arg[0] == '!' && arg[1] == '=') {
            op = CompareOp::NE; valStart = 2;
        } else if (arg[0] == '>') {
            op = CompareOp::GT; valStart = 1;
        } else if (arg[0] == '<') {
            op = CompareOp::LT; valStart = 1;
        } else if (arg[0] == '=') {
            op = CompareOp::EQ_EXACT; valStart = 1;
        } else if (arg[0] == '!') {
            op = CompareOp::NE; valStart = 1;
        }

        std::string valueText = arg.substr(valStart);
        NumericValue nv = parseNumericValue(valueText);

        if (op == CompareOp::EQ && granularity && nv.ok && nv.hasUnit &&
            nv.multiplier > 1 && nv.value >= 0) {
            uint64_t lo = static_cast<uint64_t>(nv.value * static_cast<double>(nv.multiplier));
            uint64_t hi = lo + nv.multiplier - 1;
            node.op = CompareOp::RANGE;
            node.numVal1 = lo;
            node.numVal2 = hi;
            return;
        }
        if (op == CompareOp::NE && granularity && nv.ok && nv.hasUnit &&
            nv.multiplier > 1) {
            uint64_t lo = static_cast<uint64_t>(nv.value * static_cast<double>(nv.multiplier));
            node.op = CompareOp::NOT_RANGE;
            node.numVal1 = lo;
            node.numVal2 = lo + nv.multiplier - 1;
            return;
        }

        node.op = op;
        node.numVal1 = nv.ok
            ? static_cast<uint64_t>(nv.value * static_cast<double>(nv.multiplier))
            : parseValueWithUnit(valueText);
    }

    static bool startsWithDigit(const std::string& s) {
        return !s.empty() && std::isdigit(static_cast<unsigned char>(s[0])) != 0;
    }

    // -----------------------------------------------------------------------
    // Modifiers
    // -----------------------------------------------------------------------

    static void applyModifier(QueryNode& node, const std::string& m) {
        if (m == "case") {
            node.caseSensitive = true;
        } else if (m == "nocase" || m == "no-case") {
            node.caseSensitive = false;
        } else if (m == "diacritics" || m == "diac") {
            node.stripDiacritics = false;
        } else if (m == "no-diacritics") {
            node.stripDiacritics = true;
        } else if (m == "ignore-punctuation" || m == "ignorepunctuation" || m == "nopunc") {
            node.ignorePunct = true;
        } else if (m == "no-ignore-punctuation") {
            node.ignorePunct = false;
        } else if (m == "ignore-whitespace" || m == "ignorewhitespace" || m == "nows") {
            node.ignoreSpace = true;
        } else if (m == "no-ignore-whitespace") {
            node.ignoreSpace = false;
        } else if (m == "regex") {
            node.mode = MatchMode::REGEX;
            node.textLower.clear();
        } else if (m == "wildcards") {
            if (node.mode == MatchMode::SUBSTRING) node.mode = MatchMode::GLOB;
        } else if (m == "no-wildcards" || m == "nowildcards") {
            node.literalWild = true;
            if (node.mode == MatchMode::GLOB) node.mode = MatchMode::SUBSTRING;
        } else if (m == "path" || m == "\\") {
            if (node.target == MatchTarget::NAME || node.target == MatchTarget::STEM) {
                node.target = MatchTarget::FULL_PATH;
                node.matchPath = true;
                node.nameOnly = false;
            }
        } else if (m == "no-path") {
            if (node.target == MatchTarget::FULL_PATH) {
                node.target = MatchTarget::NAME;
                node.matchPath = false;
                node.nameOnly = true;
            }
        } else if (m == "file") {
            node.typeConstraint = 1;
        } else if (m == "folder") {
            node.typeConstraint = 2;
        } else if (m == "prefix") {
            // Everything's prefix:/suffix: functions match the beginning/end
            // of the filename, not individual words (whole-words: does that).
            node.mode = MatchMode::STARTSWITH;
        } else if (m == "no-prefix") {
            if (node.mode == MatchMode::STARTSWITH) node.mode = MatchMode::SUBSTRING;
        } else if (m == "suffix") {
            node.mode = MatchMode::ENDSWITH;
        } else if (m == "no-suffix") {
            if (node.mode == MatchMode::ENDSWITH) node.mode = MatchMode::SUBSTRING;
        } else if (m == "start-with" || m == "startwith" || m == "^") {
            node.mode = MatchMode::STARTSWITH;
        } else if (m == "end-with" || m == "endwith" || m == "$") {
            node.mode = MatchMode::ENDSWITH;
        } else if (m == "whole" || m == "whole-filename" ||
                   m == "wholefilename" || m == "wfn" ||
                   m == "entire" || m == "fn") {
            node.mode = MatchMode::WHOLEFILENAME;
            if (m == "fn" || m == "wfn") {
                node.target = MatchTarget::NAME;
                node.nameOnly = true;
                node.matchPath = false;
            }
        } else if (m == "fu") {
            node.mode = MatchMode::WHOLEFILENAME;
            node.target = MatchTarget::FULL_PATH;
            node.matchPath = true;
            node.nameOnly = false;
        } else if (m == "whole-words" || m == "wholeword" || m == "ww") {
            node.mode = MatchMode::WHOLEWORD;
        } else if (m == "no-whole-words" || m == "nowholewords") {
            if (node.mode == MatchMode::WHOLEWORD) node.mode = MatchMode::SUBSTRING;
        } else if (m == "dot-all" || m == "dotall") {
            if (node.regexFlags.find('s') == std::string::npos) node.regexFlags += 's';
        } else if (m == "multi-line" || m == "multiline") {
            if (node.regexFlags.find('m') == std::string::npos) node.regexFlags += 'm';
        } else if (m == "ungreedy") {
            if (node.regexFlags.find('U') == std::string::npos) node.regexFlags += 'U';
        }
    }

    /// Build the AST for `[modifier:]*[function:]<text>`.
    static void parseTextChain(QueryNode& node, const std::string& first,
                               const std::string& rest) {
        std::vector<std::string> mods;
        std::string func;
        std::string text = rest;

        if (isTextFunctionName(first)) {
            func = first;
        } else {
            mods.push_back(first);
            // Innermost function consumes the remainder. Unknown names are
            // literal text, so `case:foo:bar` searches for "foo:bar".
            while (true) {
                auto colon = text.find(':');
                if (colon == std::string::npos || colon == 0) break;
                std::string head = me::toLower(text.substr(0, colon));
                std::string after = text.substr(colon + 1);
                if (isTextModifierName(head)) {
                    mods.push_back(head);
                    text = std::move(after);
                    continue;
                }
                if (isTextFunctionName(head)) {
                    func = head;
                    text = std::move(after);
                }
                break;
            }
        }

        // ext: and content: produce FILTER nodes, not terms.
        if (func == "ext") {
            auto f = QueryNode::makeFilter("ext", text);
            parseExt(*f, text);
            for (auto it = mods.rbegin(); it != mods.rend(); ++it) {
                if (*it == "case") f->caseSensitive = true;
                else if (*it == "nocase" || *it == "no-case") f->caseSensitive = false;
                else if (*it == "diacritics" || *it == "diac") f->stripDiacritics = false;
                else if (*it == "no-diacritics") f->stripDiacritics = true;
            }
            node = std::move(*f);
            return;
        }
        if (func == "content" || func == "infile") {
            node.filterName = "content";
            node.filterArg = text;
            return;
        }
        if (func == "len" || func == "size" || func == "fs" ||
            func == "depth" || func == "childcount" ||
            func == "child-count" || func == "child-file-count" ||
            func == "child-folder-count" || func == "name-length" ||
            func == "namelen" || func == "full-path-length" ||
            func == "fullpathlen" || isDateFilterName(func) ||
            isDateSegmentName(func)) {
            node.filterName = func;
            node.filterArg = text;
            parse(node);
            return;
        }

        auto term = QueryNode::makeTerm(text);
        if (func == "name") {
            term->target = MatchTarget::NAME;
            term->nameOnly = true;
        } else if (func == "stem") {
            term->target = MatchTarget::STEM;
            term->nameOnly = true;
        } else if (func == "extension") {
            term->target = MatchTarget::EXTENSION;
            term->typeConstraint = 1;
        } else if (func == "full-path" || func == "fullpath" ||
                   func == "path-part" || func == "pathpart") {
            term->target = MatchTarget::FULL_PATH;
            term->matchPath = true;
        } else if (func == "parent-name" || func == "parentname") {
            term->target = MatchTarget::PARENT_NAME;
        } else if (func == "location" || func == "loc") {
            term->target = MatchTarget::LOCATION;
        } else if (func == "ancestor") {
            term->target = MatchTarget::ANCESTOR_PATH;
        } else if (func == "ancestor-name") {
            term->target = MatchTarget::ANCESTOR_NAME;
        }

        // Apply modifiers from the inside out; the leftmost modifier wins.
        for (auto it = mods.rbegin(); it != mods.rend(); ++it) {
            applyModifier(*term, *it);
        }

        // `extension:` with an empty argument matches files that have *an*
        // extension (Everything uses !extension: for files without one).
        if (term->text.empty() && term->target == MatchTarget::EXTENSION &&
            term->mode == MatchMode::SUBSTRING) {
            node = std::move(*term);
            return;
        }

        if (term->text.empty()) {
            if (term->typeConstraint == 1) {
                node.type = QueryNodeType::FILTER;
                node.filterName = "file";
                node.filterArg.clear();
                node.children.clear();
                return;
            }
            if (term->typeConstraint == 2) {
                node.type = QueryNodeType::FILTER;
                node.filterName = "folder";
                node.filterArg.clear();
                node.children.clear();
                return;
            }
            // A bare modifier (e.g. `case:` or `path:`) is a no-op term.
            makeTautology(node);
            return;
        }

        if (term->mode != MatchMode::REGEX) {
            term->textLower = me::toLower(term->text);
        }
        node = std::move(*term);
    }

    // -----------------------------------------------------------------------
    // Macros
    // -----------------------------------------------------------------------

    static const char* macroExtensions(const std::string& name) {
        if (name == "audio")   return "mp3;wav;flac;aac;ogg;m4a;wma;alac;aiff;aif;opus;mid;midi";
        if (name == "video")   return "mp4;avi;mkv;mov;wmv;flv;webm;m4v;mpg;mpeg;3gp;ts;vob";
        if (name == "pic" || name == "image")
                               return "jpg;jpeg;png;gif;bmp;tiff;tif;webp;svg;ico;heic;heif;raw;psd;ai";
        if (name == "doc")     return "pdf;doc;docx;xls;xlsx;ppt;pptx;txt;rtf;odt;ods;odp;csv;md;pages;numbers;key;epub;mobi";
        if (name == "exe")     return "app;dmg;pkg;sh;command;csh;action";
        if (name == "zip" || name == "archive")
                               return "zip;rar;7z;tar;gz;bz2;xz;tgz;zst;lz4;cab;iso;dmg";
        return "";
    }

    static void expandMacro(QueryNode& node, const std::string& extString) {
        node.filterName = "ext";
        node.filterArg = extString;
        parseExt(node, extString);
    }
};
