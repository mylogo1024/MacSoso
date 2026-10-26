#include "SearchEngine.h"
#include "StringUtils.h"
#include "QueryParser.h"
#include "QueryTokenizer.h"
#include "QueryFilterParser.h"
#include "QueryNeedsAnalysis.h"
#include "ASTStructuredTransform.h"
#include "ASTGlobTransform.h"
#include "CompiledGlob.h"
#include "SIMDSearch.h"
#include "Logger.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <unordered_map>
#include <chrono>
#include <re2/re2.h>
#include <re2/filtered_re2.h>
#include <functional>
#include <thread>
#include <dispatch/dispatch.h>

// ---------------------------------------------------------------------------
// queryAdvanced: evaluate a parsed AST against the record set.
//
// Strategy:
// 1. Parse the query into an AST.
// 2. Extract best TERM for trigram pre-filtering.
// 3. Evaluate AST per-record: TERM=substring/glob, FILTER=structured filters,
//    AND/OR/NOT=boolean logic.
// 4. Score, sort, and return results.
//
// Supported FILTER types: ext:, size:, file:, folder:, type:, path:, nopath:,
// parent:, depth:, len:, dm:/dc:/da:, fn:, fs:, fu:, noext:, empty:, dupe:,
// child:, childcount:, plus the query-level sort: modifier.
// ---------------------------------------------------------------------------

namespace {

/// Compare a numeric value against a filter's parsed operator and thresholds.
static bool compareNumeric(uint64_t val, CompareOp op, uint64_t v1, uint64_t v2) {
    switch (op) {
        case CompareOp::EQ:    return val == v1;
        case CompareOp::LT:    return val < v1;
        case CompareOp::LE:    return val <= v1;
        case CompareOp::GT:    return val > v1;
        case CompareOp::GE:    return val >= v1;
        case CompareOp::RANGE: return val >= v1 && val <= v2;
        case CompareOp::EQ_EXACT: return val == v1;
        case CompareOp::NE:     return val != v1;
        case CompareOp::NOT_RANGE: return val < v1 || val > v2;
    }
    return false;
}

/// Extract lowercase file extension from name data.
/// Returns a string_view into nameData (zero-allocation) or an empty view
/// when the name has no extension. nameData must already be lowercase.
static std::string_view getExtLower(const char* nameData, uint16_t nameLen) {
    // Find last '.'
    for (int i = static_cast<int>(nameLen) - 1; i >= 0; --i) {
        if (nameData[i] == '.') {
            if (i == static_cast<int>(nameLen) - 1) return {};  // trailing dot
            return std::string_view(nameData + i + 1, nameLen - i - 1);
        }
    }
    return {};
}

/// Count '/' in path to determine depth.
static uint64_t computeDepth(const char* pathData, uint16_t pathLen) {
    // depth: counts parent folders, excluding the root itself. A file in the
    // root folder ("/name") has depth 0; "/Users/name" has depth 1.
    if (pathLen == 0) return 0;
    if (pathLen == 1 && pathData[0] == '/') return 0;
    uint64_t depth = 0;
    for (uint16_t i = 0; i < pathLen; ++i) {
        if (pathData[i] == '/') depth++;
    }
    if (pathData[pathLen - 1] == '/') depth--;
    return depth;
}

/// Normalise text for the `ignore-punctuation:`, `ignore-whitespace:` and
/// `diacritics:` modifiers. Lowercases ASCII, optionally strips ASCII
/// punctuation / whitespace, and drops Unicode combining diacritical marks
/// (the UTF-8 0xCC/0xCD + continuation ranges). Allocates a temporary string
/// only when one of the flags is active.
static std::string normalizeMatchText(std::string_view sv,
                                      bool punct, bool space, bool diac,
                                      bool lowercase = true) {
    std::string out;
    out.reserve(sv.size());
    bool prevLeader = false;
    for (unsigned char c : sv) {
        if (diac && prevLeader && c >= 0x80 && c <= 0xBF) {
            prevLeader = false;   // drop the combining mark byte
            continue;
        }
        prevLeader = (c == 0xCC || c == 0xCD);
        if (punct && c < 0x80 && std::ispunct(static_cast<unsigned char>(c))) continue;
        if (space && std::isspace(static_cast<unsigned char>(c))) continue;
        out.push_back(lowercase
            ? static_cast<char>(std::tolower(static_cast<unsigned char>(c)))
            : static_cast<char>(c));
    }
    return out;
}

// globMatchImpl is now in CompiledGlob.h

/// Check if a character is a word boundary character (not alphanumeric).
/// Note: underscore '_' IS a boundary (consistent with Everything behavior).
static bool isWordBoundaryChar(char c) {
    return !std::isalnum(static_cast<unsigned char>(c));
}

/// Pre-compiled regex cache, keyed by QueryNode pointer.
/// Uses RE2 for guaranteed linear-time matching (no backtracking).
using RegexCache = std::unordered_map<const QueryNode*, std::unique_ptr<re2::RE2>>;

/// Stem length: the name without its final extension (folders keep the name).
static uint16_t stemLength(const char* data, uint16_t len) {
    if (!data) return 0;
    for (int i = static_cast<int>(len) - 1; i > 0; --i) {
        if (data[i] == '.') return static_cast<uint16_t>(i);
    }
    return len;
}

/// Apply a TERM's match mode to one piece of text. `lowerText` is the
/// case-folded form; `origText` preserves the original case.
static bool matchTextMode(const QueryNode& node,
                          std::string_view lowerText,
                          std::string_view origText,
                          const RegexCache& regexCache) {
    switch (node.mode) {
        case MatchMode::REGEX: {
            auto it = regexCache.find(&node);
            if (it == regexCache.end()) return false;
            std::string_view subject = node.caseSensitive ? origText : lowerText;
            re2::StringPiece sp(subject.data(), subject.size());
            return RE2::PartialMatch(sp, *it->second);
        }
        case MatchMode::GLOB: {
            if (node.caseSensitive) {
                return globMatchImpl(node.text, origText.data(), origText.size());
            }
            if (node.compiledGlob) {
                return compiledGlobMatch(*node.compiledGlob,
                                         lowerText.data(), lowerText.size());
            }
            return globMatchImpl(node.textLower, lowerText.data(), lowerText.size());
        }
        default:
            break;
    }

    std::string normSubject, normTerm;
    std::string_view subject, term;
    const bool lower = !node.caseSensitive;
    if (node.ignorePunct || node.ignoreSpace || node.stripDiacritics) {
        normSubject = normalizeMatchText(node.caseSensitive ? origText : lowerText,
                                         node.ignorePunct, node.ignoreSpace,
                                         node.stripDiacritics, lower);
        normTerm = normalizeMatchText(
            node.caseSensitive ? std::string_view(node.text)
                               : std::string_view(node.textLower),
            node.ignorePunct, node.ignoreSpace, node.stripDiacritics, lower);
        subject = normSubject;
        term = normTerm;
    } else if (node.caseSensitive) {
        subject = origText;
        term = node.text;
    } else {
        subject = lowerText;
        term = node.textLower;
    }

    if (term.empty() && node.mode != MatchMode::WHOLEFILENAME &&
        node.mode != MatchMode::WHOLEPATH) {
        return false;
    }

    switch (node.mode) {
        case MatchMode::SUBSTRING:
        case MatchMode::STEM:
        case MatchMode::WHOLEWORD:
        case MatchMode::WORDPREFIX:
        case MatchMode::WORDSUFFIX: {
            if (subject.size() < term.size()) return false;
            const bool wantLeft = (node.mode == MatchMode::WHOLEWORD ||
                                   node.mode == MatchMode::WORDPREFIX);
            const bool wantRight = (node.mode == MatchMode::WHOLEWORD ||
                                    node.mode == MatchMode::WORDSUFFIX);
            for (size_t i = 0; i + term.size() <= subject.size(); ++i) {
                if (subject.compare(i, term.size(), term) != 0) continue;
                if (wantLeft && i > 0 &&
                    !isWordBoundaryChar(subject[i - 1])) continue;
                size_t end = i + term.size();
                if (wantRight && end < subject.size() &&
                    !isWordBoundaryChar(subject[end])) continue;
                return true;
            }
            return false;
        }
        case MatchMode::STARTSWITH:
            return subject.size() >= term.size() &&
                   subject.compare(0, term.size(), term) == 0;
        case MatchMode::ENDSWITH:
            return subject.size() >= term.size() &&
                   subject.compare(subject.size() - term.size(), term.size(), term) == 0;
        case MatchMode::WHOLEFILENAME:
        case MatchMode::WHOLEPATH:
            return subject.size() == term.size() &&
                   subject.compare(term) == 0;
        default:
            return false;
    }
}

/// Collect all REGEX TERM nodes from the AST for pre-compilation.
static void collectRegexNodes(const QueryNode& node, std::vector<const QueryNode*>& out) {
    if (node.type == QueryNodeType::TERM && node.mode == MatchMode::REGEX) {
        out.push_back(&node);
    }
    for (auto& child : node.children) {
        collectRegexNodes(*child, out);
    }
}

/// Clone RegexCache for each thread to avoid RE2 DFA mutex contention.
/// RE2's lazy DFA construction uses internal mutexes; sharing one RE2 object
/// across threads causes severe contention (measured: 12T 6x slower than 1T).
/// Per-thread clones eliminate this, yielding ~3.8x MT speedup.
static std::vector<RegexCache> cloneRegexCachePerThread(
    const RegexCache& src, unsigned numThreads) {
    std::vector<RegexCache> perThread(numThreads);
    if (src.empty()) return perThread;
    for (unsigned t = 0; t < numThreads; t++) {
        for (const auto& [nodePtr, re] : src) {
            re2::RE2::Options opts;
            opts.set_case_sensitive(re->options().case_sensitive());
            opts.set_log_errors(false);
            perThread[t].emplace(nodePtr,
                std::make_unique<re2::RE2>(re->pattern(), opts));
        }
    }
    return perThread;
}

/// Build `path + "/" + name` into `buf`, avoiding a doubled slash when the
/// parent path is the filesystem root ("/").
static size_t buildFullPath(char* buf, const char* path, uint16_t pathLen,
                            const char* name, uint16_t nameLen) {
    size_t n = pathLen;
    memcpy(buf, path, pathLen);
    if (n == 0 || buf[n - 1] != '/') buf[n++] = '/';
    memcpy(buf + n, name, nameLen);
    return n + nameLen;
}

/// ext: — Everything semantics: files only, whole-extension match, `*`/`?`
/// wildcards, leading `.`/`*` ignored, empty list = files without extension.
static bool evalExtFilter(const QueryNode& node,
                          const char* nameData, uint16_t nameLen,
                          const char* origNameData, uint16_t origNameLen,
                          uint8_t recType) {
    if (recType == 2) return false;   // folders have no extension
    std::string_view ext = getExtLower(nameData, nameLen);
    if (node.extList.empty()) return ext.empty();
    if (ext.empty()) return false;

    std::string_view origExt = origNameData
        ? getExtLower(origNameData, origNameLen) : ext;

    for (const auto& raw : node.extList) {
        if (raw == "*") return true;
        std::string pat = node.caseSensitive ? raw : me::toLower(raw);
        std::string_view subject = node.caseSensitive ? origExt : ext;
        std::string subjStore, patStore;
        if (node.stripDiacritics) {
            subjStore = normalizeMatchText(subject, false, false, true,
                                           !node.caseSensitive);
            patStore = normalizeMatchText(pat, false, false, true,
                                          !node.caseSensitive);
            subject = subjStore;
            pat = patStore;
        }
        bool wild = pat.find('*') != std::string::npos ||
                    pat.find('?') != std::string::npos;
        if (wild) {
            if (globMatchImpl(pat, subject.data(), subject.size())) return true;
        } else if (subject == pat) {
            return true;
        }
    }
    return false;
}

/// Evaluate a FILTER node against a single record (SoA fields).
/// When called from the pure-filter SoA fast path, nameData and pathData are nullptr —
/// string-based filters short-circuit (return true) since isPureFilter() guarantees
/// the query contains no string filters.
static bool evalFilter(const QueryNode& node,
                       uint8_t recType, uint64_t recSize, time_t recModTime,
                       const char* nameData, uint16_t nameLen,
                       const char* pathData, uint16_t pathLen,
                       const char* origNameData, uint16_t origNameLen,
                       std::vector<char>& pathBuf) {
    const auto& name = node.filterName;

    // Internal match-all placeholder (bare modifiers, unknown functions).
    if (name == "__tautology") return true;

    // __pathseg: internal filter — structured path segment matching
    if (name == "__pathseg") {
        if (!nameData || !pathData) return true;
        size_t capacity = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < capacity) pathBuf.resize(capacity * 2);
        size_t fullLen = buildFullPath(pathBuf.data(), pathData, pathLen,
                                       nameData, nameLen);
        std::string_view fullPath(pathBuf.data(), fullLen);
        return SearchEngine::pathSegmentsMatch(fullPath, node.pathSegments);
    }

    // ext: — whole-extension match with wildcards (requires name data)
    if (name == "ext") {
        if (!nameData) return true;
        return evalExtFilter(node, nameData, nameLen,
                             origNameData, origNameLen, recType);
    }

    // size: / fs: — compare file size
    if (name == "size" || name == "fs") {
        return compareNumeric(recSize, node.op, node.numVal1, node.numVal2);
    }

    // file: — match files only (folders are the only non-file type)
    if (name == "file") {
        return recType != 2;
    }

    // folder: — match directories only
    if (name == "folder") {
        return recType == 2;
    }

    // type: — shorthand for file/folder, plus Everything's media types
    if (name == "type") {
        const std::string& a = node.filterArg;
        if (a == "file") return recType != 2;
        if (a == "folder" || a == "dir" || a == "directory") return recType == 2;
        if (a == "audio" || a == "video" || a == "pic" || a == "image" ||
            a == "doc" || a == "document" || a == "exe" || a == "zip" ||
            a == "archive") {
            if (!nameData) return true;
            if (recType == 2) return false;
            std::string_view ext = getExtLower(nameData, nameLen);
            if (ext.empty()) return false;
            const char* macro = nullptr;
            if (a == "audio") macro = "mp3;wav;flac;aac;ogg;m4a;wma;alac;aiff;aif;opus;mid;midi";
            else if (a == "video") macro = "mp4;avi;mkv;mov;wmv;flv;webm;m4v;mpg;mpeg;3gp;ts;vob";
            else if (a == "pic" || a == "image") macro = "jpg;jpeg;png;gif;bmp;tiff;tif;webp;svg;ico;heic;heif;raw;psd;ai";
            else if (a == "doc" || a == "document") macro = "pdf;doc;docx;xls;xlsx;ppt;pptx;txt;rtf;odt;ods;odp;csv;md;pages;numbers;key;epub;mobi";
            else if (a == "exe") macro = "app;dmg;pkg;sh;command;csh;action";
            else macro = "zip;rar;7z;tar;gz;bz2;xz;tgz;zst;lz4;cab;iso;dmg";
            std::string_view macroSv(macro);
            size_t start = 0;
            while (start <= macroSv.size()) {
                size_t semi = macroSv.find(';', start);
                if (semi == std::string_view::npos) semi = macroSv.size();
                if (macroSv.substr(start, semi - start) == ext) return true;
                if (semi == macroSv.size()) break;
                start = semi + 1;
            }
            return false;
        }
        return false;
    }

    // path: — full path must contain the text (legacy FILTER form; the
    // modern `path:` is a text modifier handled as a TERM).
    if (name == "path") {
        if (!nameData || !pathData) return true;
        size_t capacity = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < capacity) pathBuf.resize(capacity * 2);
        size_t fullLen = buildFullPath(pathBuf.data(), pathData, pathLen,
                                       nameData, nameLen);
        return me::simdContains(pathBuf.data(), fullLen,
                                node.filterArg.data(), node.filterArg.size());
    }

    // nopath: — full path must NOT contain the text
    if (name == "nopath") {
        if (!nameData || !pathData) return true;
        size_t capacity = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < capacity) pathBuf.resize(capacity * 2);
        size_t fullLen = buildFullPath(pathBuf.data(), pathData, pathLen,
                                       nameData, nameLen);
        return !me::simdContains(pathBuf.data(), fullLen,
                                 node.filterArg.data(), node.filterArg.size());
    }

    // parent: — exact absolute parent folder path (Everything semantics).
    if (name == "parent") {
        if (!pathData) return true;
        std::string_view parentPath(pathData, pathLen);
        std::string_view wanted(node.filterArg);
        // A root path is stored as "/" (or "" in synthetic test data).
        if (wanted == "/") return parentPath.empty() || parentPath == "/";
        while (parentPath.size() > 1 && parentPath.back() == '/')
            parentPath.remove_suffix(1);
        return parentPath == wanted;
    }

    // depth: / parent-count: — number of parent folders
    if (name == "depth") {
        if (!pathData) return true;
        uint64_t depth = computeDepth(pathData, pathLen);
        return compareNumeric(depth, node.op, node.numVal1, node.numVal2);
    }

    // len: — filename length (requires name data)
    if (name == "len") {
        if (!nameData) return true;
        return compareNumeric(static_cast<uint64_t>(nameLen), node.op,
                              node.numVal1, node.numVal2);
    }

    // name-length: — filename length
    if (name == "name-length" || name == "namelen") {
        if (!nameData) return true;
        return compareNumeric(static_cast<uint64_t>(nameLen), node.op,
                              node.numVal1, node.numVal2);
    }

    // full-path-length: — path + "/" + name length
    if (name == "full-path-length" || name == "fullpathlen") {
        if (!nameData || !pathData) return true;
        uint64_t fullLen = static_cast<uint64_t>(pathLen) + 1 + nameLen;
        if (pathLen > 0 && pathData[pathLen - 1] == '/')
            fullLen = static_cast<uint64_t>(pathLen) + nameLen;
        return compareNumeric(fullLen, node.op, node.numVal1, node.numVal2);
    }

    // dm: / datemodified: — modification date filter
    if (name == "dm" || name == "datemodified") {
        return compareNumeric(static_cast<uint64_t>(recModTime), node.op,
                              node.numVal1, node.numVal2);
    }

    // dc: / datecreated: — creation date (uses modTime as fallback since
    // FileRecord doesn't store birthtime yet)
    if (name == "dc" || name == "datecreated") {
        return compareNumeric(static_cast<uint64_t>(recModTime), node.op,
                              node.numVal1, node.numVal2);
    }

    // da: / dateaccessed: — access date (uses modTime as fallback)
    if (name == "da" || name == "dateaccessed") {
        return compareNumeric(static_cast<uint64_t>(recModTime), node.op,
                              node.numVal1, node.numVal2);
    }

    // extension: — substring *within* the extension (vs ext: whole-extension)
    if (name == "extension" || name == "extcontains") {
        if (!nameData) return true;
        if (recType == 2) return false;
        std::string_view ext = getExtLower(nameData, nameLen);
        if (ext.empty()) return false;
        return me::simdContains(ext.data(), ext.size(),
                                node.filterArg.data(), node.filterArg.size());
    }

    // parent-name: — the parent directory's *name* contains the text
    if (name == "parent-name" || name == "parentname") {
        if (!pathData) return true;
        std::string_view parentPath(pathData, pathLen);
        while (parentPath.size() > 1 && parentPath.back() == '/')
            parentPath.remove_suffix(1);
        size_t slash = parentPath.find_last_of('/');
        std::string_view parentName = (slash == std::string_view::npos)
            ? parentPath : parentPath.substr(slash + 1);
        if (parentName.empty()) return false;
        return me::simdContains(parentName.data(), parentName.size(),
                                node.filterArg.data(), node.filterArg.size());
    }

    // ancestor: — exact absolute ancestor path (the record must live below it)
    if (name == "ancestor") {
        if (!pathData) return true;
        std::string_view parentPath(pathData, pathLen);
        std::string wanted = node.filterArg;
        while (!wanted.empty() && wanted.back() == '/') wanted.pop_back();
        if (wanted.empty()) return true;
        if (parentPath.size() < wanted.size()) return false;
        if (parentPath.compare(0, wanted.size(), wanted) != 0) return false;
        return parentPath.size() == wanted.size() ||
               parentPath[wanted.size()] == '/';
    }

    // ancestor-name: — any ancestor folder name contains the text
    if (name == "ancestor-name") {
        if (!pathData) return true;
        std::string_view parentPath(pathData, pathLen);
        size_t start = 0;
        while (start <= parentPath.size()) {
            size_t slash = parentPath.find('/', start);
            size_t end = (slash == std::string_view::npos)
                ? parentPath.size() : slash;
            if (end > start) {
                std::string_view comp = parentPath.substr(start, end - start);
                if (me::simdContains(comp.data(), comp.size(),
                                     node.filterArg.data(),
                                     node.filterArg.size()))
                    return true;
            }
            if (slash == std::string_view::npos) break;
            start = slash + 1;
        }
        return false;
    }

    // root: — items with no parent folder
    if (name == "root") {
        return pathLen == 0 || (pathLen == 1 && pathData[0] == '/');
    }

    // Segmented date filters: compare a single field of the date.
    if (node.dateSegment && *node.dateSegment >= 0) {
        struct tm t;
        localtime_r(&recModTime, &t);
        uint64_t fieldVal = 0;
        switch (static_cast<QueryFilterParser::DateSegmentKind>(*node.dateSegment)) {
            case QueryFilterParser::DateSegmentKind::HOUR:   fieldVal = static_cast<uint64_t>(t.tm_hour); break;
            case QueryFilterParser::DateSegmentKind::MINUTE: fieldVal = static_cast<uint64_t>(t.tm_min); break;
            case QueryFilterParser::DateSegmentKind::DAY:    fieldVal = static_cast<uint64_t>(t.tm_mday); break;
            case QueryFilterParser::DateSegmentKind::DOW:    fieldVal = static_cast<uint64_t>(t.tm_wday); break;
            case QueryFilterParser::DateSegmentKind::MONTH:  fieldVal = static_cast<uint64_t>(t.tm_mon + 1); break;
            case QueryFilterParser::DateSegmentKind::YEAR:   fieldVal = static_cast<uint64_t>(t.tm_year + 1900); break;
            case QueryFilterParser::DateSegmentKind::DATE:
                fieldVal = static_cast<uint64_t>(t.tm_year + 1900) * 10000 +
                           static_cast<uint64_t>(t.tm_mon + 1) * 100 +
                           static_cast<uint64_t>(t.tm_mday);
                break;
            case QueryFilterParser::DateSegmentKind::TIME:
                fieldVal = static_cast<uint64_t>(t.tm_hour) * 3600 +
                           static_cast<uint64_t>(t.tm_min) * 60 +
                           static_cast<uint64_t>(t.tm_sec);
                break;
        }
        return compareNumeric(fieldVal, node.op, node.numVal1, node.numVal2);
    }

    // Unknown filter — pass through
    return true;
}

/// Evaluate a TERM node against a single record. MatchTarget selects which
/// text is examined (the filename by default, as in Everything); MatchMode
/// then decides how the text is compared.
/// nameData/pathData are lowercase; origNameData/origPathData keep case.
/// Returns false when nameData is nullptr (pure-filter SoA fast path).
static bool evalTerm(const QueryNode& node,
                     uint8_t recType,
                     const char* nameData, uint16_t nameLen,
                     const char* pathData, uint16_t pathLen,
                     const char* origNameData, uint16_t origNameLen,
                     const char* origPathData, uint16_t origPathLen,
                     std::vector<char>& pathBuf,
                     const RegexCache& regexCache) {
    if (!nameData) return false; // SoA pure-filter path — no string data available
    if (node.typeConstraint == 1 && recType == 2) return false;
    if (node.typeConstraint == 2 && recType != 2) return false;

    MatchTarget target = node.target;
    if (node.matchPath && target == MatchTarget::NAME)
        target = MatchTarget::FULL_PATH;

    switch (target) {

    case MatchTarget::NAME:
        return matchTextMode(node,
                             std::string_view(nameData, nameLen),
                             std::string_view(origNameData, origNameLen),
                             regexCache);

    case MatchTarget::STEM: {
        // Folders have no extension, so their stem is the whole name.
        uint16_t lLen = (recType == 2) ? nameLen : stemLength(nameData, nameLen);
        uint16_t oLen = (recType == 2) ? origNameLen : stemLength(origNameData, origNameLen);
        return matchTextMode(node,
                             std::string_view(nameData, lLen),
                             std::string_view(origNameData, oLen),
                             regexCache);
    }

    case MatchTarget::EXTENSION: {
        if (recType == 2) return false;
        std::string_view ext = getExtLower(nameData, nameLen);
        std::string_view origExt = getExtLower(origNameData, origNameLen);
        // `extension:` with no text = files that have an extension.
        if (node.text.empty() && node.mode == MatchMode::SUBSTRING)
            return !ext.empty();
        if (ext.empty()) return false;
        return matchTextMode(node, ext, origExt, regexCache);
    }

    case MatchTarget::FULL_PATH: {
        // Pick the case variant once so the single pathBuf is enough.
        const char* pd = node.caseSensitive ? origPathData : pathData;
        uint16_t pl = node.caseSensitive ? origPathLen : pathLen;
        const char* nd = node.caseSensitive ? origNameData : nameData;
        uint16_t nl = node.caseSensitive ? origNameLen : nameLen;
        size_t capacity = static_cast<size_t>(pl) + 1 + nl;
        if (pathBuf.size() < capacity) pathBuf.resize(capacity * 2);
        size_t len = buildFullPath(pathBuf.data(), pd, pl, nd, nl);
        std::string_view subject(pathBuf.data(), len);
        if (node.mode == MatchMode::GLOB) {
            const std::string& pat = node.caseSensitive ? node.text : node.textLower;
            return globMatchPathImpl(pat, subject.data(), subject.size());
        }
        return matchTextMode(node, subject, subject, regexCache);
    }

    case MatchTarget::PARENT_NAME: {
        std::string_view parentPath(pathData, pathLen);
        std::string_view parentOrig(origPathData, origPathLen);
        while (parentPath.size() > 1 && parentPath.back() == '/')
            parentPath.remove_suffix(1);
        while (parentOrig.size() > 1 && parentOrig.back() == '/')
            parentOrig.remove_suffix(1);
        size_t slash = parentPath.find_last_of('/');
        std::string_view parentName = (slash == std::string_view::npos)
            ? parentPath : parentPath.substr(slash + 1);
        size_t oslash = parentOrig.find_last_of('/');
        std::string_view parentNameOrig = (oslash == std::string_view::npos)
            ? parentOrig : parentOrig.substr(oslash + 1);
        if (parentName.empty()) return false;
        return matchTextMode(node, parentName, parentNameOrig, regexCache);
    }

    case MatchTarget::LOCATION:
        // Everything location: matches files *inside* the folder, not the
        // folder itself — pathData is already the parent folder path.
        return matchTextMode(node,
                             std::string_view(pathData, pathLen),
                             std::string_view(origPathData, origPathLen),
                             regexCache);

    case MatchTarget::ANCESTOR_PATH: {
        // Exact ancestor path; records below it (subfolders included) match.
        std::string_view parentPath = node.caseSensitive
            ? std::string_view(origPathData, origPathLen)
            : std::string_view(pathData, pathLen);
        std::string_view wanted = node.caseSensitive
            ? std::string_view(node.text) : std::string_view(node.textLower);
        while (!wanted.empty() && wanted.back() == '/')
            wanted.remove_suffix(1);
        if (wanted.empty()) return false;
        if (parentPath.size() < wanted.size()) return false;
        if (parentPath.compare(0, wanted.size(), wanted) != 0) return false;
        return parentPath.size() == wanted.size() ||
               parentPath[wanted.size()] == '/';
    }

    case MatchTarget::ANCESTOR_NAME: {
        std::string_view parentPath = node.caseSensitive
            ? std::string_view(origPathData, origPathLen)
            : std::string_view(pathData, pathLen);
        size_t start = 0;
        while (start <= parentPath.size()) {
            size_t slash = parentPath.find('/', start);
            size_t end = (slash == std::string_view::npos)
                ? parentPath.size() : slash;
            if (end > start) {
                std::string_view comp = parentPath.substr(start, end - start);
                if (matchTextMode(node, comp, comp, regexCache)) return true;
            }
            if (slash == std::string_view::npos) break;
            start = slash + 1;
        }
        return false;
    }

    }
    return false;
}

/// Recursively evaluate AST node against a single record.
static bool evalNode(const QueryNode& node,
                     uint8_t recType, uint64_t recSize, time_t recModTime,
                     const char* nameData, uint16_t nameLen,
                     const char* pathData, uint16_t pathLen,
                     const char* origNameData, uint16_t origNameLen,
                     const char* origPathData, uint16_t origPathLen,
                     std::vector<char>& pathBuf,
                     const RegexCache& regexCache) {
    switch (node.type) {
        case QueryNodeType::TERM:
            return evalTerm(node, recType, nameData, nameLen, pathData, pathLen,
                            origNameData, origNameLen, origPathData, origPathLen,
                            pathBuf, regexCache);

        case QueryNodeType::AND:
            for (auto& child : node.children) {
                if (!evalNode(*child, recType, recSize, recModTime,
                              nameData, nameLen, pathData, pathLen,
                              origNameData, origNameLen, origPathData, origPathLen,
                              pathBuf, regexCache))
                    return false;
            }
            return true;

        case QueryNodeType::OR:
            for (auto& child : node.children) {
                if (evalNode(*child, recType, recSize, recModTime,
                             nameData, nameLen, pathData, pathLen,
                             origNameData, origNameLen, origPathData, origPathLen,
                             pathBuf, regexCache))
                    return true;
            }
            return false;

        case QueryNodeType::NOT:
            if (node.children.empty()) return true;
            return !evalNode(*node.children[0], recType, recSize, recModTime,
                             nameData, nameLen, pathData, pathLen,
                             origNameData, origNameLen, origPathData, origPathLen,
                             pathBuf, regexCache);

        case QueryNodeType::FILTER:
            return evalFilter(node, recType, recSize, recModTime,
                              nameData, nameLen, pathData, pathLen,
                              origNameData, origNameLen, pathBuf);
    }
    return false;
}

/// Find the single best (longest) SUBSTRING TERM from the AND-level for trigram pre-filtering.
/// For OR queries we can't pre-filter (any branch might match), so return empty.
/// Skips __pathseg filter nodes and recurses into AND children.
/// Extract the best trigram key from a TERM node (SUBSTRING or GLOB mode).
/// Only terms matched against the filename can use the name trigram index;
/// path-target terms and normalised (-punct/-whitespace/diacritics) terms
/// would produce false negatives, so they return no key.
static std::string termTrigramKey(const QueryNode& node) {
    if (node.ignorePunct || node.ignoreSpace || node.stripDiacritics) return {};
    // matchPath retargets NAME terms to the full path — the name index no
    // longer covers every possible match.
    if (node.matchPath) return {};
    if (node.target != MatchTarget::NAME &&
        node.target != MatchTarget::STEM &&
        node.target != MatchTarget::EXTENSION) return {};

    if (node.mode == MatchMode::GLOB) {
        auto segs = extractLiteralSegments(node.textLower);
        std::string best;
        for (const auto& s : segs)
            if (s.size() >= 3 && s.size() > best.size()) best = s;
        return best;
    }
    if (node.mode == MatchMode::SUBSTRING ||
        node.mode == MatchMode::STEM ||
        node.mode == MatchMode::STARTSWITH ||
        node.mode == MatchMode::ENDSWITH ||
        node.mode == MatchMode::WHOLEWORD ||
        node.mode == MatchMode::WORDPREFIX ||
        node.mode == MatchMode::WORDSUFFIX ||
        node.mode == MatchMode::WHOLEFILENAME) {
        return node.textLower;
    }
    return {};
}

static std::string bestTrigramTerm(const QueryNode& node) {
    if (node.type == QueryNodeType::TERM) {
        return termTrigramKey(node);
    }
    if (node.type == QueryNodeType::AND) {
        std::string best;
        for (auto& child : node.children) {
            if (child->type == QueryNodeType::TERM) {
                std::string t = termTrigramKey(*child);
                if (t.size() > best.size()) best = t;
            } else if (child->type == QueryNodeType::AND) {
                std::string t = bestTrigramTerm(*child);
                if (t.size() > best.size()) best = t;
            }
        }
        return best;
    }
    // OR, NOT, FILTER — can't reliably pre-filter
    return {};
}

/// Extract literal atoms from a regex pattern using RE2's FilteredRE2.
/// Returns lowercased, deduplicated atoms (min length 3) that RE2's own
/// AST parser determines are useful for pre-filtering.
/// Correctly handles alternation, character classes, and all regex constructs.
static std::vector<std::string> extractRegexLiteralsImpl(const std::string& pattern) {
    re2::FilteredRE2 fre2(3);  // min_atom_len = 3 (matches trigram minimum)
    re2::RE2::Options opts;
    opts.set_case_sensitive(false);
    opts.set_log_errors(false);
    int id;
    if (fre2.Add(pattern, opts, &id) != re2::RE2::NoError) {
        return {};
    }
    std::vector<std::string> atoms;
    fre2.Compile(&atoms);
    return atoms;
}

/// Collect regex literal substrings from all REGEX TERM nodes in the AST.
static std::vector<std::string> extractRegexLiteralsFromAST(const QueryNode& node) {
    std::vector<std::string> allLiterals;
    std::function<void(const QueryNode&)> collect = [&](const QueryNode& n) {
        if (n.type == QueryNodeType::TERM && n.mode == MatchMode::REGEX) {
            auto lits = extractRegexLiteralsImpl(n.text);
            allLiterals.insert(allLiterals.end(), lits.begin(), lits.end());
        }
        for (auto& child : n.children) collect(*child);
    };
    collect(node);
    return allLiterals;
}


/// Extract the first SUBSTRING TERM text from the AST for result scoring.
/// Returns empty string if no suitable term is found (pure filter queries).
/// Skips __pathseg filter nodes (they are path constraints, not scoring terms).
static std::string extractScoringTerm(const QueryNode& node) {
    if (node.type == QueryNodeType::TERM && node.mode == MatchMode::SUBSTRING &&
        (node.target == MatchTarget::NAME || node.target == MatchTarget::STEM ||
         node.target == MatchTarget::EXTENSION) &&
        !node.ignorePunct && !node.ignoreSpace && !node.stripDiacritics) {
        return node.textLower;
    }
    if (node.type == QueryNodeType::FILTER && node.filterName == "__pathseg") {
        return {}; // path segment constraint, not a scoring term
    }
    if (node.type == QueryNodeType::AND || node.type == QueryNodeType::OR) {
        for (auto& child : node.children) {
            auto t = extractScoringTerm(*child);
            if (!t.empty()) return t;
        }
    }
    return {};
}

/// Extract a trigram key for the path index from a TERM node whose subject
/// includes the parent folder path. The path trigram index only covers parent
/// paths, so the key must be a literal that is guaranteed to occur there:
///  - FULL_PATH: every component before the last separator (the trailing one
///    may match the filename instead).
///  - LOCATION / PARENT_NAME / ANCESTOR_PATH / ANCESTOR_NAME: the whole text
///    (these targets never look at the filename).
/// Returns empty when the match could succeed without the literal appearing in
/// the parent path (name-only terms, normalised text, regex).
static std::string termPathTrigramKey(const QueryNode& node) {
    if (node.ignorePunct || node.ignoreSpace || node.stripDiacritics) return {};
    if (node.mode == MatchMode::REGEX) return {};

    // The index keys are lowercase, so normalise regardless of match case.
    std::string text = node.textLower.empty() ? me::toLower(node.text) : node.textLower;
    if (text.size() < 3) return {};

    auto longestPlainComponent = [](const std::string& s, bool skipWildcards) {
        std::string best;
        size_t start = 0;
        while (start <= s.size()) {
            size_t slash = s.find('/', start);
            size_t end = (slash == std::string::npos) ? s.size() : slash;
            size_t len = end - start;
            bool usable = len >= 3;
            if (skipWildcards &&
                (s.find('*', start) < end || s.find('?', start) < end))
                usable = false;
            if (usable && len > best.size()) best = s.substr(start, len);
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return best;
    };

    switch (node.target) {
    case MatchTarget::FULL_PATH: {
        // Only the directory part can be sited in the parent path; the part
        // after the last separator may live entirely in the filename.
        size_t slash = text.find_last_of('/');
        if (slash == std::string::npos || slash == 0) return {};
        std::string dirPart = text.substr(0, slash);
        if (node.mode == MatchMode::GLOB) {
            // A wildcard component is not a literal — keep only plain ones.
            return longestPlainComponent(dirPart, true);
        }
        return longestPlainComponent(dirPart, false);
    }
    case MatchTarget::LOCATION:
    case MatchTarget::PARENT_NAME:
    case MatchTarget::ANCESTOR_PATH:
    case MatchTarget::ANCESTOR_NAME:
        if (node.mode == MatchMode::GLOB) {
            auto segs = extractLiteralSegments(text);
            std::string best;
            for (const auto& s : segs)
                if (s.size() >= 3 && s.size() > best.size()) best = s;
            return best;
        }
        return text;
    default:
        return {};
    }
}

/// Extract the best (longest, >= 3 chars) path trigram key from AND-level
/// nodes: __pathseg filters and path-target TERMs. OR branches are skipped
/// because only one branch has to match.
static std::string bestPathSegTerm(const QueryNode& node) {
    if (node.type == QueryNodeType::FILTER && node.filterName == "__pathseg") {
        std::string best;
        for (auto& seg : node.pathSegments) {
            if (seg.text.size() >= 3 && seg.text.size() > best.size()) {
                best = seg.text;
            }
        }
        return best;
    }
    if (node.type == QueryNodeType::TERM) {
        return termPathTrigramKey(node);
    }
    if (node.type == QueryNodeType::AND) {
        std::string best;
        for (auto& child : node.children) {
            auto t = bestPathSegTerm(*child);
            if (t.size() > best.size()) best = t;
        }
        return best;
    }
    return {};
}

/// Extract extension filter values from AND-level ext: FILTER nodes in the AST.
/// Returns the collected extList values for the first ext: filter found at AND
/// level, but only when the extension index can answer the filter exactly:
/// no wildcards, no case sensitivity, no diacritics normalisation and a
/// non-empty list. Otherwise the evaluator runs the filter itself.
static std::vector<std::string> extractExtFilterValues(const QueryNode& node) {
    const QueryNode* ext = nullptr;
    if (node.type == QueryNodeType::FILTER && node.filterName == "ext") {
        ext = &node;
    } else if (node.type == QueryNodeType::AND) {
        for (auto& child : node.children) {
            if (child->type == QueryNodeType::FILTER && child->filterName == "ext") {
                ext = child.get();
                break;
            }
        }
    }
    if (!ext) return {};
    if (ext->caseSensitive || ext->stripDiacritics) return {};
    if (ext->extList.empty()) return {};   // empty list = files without extension
    std::vector<std::string> values;
    values.reserve(ext->extList.size());
    for (const auto& e : ext->extList) {
        if (e == "*" || e.find('*') != std::string::npos ||
            e.find('?') != std::string::npos) return {};
        values.push_back(me::toLower(e));
    }
    return values;
}

} // anonymous namespace

// Forward declaration — the definition lives further down (it needs access to
// SearchEngine's private members, so it is defined as a member below).
namespace {
bool extractAggregateFilters(QueryNode& node,
                             std::vector<std::unique_ptr<QueryNode>>& storage,
                             std::vector<const QueryNode*>& out);
}

// Expose extractRegexLiterals for unit testing
namespace me_test {
    std::vector<std::string> extractRegexLiterals(const std::string& pattern) {
        return extractRegexLiteralsImpl(pattern);
    }
}

std::vector<uint32_t> SearchEngine::queryAdvanced(const std::string& input,
                                                   uint32_t maxResults,
                                                   bool useTrigram,
                                                   QueryTimingInfo& timing,
                                                   uint64_t myGen,
                                                   const std::atomic<uint64_t>* genPtr) const {
    // If no external generation pointer, create a local dummy (no cancellation)
    std::atomic<uint64_t> dummyGen{myGen};
    if (!genPtr) genPtr = &dummyGen;

    auto queryStart = std::chrono::steady_clock::now();

    // Parse the AST
    auto ast = QueryParser::parse(input);
    if (!ast) return {};


    // Transform TERM nodes containing '/' into structured path-segment constraints.
    // e.g. TERM("/usr/local/test") → AND(FILTER("__pathseg",[usr,local]), TERM("test"))
    ast = transformSlashTerms(std::move(ast));

    // Transform TERM nodes containing '*' or '?' from SUBSTRING to GLOB mode.
    ast = transformGlobTerms(std::move(ast));

    // Pull aggregate filters (empty:/childcount:/child:/dupe:) off the AST.
    // They need cross-record data, so they run as a post-pass after evaluation.
    // Ownership moves into aggregateStorage to keep the node pointers valid.
    std::vector<std::unique_ptr<QueryNode>> aggregateStorage;
    std::vector<const QueryNode*> aggregateNodes;
    if (ast->type == QueryNodeType::FILTER &&
        QueryFilterParser::isAggregateFilter(ast->filterName)) {
        // The query was nothing but an aggregate filter — match everything.
        aggregateStorage.push_back(std::move(ast));
        aggregateNodes.push_back(aggregateStorage.back().get());
        std::optional<SortSpec> carriedSort = aggregateNodes.back()->sort;
        ast = QueryNode::makeAnd({});
        ast->sort = carriedSort;
    } else {
        extractAggregateFilters(*ast, aggregateStorage, aggregateNodes);
    }

    // Analyze which record fields the query actually needs
    QueryNeeds needs = analyzeQueryNeeds(*ast);

    // Pre-compile all regex patterns before acquiring lock
    RegexCache regexCache;
    {
        std::vector<const QueryNode*> regexNodes;
        collectRegexNodes(*ast, regexNodes);
        for (auto* rn : regexNodes) {
            re2::RE2::Options opts;
            opts.set_case_sensitive(rn->caseSensitive);
            opts.set_log_errors(false);
            // Apply regex option flags from dot-all:/multi-line:/ungreedy:.
            std::string pattern = rn->text;
            if (!rn->regexFlags.empty()) {
                pattern = "(?" + rn->regexFlags + ")" + pattern;
            }
            auto re = std::make_unique<re2::RE2>(pattern, opts);
            if (re->ok()) {
                regexCache.emplace(rn, std::move(re));
            }
            // Invalid regex — evalTerm will return false for this node
        }
    }

    auto beforeLock = std::chrono::steady_clock::now();
    std::shared_lock lock(mutex_);
    auto afterLock = std::chrono::steady_clock::now();

    if (types_.empty()) return {};
    size_t totalSize = types_.size();
    auto beforeTrigram = afterLock, afterTrigram = afterLock;

    // --- Trigram pre-filtering: competitive selection among name, regex, and path trigrams ---
    std::string trigramKey = bestTrigramTerm(*ast);
    std::vector<uint32_t> candidates;
    bool useTrigramIndex = false;

    // Stage 1: Name trigram — stash result, don't commit yet
    std::vector<uint32_t> nameCands;
    bool nameOk = false;
    bool stage1AllFound = false;
    size_t stage1RawCandCount = 0;
    if (useTrigram && !trigramKey.empty() && trigramKey.size() >= 3 && !nameTrigramIndex_.empty()) {
        beforeTrigram = std::chrono::steady_clock::now();
        nameCands = intersectPostingLists(nameTrigramIndex_, trigramKey, stage1AllFound);
        stage1RawCandCount = nameCands.size();
        nameOk = stage1AllFound && nameCands.size() <= totalSize / 10;
        if (!nameOk) nameCands.clear();
        afterTrigram = std::chrono::steady_clock::now();
    }

    // Diagnostic: log why trigram Stage 1 was skipped or failed (for 3+ char queries)
    if (trigramKey.size() >= 3 && !nameOk) {
        bool p2 = phase2Pending_.load(std::memory_order_acquire);
        bool idxEmpty = nameTrigramIndex_.empty();
        LOG_INFO("SearchEngine", "DIAG-TRIGRAM key=\"" << trigramKey
            << "\" SKIP reason="
            << (!useTrigram ? "useTrigram=false" :
                idxEmpty ? "indexEmpty" :
                !stage1AllFound ? "allFound=false" :
                stage1RawCandCount > totalSize / 10 ? "tooManyCands" : "unknown")
            << " phase2Pending=" << p2
            << " indexEmpty=" << idxEmpty
            << " indexKeys=" << nameTrigramIndex_.keyCount()
            << " allFound=" << stage1AllFound
            << " rawCands=" << stage1RawCandCount
            << " threshold=" << totalSize / 10
            << " totalSize=" << totalSize);
    }

    // Stage 2: Regex trigram (only if name failed)
    // Uses UNION (not AND) across atoms because FilteredRE2 atoms may have
    // OR semantics (e.g. alternation `foo|bar` → atoms are OR-related).
    if (useTrigram && !nameOk && !nameTrigramIndex_.empty()) {
        auto regexLiterals = extractRegexLiteralsFromAST(*ast);
        if (!regexLiterals.empty()) {
            beforeTrigram = std::chrono::steady_clock::now();
            nameCands = unionPostingListsMulti(nameTrigramIndex_, regexLiterals);
            nameOk = !nameCands.empty() && nameCands.size() <= totalSize / 10;
            if (!nameOk) nameCands.clear();
            afterTrigram = std::chrono::steady_clock::now();
        }
    }

    // Stage 3: Path trigram — always attempt (compete with name trigram)
    std::vector<uint32_t> pathCands;
    bool pathOk = false;
    if (useTrigram && !pathTrigramIndex_.empty()) {
        std::string pathSegKey = bestPathSegTerm(*ast);
        if (pathSegKey.size() >= 3) {
            bool allFound = false;
            auto pathIdxCands = intersectPostingLists(pathTrigramIndex_, pathSegKey, allFound);
            if (allFound) {
                // Early-exit: if name trigram already succeeded and path index count
                // is >= nameCands (each path index expands to >=1 record), expansion
                // cannot produce fewer candidates — skip the expensive expansion.
                bool skipExpansion = nameOk && pathIdxCands.size() >= nameCands.size();
                if (!skipExpansion) {
                    for (uint32_t pi : pathIdxCands) {
                        if (genPtr->load(std::memory_order_relaxed) != myGen) return {};
                        if (pi >= pathIdxToRecords_.size()) continue;
                        const auto& recIds = pathIdxToRecords_[pi];
                        pathCands.insert(pathCands.end(), recIds.begin(), recIds.end());
                    }
                    std::sort(pathCands.begin(), pathCands.end());
                    pathCands.erase(std::unique(pathCands.begin(), pathCands.end()), pathCands.end());
                    pathOk = pathCands.size() <= totalSize / 4;
                    if (!pathOk) pathCands.clear();
                }
            }
            afterTrigram = std::chrono::steady_clock::now();
        }
    }

    // Stage 4: Extension index pre-filtering
    std::vector<uint32_t> extCands;
    bool extOk = false;
    {
        auto extValues = extractExtFilterValues(*ast);
        if (!extValues.empty()) {
            // Union posting lists for all requested extensions
            std::vector<uint32_t> extRun;
            for (const auto& ext : extValues) {
                if (extensionIndex_.decode(ext, extRun)) {
                    extCands.insert(extCands.end(), extRun.begin(), extRun.end());
                }
            }
            if (extValues.size() > 1) {
                std::sort(extCands.begin(), extCands.end());
                extCands.erase(std::unique(extCands.begin(), extCands.end()), extCands.end());
            }
            extOk = true; // Extension index is precise — empty means no matches
        }
    }

    // Stage 5: Pick best — fewer candidates wins
    // Collect all viable candidate sets and pick the smallest
    struct CandidateSet {
        std::vector<uint32_t>* cands;
        const char* label;
    };
    std::vector<CandidateSet> viable;
    if (nameOk) viable.push_back({&nameCands, "name"});
    if (pathOk) viable.push_back({&pathCands, "path"});
    if (extOk)  viable.push_back({&extCands,  "ext"});

    if (!viable.empty()) {
        // Find smallest
        auto best = &viable[0];
        for (size_t i = 1; i < viable.size(); i++) {
            if (viable[i].cands->size() < best->cands->size()) {
                best = &viable[i];
            }
        }
        candidates = std::move(*best->cands);
        // If the winner is not name trigram, clear trigramKey for label purposes
        if (std::string(best->label) != "name") {
            trigramKey.clear();
        }
        useTrigramIndex = true;
    }

    // Evaluate AST against each candidate (or all records for linear scan)
    // Uses SearchEngine::Match (declared in SearchEngine.h)
    std::vector<Match> merged;
    std::vector<char> pathBuf;

    // Extract scoring term from AST (not raw input) — hoisted out of per-record loop
    std::string scoringTerm = extractScoringTerm(*ast);

    auto beforePhase = std::chrono::steady_clock::now();

    if (useTrigramIndex) {
        // Trigram-filtered path with parallel dispatch + software prefetch
        const size_t candidateCount = candidates.size();
        constexpr size_t PARALLEL_THRESHOLD = 10000;
        constexpr size_t PREFETCH_DIST = 8;

        if (candidateCount >= PARALLEL_THRESHOLD) {
            // Large candidate set — parallel dispatch_apply with prefetch
            unsigned numThreads = std::thread::hardware_concurrency();
            if (numThreads < 1) numThreads = 1;
            if (numThreads > 32) numThreads = 32;
            size_t chunkSize = (candidateCount + numThreads - 1) / numThreads;

            __block std::vector<std::vector<Match>> threadResults(numThreads);
            uint64_t capturedGen = myGen;
            const uint32_t* candidatesData = candidates.data();
            const auto* astPtr = ast.get();
            const auto* typesPtr = types_.data();
            const auto* sizesPtr = sizes_.data();
            const auto* modTimesPtr = modTimes_.data();
            const auto& namePool = namePool_;
            const auto& origNamePool = origNamePool_;
            const auto& lowerPathPool = lowerPathPool_;
            const auto& pathPool = pathPool_;
            const auto& pIndices = pathIndices_;
            // Per-thread RE2 clones to avoid DFA mutex contention
            auto perThreadCaches = cloneRegexCachePerThread(regexCache, numThreads);
            auto* ptcPtr = perThreadCaches.data();
            const auto& sTerm = scoringTerm;

            dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
            dispatch_apply(numThreads, queue, ^(size_t t) {
                size_t start = t * chunkSize;
                size_t end = std::min(start + chunkSize, candidateCount);
                if (start >= end) return;
                auto& local = threadResults[t];
                std::vector<char> localPathBuf;
                const auto& regCache = ptcPtr[t];
                for (size_t ci = start; ci < end; ci++) {
                    if ((ci & 1023) == 0 && genPtr->load(std::memory_order_relaxed) != capturedGen) return;
                    if (ci + PREFETCH_DIST < end) {
                        uint32_t futureIdx = candidatesData[ci + PREFETCH_DIST];
                        __builtin_prefetch(typesPtr + futureIdx, 0, 0);
                        __builtin_prefetch(namePool.entries() + futureIdx, 0, 0);
                    }
                    uint32_t idx = candidatesData[ci];
                    if (typesPtr[idx] == 0) continue;
                    const char* nd = namePool.data(idx);
                    uint16_t nl = namePool.length(idx);
                    uint32_t pi = pIndices[idx];
                    const char* pd = lowerPathPool.data(pi);
                    uint16_t pl = lowerPathPool.length(pi);
                    const char* ond = origNamePool.data(idx);
                    uint16_t onl = origNamePool.length(idx);
                    const char* opd = pathPool.data(pi);
                    uint16_t opl = pathPool.length(pi);
                    if (!evalNode(*astPtr, typesPtr[idx], sizesPtr[idx],
                                  static_cast<time_t>(modTimesPtr[idx]),
                                  nd, nl, pd, pl, ond, onl, opd, opl,
                                  localPathBuf, regCache)) continue;
                    uint8_t priority = 2;
                    if (!sTerm.empty()) {
                        if (me::simdContains(nd, nl, sTerm.data(), sTerm.size())) {
                            priority = namePriority(nd, nl, sTerm.data(), sTerm.size());
                        } else {
                            priority = 3;
                        }
                    }
                    local.push_back({idx, priority, static_cast<uint32_t>(pl + 1 + nl)});
                }
            });

            if (genPtr->load(std::memory_order_relaxed) != myGen) return {};
            for (auto& v : threadResults) {
                merged.insert(merged.end(), v.begin(), v.end());
            }
        } else {
            // Small candidate set — single-threaded with prefetch
            const uint32_t* candidatesData = candidates.data();
            const auto* smallTypesPtr = types_.data();
            const auto* smallSizesPtr = sizes_.data();
            const auto* smallModTimesPtr = modTimes_.data();
            for (size_t ci = 0; ci < candidateCount; ci++) {
                if ((ci & 1023) == 0 && genPtr->load(std::memory_order_relaxed) != myGen) return {};
                if (ci + PREFETCH_DIST < candidateCount) {
                    uint32_t futureIdx = candidatesData[ci + PREFETCH_DIST];
                    __builtin_prefetch(smallTypesPtr + futureIdx, 0, 0);
                    __builtin_prefetch(namePool_.entries() + futureIdx, 0, 0);
                }
                uint32_t idx = candidatesData[ci];
                if (smallTypesPtr[idx] == 0) continue;
                const char* nd = namePool_.data(idx);
                uint16_t nl = namePool_.length(idx);
                uint32_t pi = pathIndices_[idx];
                const char* pd = lowerPathPool_.data(pi);
                uint16_t pl = lowerPathPool_.length(pi);
                const char* ond = origNamePool_.data(idx);
                uint16_t onl = origNamePool_.length(idx);
                const char* opd = pathPool_.data(pi);
                uint16_t opl = pathPool_.length(pi);
                if (!evalNode(*ast, smallTypesPtr[idx], smallSizesPtr[idx],
                              static_cast<time_t>(smallModTimesPtr[idx]),
                              nd, nl, pd, pl, ond, onl, opd, opl,
                              pathBuf, regexCache)) continue;
                uint8_t priority = 2;
                if (!scoringTerm.empty()) {
                    if (me::simdContains(nd, nl, scoringTerm.data(), scoringTerm.size())) {
                        priority = namePriority(nd, nl, scoringTerm.data(), scoringTerm.size());
                    } else {
                        priority = 3;
                    }
                }
                merged.push_back({idx, priority, static_cast<uint32_t>(pl + 1 + nl)});
            }
        }
    } else {
        // Linear scan — parallelize with GCD dispatch_apply
        unsigned numThreads = std::thread::hardware_concurrency();
        if (numThreads < 1) numThreads = 1;
        if (numThreads > 32) numThreads = 32;
        // For small record sets, don't over-parallelize
        if (totalSize < 10000) numThreads = 1;

        size_t chunkSize = (totalSize + numThreads - 1) / numThreads;
        __block std::vector<std::vector<Match>> threadResults(numThreads);

        uint64_t capturedGen = myGen;
        bool pureFilter = needs.isPureFilter();

        // Capture references for block
        const auto* astPtr = ast.get();
        const auto* typesPtr = types_.data();
        const auto* sizesPtr = sizes_.data();
        const auto* modTimesPtr = modTimes_.data();
        const auto& namePool = namePool_;
        const auto& origNamePool = origNamePool_;
        const auto& lowerPathPool = lowerPathPool_;
        const auto& pathPool = pathPool_;
        const auto& pIndices = pathIndices_;
        // Per-thread RE2 clones to avoid DFA mutex contention
        auto perThreadCaches = cloneRegexCachePerThread(regexCache, numThreads);
        auto* ptcPtr = perThreadCaches.data();
        const auto& sTerm = scoringTerm;

        dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
        dispatch_apply(numThreads, queue, ^(size_t t) {
            size_t start = t * chunkSize;
            size_t end = std::min(start + chunkSize, totalSize);
            if (start >= end) return;

            auto& local = threadResults[t];
            std::vector<char> localPathBuf;
            const auto& regCache = ptcPtr[t];

            if (pureFilter) {
                // ── SIMD-batched pure-filter fast path ──
                size_t idx = start;

                size_t alignedStart = (start + 15) & ~size_t(15);
                if (alignedStart > end) alignedStart = end;

                // Scalar pre-amble
                for (; idx < alignedStart; idx++) {
                    if (typesPtr[idx] == 0) continue;
                    if (!evalNode(*astPtr, typesPtr[idx], sizesPtr[idx],
                                  static_cast<time_t>(modTimesPtr[idx]),
                                  nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0,
                                  localPathBuf, regCache)) continue;
                    local.push_back({static_cast<uint32_t>(idx), 2, 0});
                }

                // SIMD main loop: 16 records per iteration
                for (; idx + 16 <= end; idx += 16) {
                    if ((idx & 4095) == 0 && genPtr->load(std::memory_order_relaxed) != capturedGen) return;

                    uint16_t liveMask = me::simdTypeLive16(typesPtr + idx);
                    if (liveMask == 0) continue;

                    while (liveMask) {
                        int bit = __builtin_ctz(liveMask);
                        size_t ri = idx + bit;
                        if (evalNode(*astPtr, typesPtr[ri], sizesPtr[ri],
                                     static_cast<time_t>(modTimesPtr[ri]),
                                     nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0,
                                     localPathBuf, regCache)) {
                            local.push_back({static_cast<uint32_t>(ri), 2, 0});
                        }
                        liveMask &= liveMask - 1;
                    }
                }

                // Scalar tail
                for (; idx < end; idx++) {
                    if (typesPtr[idx] == 0) continue;
                    if (!evalNode(*astPtr, typesPtr[idx], sizesPtr[idx],
                                  static_cast<time_t>(modTimesPtr[idx]),
                                  nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0,
                                  localPathBuf, regCache)) continue;
                    local.push_back({static_cast<uint32_t>(idx), 2, 0});
                }
            } else {
                // ── Full evaluation path (needs string access) ──
                for (size_t idx = start; idx < end; idx++) {
                    if ((idx & 4095) == 0 && genPtr->load(std::memory_order_relaxed) != capturedGen) return;

                    if (typesPtr[idx] == 0) continue;
                    const char* nd = namePool.data(static_cast<uint32_t>(idx));
                    uint16_t nl = namePool.length(static_cast<uint32_t>(idx));
                    uint32_t pi = pIndices[idx];
                    const char* pd = lowerPathPool.data(pi);
                    uint16_t pl = lowerPathPool.length(pi);
                    const char* ond = origNamePool.data(static_cast<uint32_t>(idx));
                    uint16_t onl = origNamePool.length(static_cast<uint32_t>(idx));
                    const char* opd = pathPool.data(pi);
                    uint16_t opl = pathPool.length(pi);

                    if (!evalNode(*astPtr, typesPtr[idx], sizesPtr[idx],
                                  static_cast<time_t>(modTimesPtr[idx]),
                                  nd, nl, pd, pl, ond, onl, opd, opl,
                                  localPathBuf, regCache)) continue;

                    uint8_t priority = 2;
                    if (!sTerm.empty()) {
                        if (me::simdContains(nd, nl, sTerm.data(), sTerm.size())) {
                            priority = namePriority(nd, nl, sTerm.data(), sTerm.size());
                        } else {
                            priority = 3;
                        }
                    }
                    uint32_t pLen = static_cast<uint32_t>(pl + 1 + nl);
                    local.push_back({static_cast<uint32_t>(idx), priority, pLen});
                }
            }
        });

        // Check if superseded after dispatch_apply
        if (genPtr->load(std::memory_order_relaxed) != myGen) return {};

        // Merge thread-local results
        for (auto& v : threadResults) {
            merged.insert(merged.end(), v.begin(), v.end());
        }
    }

    auto afterPhase = std::chrono::steady_clock::now();

    // Aggregate post-pass filters (empty:/childcount:/child:/child-file:/
    // child-folder:/dupe:) need pathIdxToRecords_ / pathLookup_, so they run
    // while the lock is held.
    if (!aggregateNodes.empty()) {
        applyAggregateFilters(merged, aggregateNodes);
    }

    // Result controls: distinct:/unique: dedupe the candidate set.
    if (ast->distinctBy && !ast->distinctBy->empty()) {
        applyDistinctFilter(merged, *ast->distinctBy, ast->uniqueMode);
    } else if (ast->uniqueMode) {
        applyDistinctFilter(merged, "name", true);
    }

    // Requested sort: `sort:` / `sorted:` on the query root. Default is the
    // engine's relevance ordering (match quality, then path length).
    SortSpec sortSpec;
    if (ast->sort) sortSpec = *ast->sort;

    size_t resultCount = merged.size();
    if (maxResults > 0 && resultCount > maxResults) resultCount = maxResults;
    // count: query-level result cap.
    if (ast->limit && *ast->limit < resultCount) resultCount = *ast->limit;

    std::vector<uint32_t> result;
    std::chrono::steady_clock::time_point beforeSort{}, afterSort{}, beforeUnlock{};

    if (sortSpec.isDefault() && !sortSpec.foldersFirst) {
        // Relevance ordering needs no column access — release the lock first
        // so the sort does not block other threads.
        beforeUnlock = std::chrono::steady_clock::now();
        lock.unlock();

        beforeSort = std::chrono::steady_clock::now();
        if (resultCount < merged.size()) {
            std::partial_sort(merged.begin(), merged.begin() + resultCount, merged.end(),
                              relevanceLess);
        } else {
            std::sort(merged.begin(), merged.end(), relevanceLess);
        }
        afterSort = std::chrono::steady_clock::now();

        result.reserve(resultCount);
        for (size_t i = 0; i < resultCount; i++) {
            result.push_back(merged[i].idx);
        }
    } else {
        // Field access (and folders-first grouping) requires the lock.
        beforeSort = std::chrono::steady_clock::now();
        sortMatches(merged, sortSpec, resultCount, result);
        afterSort = std::chrono::steady_clock::now();

        beforeUnlock = std::chrono::steady_clock::now();
        lock.unlock();
    }

    // Populate timing
    auto toMs = [](auto dur) { return std::chrono::duration<double, std::milli>(dur).count(); };
    timing.totalMs = toMs(std::chrono::steady_clock::now() - queryStart);
    timing.lockWaitMs = toMs(afterLock - beforeLock);
    timing.lockHeldMs = toMs(beforeUnlock - afterLock);
    timing.sortMs = toMs(afterSort - beforeSort);
    timing.trigramMs = toMs(afterTrigram - beforeTrigram);
    timing.phase1Ms = 0;
    timing.phase2Ms = toMs(afterPhase - beforePhase);
    timing.totalRecords = totalSize;
    timing.candidates = candidates.size();
    timing.nameMatches = 0;
    timing.pathMatches = 0;
    timing.resultCount = result.size();
    timing.usedTrigram = useTrigramIndex;
    if (useTrigramIndex) {
        if (!trigramKey.empty()) {
            timing.searchPath = "advanced-trigram";
        } else if (extOk && !extractExtFilterValues(*ast).empty()) {
            timing.searchPath = "advanced-ext-index";
        } else if (!bestPathSegTerm(*ast).empty()) {
            timing.searchPath = "advanced-path-trigram";
        } else {
            timing.searchPath = "advanced-regex-trigram";
        }
    } else if (needs.isPureFilter()) {
        timing.searchPath = "advanced-pure-filter-soa-gcd";
    } else {
        timing.searchPath = "advanced-linear-gcd";
    }

    auto ms = static_cast<long long>(timing.totalMs);
    if (ms > 100) {
        LOG_INFO("SearchEngine", "QueryAdvanced \"" << input << "\" total=" << ms
            << "ms | path=" << timing.searchPath
            << " candidates=" << timing.candidates
            << " results=" << timing.resultCount
            << " totalRecords=" << timing.totalRecords);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Everything-compatible aggregate filters
//
// `empty:`, `childcount:`, `child:` and `dupe:` need data that spans multiple
// records (how many children a directory has, which names repeat), so they
// cannot be evaluated per record. The parser strips them off the AST and the
// engine applies them here as a post-pass over the candidate set.
// ---------------------------------------------------------------------------

namespace {

/// Recursively collect aggregate FILTER nodes and move them out of the AST.
/// Ownership transfers to `storage` so the pointers in `out` stay valid for
/// the lifetime of the query. Mirrors the sort extraction in QueryParser.cpp.
bool extractAggregateFilters(QueryNode& node,
                             std::vector<std::unique_ptr<QueryNode>>& storage,
                             std::vector<const QueryNode*>& out) {
    if (node.type == QueryNodeType::FILTER &&
        QueryFilterParser::isAggregateFilter(node.filterName)) {
        return true;   // caller moves this node into storage
    }

    bool found = false;
    for (auto& child : node.children) {
        if (!child) continue;
        if (extractAggregateFilters(*child, storage, out)) {
            storage.push_back(std::move(child));
            out.push_back(storage.back().get());
            found = true;
        }
    }

    if (found) {
        node.children.erase(
            std::remove_if(node.children.begin(), node.children.end(),
                           [](const std::unique_ptr<QueryNode>& c) { return !c; }),
            node.children.end());
        // A NOT node left childless would match everything; give it a
        // match-all child so the negation yields nothing instead.
        if (node.type == QueryNodeType::NOT && node.children.empty()) {
            node.children.push_back(QueryNode::makeAnd({}));
        }
    }
    return false;
}

/// True when a child name matches the child:/child-file:/child-folder:
/// argument. Everything treats a pattern containing wildcards as a whole-name
/// glob; otherwise it is a case-insensitive substring match.
static bool childNameMatches(std::string_view nameLower, const std::string& pattern) {
    if (pattern.empty()) return true;
    if (pattern.find('*') != std::string::npos ||
        pattern.find('?') != std::string::npos)
        return globMatchImpl(pattern, nameLower.data(), nameLower.size());
    return nameLower.find(pattern) != std::string_view::npos;
}

/// Lowercase and expand a `dupe-from:` folder, dropping trailing separators.
static std::string normalizeFolderPattern(const std::string& raw) {
    std::string p = me::toLower(raw);
    if (!p.empty() && p[0] == '~' && (p.size() == 1 || p[1] == '/')) {
        if (const char* home = std::getenv("HOME")) {
            p = me::toLower(std::string(home) + p.substr(1));
        }
    }
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

/// True when `path` (a lowercased parent path) is the folder itself or a
/// subfolder of it. With wildcards, the folder path and every ancestor are
/// tested so subfolders are still included.
static bool pathInFolder(std::string_view path, const std::string& pattern) {
    if (pattern.empty()) return true;
    const bool hasWildcard = pattern.find('*') != std::string::npos ||
                             pattern.find('?') != std::string::npos;
    if (!hasWildcard) {
        if (path == pattern) return true;
        return path.size() > pattern.size() &&
               path.compare(0, pattern.size(), pattern) == 0 &&
               path[pattern.size()] == '/';
    }
    if (globMatchImpl(pattern, path.data(), path.size())) return true;
    size_t pos = path.size();
    while (pos > 0) {
        pos = path.rfind('/', pos - 1);
        if (pos == std::string::npos) break;
        std::string_view ancestor = path.substr(0, pos);
        if (!ancestor.empty() &&
            globMatchImpl(pattern, ancestor.data(), ancestor.size())) {
            return true;
        }
    }
    return false;
}

/// Properties selectable by dupe:/distinct:/unique:.
struct DupeProps {
    bool name = false;
    bool size = false;
    bool ext = false;
    bool dateModified = false;
    bool dateCreated = false;
    bool dateAccessed = false;
    bool path = false;

    bool any() const {
        return name || size || ext || dateModified || dateCreated ||
               dateAccessed || path;
    }
};

/// Parse a ';'-separated property list. An empty (or unknown-only) list
/// means `name`, matching Everything's default.
static DupeProps parseDupeProps(const std::string& raw) {
    DupeProps props;
    std::string lower = me::toLower(raw);
    size_t start = 0;
    for (size_t i = 0; i <= lower.size(); i++) {
        if (i != lower.size() && lower[i] != ';' && lower[i] != ',') continue;
        size_t b = start, e = i;
        while (b < e && (lower[b] == ' ' || lower[b] == '\t')) b++;
        while (e > b && (lower[e - 1] == ' ' || lower[e - 1] == '\t')) e--;
        std::string prop = lower.substr(b, e - b);
        if (prop == "name" || prop == "filename") props.name = true;
        else if (prop == "size" || prop == "filesize") props.size = true;
        else if (prop == "ext" || prop == "extension") props.ext = true;
        else if (prop == "dm" || prop == "datemodified" || prop == "date-modified")
            props.dateModified = true;
        else if (prop == "dc" || prop == "datecreated" || prop == "date-created")
            props.dateCreated = true;
        else if (prop == "da" || prop == "dateaccessed" || prop == "date-accessed")
            props.dateAccessed = true;
        else if (prop == "path" || prop == "fullpath" || prop == "full-path")
            props.path = true;
        start = i + 1;
    }
    if (!props.any()) props.name = true;
    return props;
}

} // namespace

void SearchEngine::applyAggregateFilters(std::vector<Match>& matches,
                                         const std::vector<const QueryNode*>& nodes) const {
    if (nodes.empty() || matches.empty()) return;

    bool needChildData = false;
    for (const auto* n : nodes) {
        const std::string& f = n->filterName;
        if (f == "empty" || f == "childcount" || f == "child-count" ||
            f == "child-file-count" || f == "child-folder-count" ||
            f == "child" || f == "child-file" || f == "child-folder") {
            needChildData = true;
            break;
        }
    }

    // Duplicate keys are computed once over the whole index, not just the
    // candidate set — a duplicate may live outside the current search.
    auto makeDupeKey = [&](const DupeProps& props, uint32_t idx) {
        std::string key;
        if (props.name) {
            key.append(namePool_.data(idx), namePool_.length(idx));
            key.push_back('\x01');
        }
        if (props.size) {
            key.append(std::to_string(sizes_[idx]));
            key.push_back('\x01');
        }
        if (props.ext) {
            std::string_view ext = getExtLower(namePool_.data(idx),
                                               namePool_.length(idx));
            key.append(ext);
            key.push_back('\x01');
        }
        if (props.dateModified || props.dateCreated || props.dateAccessed) {
            key.append(std::to_string(modTimes_[idx]));
            key.push_back('\x01');
        }
        if (props.path) {
            uint32_t pi = pathIndices_[idx];
            key.append(lowerPathPool_.data(pi), lowerPathPool_.length(pi));
            key.push_back('/');
            key.append(namePool_.data(idx), namePool_.length(idx));
            key.push_back('\x01');
        }
        return key;
    };

    std::unordered_map<std::string, std::unordered_set<std::string>> dupeKeys;
    for (const auto* n : nodes) {
        if (n->filterName != "dupe") continue;
        if (dupeKeys.count(n->filterArg)) continue;
        const DupeProps props = parseDupeProps(n->filterArg);
        std::unordered_map<std::string, uint32_t> counts;
        counts.reserve(types_.size() / 4 + 16);
        for (uint32_t i = 0; i < types_.size(); i++) {
            if (types_[i] == 0) continue;
            counts[makeDupeKey(props, i)]++;
        }
        auto& keys = dupeKeys[n->filterArg];
        for (auto& [k, c] : counts) {
            if (c > 1) keys.insert(std::move(k));
        }
    }

    // dupe-min:/dupe-max: constrain the size of each duplicate group. The
    // grouping properties come from the dupe: filter (Everything's default
    // grouping is the file name).
    const QueryNode* dupeGroupNode = nullptr;
    const QueryNode* dupeCountNode = nullptr;
    std::vector<const QueryNode*> dupeFromNodes;
    bool needGroupCounts = false;
    for (const auto* n : nodes) {
        if (!dupeGroupNode && n->filterName == "dupe") dupeGroupNode = n;
        if (!dupeCountNode && n->filterName == "dupe-count") dupeCountNode = n;
        if (n->filterName == "dupe-from") dupeFromNodes.push_back(n);
        if (n->filterName == "dupe-min" || n->filterName == "dupe-max")
            needGroupCounts = true;
    }
    DupeProps dupeGroupProps;
    dupeGroupProps.name = true;
    if (dupeGroupNode) dupeGroupProps = parseDupeProps(dupeGroupNode->filterArg);

    std::unordered_map<std::string, uint32_t> dupeGroupCounts;
    if (needGroupCounts) {
        dupeGroupCounts.reserve(types_.size() / 4 + 16);
        for (uint32_t i = 0; i < types_.size(); i++) {
            if (types_[i] == 0) continue;
            dupeGroupCounts[makeDupeKey(dupeGroupProps, i)]++;
        }
    }

    // dupe-from: a group qualifies when at least one of its members lives in
    // (or below) the given folder. Collect qualifying groups per filter so
    // multiple dupe-from: clauses can be combined with AND.
    std::unordered_map<const QueryNode*, std::unordered_set<std::string>> dupeFromGroups;
    for (const auto* n : dupeFromNodes) {
        const std::string folder = normalizeFolderPattern(n->filterArg);
        auto& groups = dupeFromGroups[n];
        groups.reserve(types_.size() / 8 + 16);
        for (uint32_t i = 0; i < types_.size(); i++) {
            if (types_[i] == 0) continue;
            const uint32_t pi = pathIndices_[i];
            std::string_view parent(lowerPathPool_.data(pi),
                                    lowerPathPool_.length(pi));
            if (pathInFolder(parent, folder)) {
                groups.insert(makeDupeKey(dupeGroupProps, i));
            }
        }
    }

    std::vector<Match> kept;
    kept.reserve(matches.size());

    std::string ownPath;
    for (const auto& m : matches) {
        const uint32_t idx = m.idx;
        if (idx >= types_.size() || types_[idx] == 0) continue;
        const bool isDir = (types_[idx] == 2);

        uint32_t ownPathIdx = UINT32_MAX;
        if (isDir && needChildData) {
            const uint32_t parentPathIdx = pathIndices_[idx];
            ownPath.clear();
            ownPath.append(lowerPathPool_.data(parentPathIdx),
                           lowerPathPool_.length(parentPathIdx));
            if (ownPath.empty() || ownPath.back() != '/') ownPath.push_back('/');
            ownPath.append(namePool_.data(idx), namePool_.length(idx));
            uint32_t found = findLowerPathIdxUnlocked(ownPath);
            if (found != PathIndexTable::kNotFound) ownPathIdx = found;
        }

        uint32_t childTotal = 0, childFiles = 0, childFolders = 0;
        bool countsReady = false;
        auto ensureCounts = [&]() {
            if (countsReady) return;
            countsReady = true;
            if (!isDir || ownPathIdx == UINT32_MAX ||
                ownPathIdx >= pathIdxToRecords_.size()) return;
            for (uint32_t k : pathIdxToRecords_[ownPathIdx]) {
                if (k >= types_.size() || types_[k] == 0) continue;
                childTotal++;
                if (types_[k] == 2) childFolders++;
                else childFiles++;
            }
        };

        bool ok = true;
        for (const auto* n : nodes) {
            const std::string& f = n->filterName;

            if (f == "empty") {
                ensureCounts();
                ok = isDir && childTotal == 0;
            } else if (f == "childcount" || f == "child-count") {
                ensureCounts();
                ok = isDir && compareNumeric(childTotal, n->op, n->numVal1, n->numVal2);
            } else if (f == "child-file-count") {
                ensureCounts();
                ok = isDir && compareNumeric(childFiles, n->op, n->numVal1, n->numVal2);
            } else if (f == "child-folder-count") {
                ensureCounts();
                ok = isDir && compareNumeric(childFolders, n->op, n->numVal1, n->numVal2);
            } else if (f == "child" || f == "child-file" || f == "child-folder") {
                ok = false;
                if (isDir && ownPathIdx != UINT32_MAX &&
                    ownPathIdx < pathIdxToRecords_.size()) {
                    const uint8_t wantType = (f == "child-file") ? 1
                                           : (f == "child-folder") ? 2 : 0;
                    const std::string pattern = me::toLower(n->filterArg);
                    for (uint32_t k : pathIdxToRecords_[ownPathIdx]) {
                        if (k >= types_.size() || types_[k] == 0) continue;
                        const bool kIsDir = (types_[k] == 2);
                        if (wantType == 1 && kIsDir) continue;
                        if (wantType == 2 && !kIsDir) continue;
                        if (childNameMatches(namePool_.view(k), pattern)) {
                            ok = true;
                            break;
                        }
                    }
                }
            } else if (f == "dupe") {
                auto kit = dupeKeys.find(n->filterArg);
                if (kit == dupeKeys.end()) {
                    ok = false;
                } else {
                    ok = kit->second.count(makeDupeKey(parseDupeProps(n->filterArg), idx)) > 0;
                }
            } else if (f == "dupe-min" || f == "dupe-max") {
                auto cit = dupeGroupCounts.find(makeDupeKey(dupeGroupProps, idx));
                uint64_t count = (cit == dupeGroupCounts.end()) ? 0 : cit->second;
                ok = (f == "dupe-min") ? (count >= n->numVal1)
                                       : (count <= n->numVal1);
            } else if (f == "dupe-from") {
                auto git = dupeFromGroups.find(n);
                ok = git != dupeFromGroups.end() &&
                     git->second.count(makeDupeKey(dupeGroupProps, idx)) > 0;
            }

            if (!ok) break;
        }

        if (ok) kept.push_back(m);
    }

    // dupe-count: caps how many members of each duplicate group are shown.
    // It only applies together with dupe:, mirroring Everything.
    if (dupeCountNode && dupeGroupNode && dupeCountNode->numVal1 > 0) {
        const uint32_t cap = static_cast<uint32_t>(dupeCountNode->numVal1);
        std::unordered_map<std::string, uint32_t> shown;
        std::vector<Match> capped;
        capped.reserve(kept.size());
        for (const auto& m : kept) {
            uint32_t& seen = shown[makeDupeKey(dupeGroupProps, m.idx)];
            if (seen >= cap) continue;
            seen++;
            capped.push_back(m);
        }
        kept.swap(capped);
    }

    matches.swap(kept);
}

void SearchEngine::applyDistinctFilter(std::vector<Match>& matches,
                                       const std::string& distinctBy,
                                       bool uniqueMode) const {
    if (matches.empty()) return;

    // Parse the ';'-separated property list (name/size/ext/path). Empty = name.
    std::vector<std::string> props;
    if (distinctBy.empty()) {
        props.push_back("name");
    } else {
        std::string lower = me::toLower(distinctBy);
        size_t start = 0;
        for (size_t i = 0; i <= lower.size(); i++) {
            if (i == lower.size() || lower[i] == ';' || lower[i] == ',') {
                if (i > start) props.push_back(lower.substr(start, i - start));
                start = i + 1;
            }
        }
    }
    if (props.empty()) props.push_back("name");

    auto keyFor = [&](uint32_t idx) {
        std::string key;
        for (auto& p : props) {
            if (p == "name") {
                key.append(namePool_.data(idx), namePool_.length(idx));
            } else if (p == "size") {
                key.append(std::to_string(sizes_[idx]));
            } else if (p == "ext" || p == "extension") {
                std::string_view ext = getExtLower(namePool_.data(idx), namePool_.length(idx));
                key.append(ext);
            } else if (p == "path" || p == "fullpath") {
                uint32_t pi = pathIndices_[idx];
                key.append(pathPool_.data(pi), pathPool_.length(pi));
                key.push_back('/');
                key.append(namePool_.data(idx), namePool_.length(idx));
            }
            key.push_back('\x01');
        }
        return key;
    };

    // First pass: count occurrences of each key.
    std::unordered_map<std::string, uint32_t> counts;
    counts.reserve(matches.size());
    for (const auto& m : matches) {
        if (m.idx >= types_.size() || types_[m.idx] == 0) continue;
        counts[keyFor(m.idx)]++;
    }

    // Second pass: keep (distinct) first of each key, or (unique) only keys
    // that appear exactly once.
    std::unordered_set<std::string> seen;
    std::vector<Match> kept;
    kept.reserve(matches.size());
    for (const auto& m : matches) {
        if (m.idx >= types_.size() || types_[m.idx] == 0) continue;
        std::string key = keyFor(m.idx);
        if (uniqueMode) {
            if (counts[key] == 1) kept.push_back(m);
        } else {
            if (seen.insert(key).second) kept.push_back(m);
        }
    }
    matches.swap(kept);
}

// ---------------------------------------------------------------------------
// Everything-compatible sorting
//
// Everything exposes a `sort:` / `sorted:` modifier with the same field names
// as its column headers. The engine's default ordering stays relevance-based;
// an explicit sort: request switches to the requested field.
// ---------------------------------------------------------------------------

namespace {

/// Compare `pathA + "/" + nameA` against `pathB + "/" + nameB` lexicographically
/// without materialising the concatenations. Everything's "Path" column sorts
/// by the full path string, which is not the same as sorting by (parent, name)
/// pairs — e.g. "/a/b/z" sorts before "/a/y" but "/a" sorts before "/a/b".
int compareFullPath(std::string_view pathA, std::string_view nameA,
                    std::string_view pathB, std::string_view nameB) {
    const size_t lenA = pathA.size() + 1 + nameA.size();
    const size_t lenB = pathB.size() + 1 + nameB.size();
    const size_t n = lenA < lenB ? lenA : lenB;
    for (size_t i = 0; i < n; i++) {
        char ca = (i < pathA.size()) ? pathA[i]
                 : (i == pathA.size() ? '/' : nameA[i - pathA.size() - 1]);
        char cb = (i < pathB.size()) ? pathB[i]
                 : (i == pathB.size() ? '/' : nameB[i - pathB.size() - 1]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (lenA != lenB) return lenA < lenB ? -1 : 1;
    return 0;
}

/// Compare two records by a sort field. Returns <0, 0 or >0.
int compareByField(SortField field, uint32_t ia, uint32_t ib,
                   const StringPool& namePool,
                   const StringPool& pathPool,
                   const std::vector<uint32_t>& pathIndices,
                   const std::vector<uint8_t>& types,
                   const std::vector<uint64_t>& sizes,
                   const std::vector<int64_t>& modTimes) {
    switch (field) {
        case SortField::Name: {
            auto a = namePool.view(ia);
            auto b = namePool.view(ib);
            if (a != b) return a < b ? -1 : 1;
            return 0;
        }
        case SortField::Path: {
            return compareFullPath(pathPool.view(pathIndices[ia]), namePool.view(ia),
                                   pathPool.view(pathIndices[ib]), namePool.view(ib));
        }
        case SortField::Extension: {
            auto a = getExtLower(namePool.data(ia), namePool.length(ia));
            auto b = getExtLower(namePool.data(ib), namePool.length(ib));
            if (a != b) return a < b ? -1 : 1;
            return 0;
        }
        case SortField::Size: {
            if (sizes[ia] != sizes[ib]) return sizes[ia] < sizes[ib] ? -1 : 1;
            return 0;
        }
        case SortField::Type: {
            // Folders sort before files in ascending order.
            uint8_t ka = (types[ia] == 2) ? 0 : 1;
            uint8_t kb = (types[ib] == 2) ? 0 : 1;
            if (ka != kb) return ka < kb ? -1 : 1;
            return 0;
        }
        case SortField::DateModified:
        case SortField::DateCreated:
        case SortField::DateAccessed:
        case SortField::DateRun:
        case SortField::RunCount: {
            // macOS exposes mtime through getattrlistbulk here; birthtime and
            // atime fall back to it until they are persisted separately.
            if (modTimes[ia] != modTimes[ib]) return modTimes[ia] < modTimes[ib] ? -1 : 1;
            return 0;
        }
        case SortField::Depth: {
            auto pa = pathPool.view(pathIndices[ia]);
            auto pb = pathPool.view(pathIndices[ib]);
            uint32_t da = 0, db = 0;
            for (char c : pa) if (c == '/') da++;
            for (char c : pb) if (c == '/') db++;
            if (da != db) return da < db ? -1 : 1;
            return 0;
        }
        case SortField::NameLength: {
            uint16_t la = namePool.length(ia);
            uint16_t lb = namePool.length(ib);
            if (la != lb) return la < lb ? -1 : 1;
            return 0;
        }
        case SortField::Relevance:
            break;
    }
    return 0;
}

} // namespace

void SearchEngine::sortMatches(std::vector<Match>& matches, const SortSpec& spec,
                               size_t resultCount, std::vector<uint32_t>& out) const {
    const size_t n = matches.size();
    out.clear();
    if (n == 0) return;

    const size_t keep = (resultCount > 0 && resultCount < n) ? resultCount : n;
    out.reserve(keep);

    // Relevance ordering (the engine default) needs no field access.
    if (spec.isDefault()) {
        if (spec.foldersFirst) {
            // Partition before truncating so a limited result set still shows
            // the folders that sort ahead of the cut-off.
            std::sort(matches.begin(), matches.end(), relevanceLess);
            std::stable_partition(matches.begin(), matches.end(),
                [&](const Match& m) {
                    return m.idx < types_.size() && types_[m.idx] == 2;
                });
        } else if (keep < n) {
            std::partial_sort(matches.begin(), matches.begin() + keep, matches.end(),
                              relevanceLess);
        } else {
            std::sort(matches.begin(), matches.end(), relevanceLess);
        }
        for (size_t i = 0; i < keep; i++) out.push_back(matches[i].idx);
        return;
    }

    // Sort a permutation so the (larger) Match vector is not shuffled.
    std::vector<uint32_t> perm(n);
    for (size_t i = 0; i < n; i++) perm[i] = static_cast<uint32_t>(i);

    auto less = [&](uint32_t a, uint32_t b) {
        int c = compareByField(spec.field, matches[a].idx, matches[b].idx,
                               namePool_, pathPool_, pathIndices_,
                               types_, sizes_, modTimes_);
        if (c != 0) return spec.dir == SortDir::Asc ? (c < 0) : (c > 0);
        for (const auto& key : spec.extraKeys) {
            c = compareByField(key.first, matches[a].idx, matches[b].idx,
                               namePool_, pathPool_, pathIndices_,
                               types_, sizes_, modTimes_);
            if (c != 0) return key.second == SortDir::Asc ? (c < 0) : (c > 0);
        }
        // Stable, deterministic tie-break: fall back to relevance ordering.
        return relevanceLess(matches[a], matches[b]);
    };

    if (spec.foldersFirst) {
        // Partition before truncating: folders must lead the final window.
        std::sort(perm.begin(), perm.end(), less);
        std::stable_partition(perm.begin(), perm.end(),
            [&](uint32_t i) {
                uint32_t idx = matches[i].idx;
                return idx < types_.size() && types_[idx] == 2;
            });
    } else if (keep < n) {
        std::partial_sort(perm.begin(), perm.begin() + keep, perm.end(), less);
    } else {
        std::sort(perm.begin(), perm.end(), less);
    }

    for (size_t i = 0; i < keep; i++) out.push_back(matches[perm[i]].idx);
}
