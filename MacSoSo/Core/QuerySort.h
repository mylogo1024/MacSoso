#pragma once
#include <string>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <utility>
#include <vector>

/// Sort fields supported by the `sort:` / `sorted:` search modifier.
///
/// Mirrors the sortable columns offered by voidtools Everything on Windows so
/// that queries written for Everything behave identically on macOS.
enum class SortField : uint8_t {
    Relevance,      ///< engine default: match quality, then path length
    Name,           ///< file name (case-insensitive byte order)
    Path,           ///< full path
    Size,           ///< file size in bytes
    Extension,      ///< file extension
    Type,           ///< folders before files (ascending) / files first (descending)
    DateModified,   ///< dm:
    DateCreated,    ///< dc:
    DateAccessed,   ///< da:
    DateRun,        ///< dr: — run history (not tracked on macOS, falls back to dm)
    Depth,          ///< number of path components
    NameLength,     ///< length of the file name in bytes
    RunCount,       ///< rc: — run count (not tracked on macOS, falls back to dm)
};

/// Ascending / descending direction for a sort field.
enum class SortDir : uint8_t { Asc, Desc };

/// A fully resolved sort request.
struct SortSpec {
    SortField field = SortField::Relevance;
    SortDir   dir   = SortDir::Asc;

    /// True when the user explicitly requested a sort (via `sort:` / `sorted:`).
    /// When false the engine keeps its relevance ordering.
    bool specified = false;

    /// Everything "folders first" display option: folders appear before files
    /// while keeping the chosen sort order within each group.
    bool foldersFirst = false;

    /// Additional sort columns, applied when the primary field ties.
    /// Everything separates columns with ';' (`sort:size;name`).
    std::vector<std::pair<SortField, SortDir>> extraKeys;

    bool isDefault() const { return !specified || field == SortField::Relevance; }

    const char* fieldName() const {
        switch (field) {
            case SortField::Relevance:    return "relevance";
            case SortField::Name:         return "name";
            case SortField::Path:         return "path";
            case SortField::Size:         return "size";
            case SortField::Extension:    return "extension";
            case SortField::Type:         return "type";
            case SortField::DateModified: return "datemodified";
            case SortField::DateCreated:  return "datecreated";
            case SortField::DateAccessed: return "dateaccessed";
            case SortField::DateRun:      return "daterun";
            case SortField::Depth:        return "depth";
            case SortField::NameLength:   return "namelength";
            case SortField::RunCount:     return "runcount";
        }
        return "relevance";
    }

    const char* dirName() const { return dir == SortDir::Asc ? "ascending" : "descending"; }

    /// Parse the argument of `sort:` / `sorted:`.
    ///
    /// Accepted forms (case-insensitive, Everything-compatible):
    ///   sort:name                      → Name ascending
    ///   sort:size-descending           → Size descending
    ///   sort:size:descending           → Size descending
    ///   sort:-size                     → Size descending (leading minus)
    ///   sort:+size                     → Size ascending (explicit plus)
    ///   sort:datemodified_descending   → DateModified descending
    ///   sort:size;name                 → Size, then Name (secondary key)
    ///   sort:name;size-descending      → Name ascending, then Size descending
    ///
    /// Unknown field names fall back to Relevance (engine default ordering).
    static SortSpec parse(const std::string& rawArg) {
        SortSpec spec;
        spec.specified = true;

        // Lowercase once; Everything treats the argument case-insensitively.
        std::string arg;
        arg.reserve(rawArg.size());
        for (char c : rawArg) {
            if (c == ' ' || c == '\t') continue;   // tolerate stray spaces
            arg.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (arg.empty()) return spec;

        // Everything separates sort columns with ';' — the first column is
        // the primary key, the rest break ties.
        bool havePrimary = false;
        size_t start = 0;
        while (start <= arg.size()) {
            size_t semi = arg.find(';', start);
            size_t end = (semi == std::string::npos) ? arg.size() : semi;
            std::string part = arg.substr(start, end - start);
            start = (semi == std::string::npos) ? arg.size() + 1 : semi + 1;
            if (part.empty()) continue;

            SortField field = SortField::Relevance;
            SortDir dir = SortDir::Asc;
            bool foldersFirst = false;
            parseOne(part, field, dir, foldersFirst);

            if (foldersFirst) spec.foldersFirst = true;
            if (field == SortField::Relevance && !foldersFirst) {
                if (!havePrimary) { spec.field = field; spec.dir = dir; havePrimary = true; }
                continue;   // don't chain meaningless relevance tie-breaks
            }
            if (!havePrimary) {
                spec.field = field;
                spec.dir = dir;
                havePrimary = true;
            } else {
                spec.extraKeys.emplace_back(field, dir);
            }
        }

        if (!havePrimary && spec.foldersFirst) {
            spec.field = SortField::Name;
        }
        return spec;
    }

    /// Parse one `field[-direction]` column.
    static void parseOne(const std::string& rawPart, SortField& field,
                         SortDir& dir, bool& foldersFirst) {
        std::string arg = rawPart;
        if (arg.empty()) return;

        // Leading '-' means descending, leading '+' means ascending.
        if (arg[0] == '-') {
            dir = SortDir::Desc;
            arg.erase(arg.begin());
        } else if (arg[0] == '+') {
            arg.erase(arg.begin());
        }
        if (arg.empty()) return;

        // A trailing direction keyword overrides whatever was parsed so far.
        static const char* kDescWords[] = {"descending", "desc", "d"};
        static const char* kAscWords[]  = {"ascending", "asc", "a"};

        auto matchesAny = [](const std::string& s, const char* const* words, size_t n) {
            for (size_t i = 0; i < n; i++)
                if (s == words[i]) return true;
            return false;
        };

        // Try the last separator as the field/direction boundary. Everything
        // uses '-' and ':' between the field and the direction; '_' is also
        // accepted because it is a common typo for either.
        static const char kSeparators[] = "-:_";
        for (size_t i = arg.size(); i-- > 0;) {
            char sep = arg[i];
            bool isSep = false;
            for (char s : kSeparators) if (s == sep) { isSep = true; break; }
            if (!isSep) continue;
            if (i + 1 >= arg.size()) break;
            std::string f = arg.substr(0, i);
            std::string dirWord = arg.substr(i + 1);
            if (matchesAny(dirWord, kDescWords, 3)) {
                dir = SortDir::Desc;
            } else if (matchesAny(dirWord, kAscWords, 3)) {
                dir = SortDir::Asc;
            } else {
                continue;   // separator is part of the field name itself
            }
            arg = f;
            break;
        }

        field = parseField(arg);

        // "folders-first" is a display option independent of the sort field;
        // it reorders folders before files while preserving the chosen order.
        if (arg == "folders-first" || arg == "foldersfirst" || arg == "folders_first") {
            foldersFirst = true;
            field = SortField::Name;
        }
    }

    /// Map a field alias to a SortField. Returns Relevance for unknown names.
    static SortField parseField(const std::string& arg) {
        struct Entry { const char* name; SortField field; };
        static const Entry table[] = {
            // Name
            {"name",           SortField::Name},
            {"filename",       SortField::Name},
            {"fn",             SortField::Name},
            // Path
            {"path",           SortField::Path},
            {"fullpath",       SortField::Path},
            {"fullname",       SortField::Path},
            {"fu",             SortField::Path},
            // Size
            {"size",           SortField::Size},
            {"filesize",       SortField::Size},
            {"fs",             SortField::Size},
            {"length",         SortField::Size},
            // Extension
            {"ext",            SortField::Extension},
            {"extension",      SortField::Extension},
            {"extensions",     SortField::Extension},
            // Type
            {"type",           SortField::Type},
            // Dates
            {"dm",             SortField::DateModified},
            {"datemodified",   SortField::DateModified},
            {"modified",       SortField::DateModified},
            {"writedate",      SortField::DateModified},
            {"dc",             SortField::DateCreated},
            {"datecreated",    SortField::DateCreated},
            {"created",        SortField::DateCreated},
            {"birthdate",      SortField::DateCreated},
            {"da",             SortField::DateAccessed},
            {"dateaccessed",   SortField::DateAccessed},
            {"accessed",       SortField::DateAccessed},
            {"accessdate",     SortField::DateAccessed},
            {"dr",             SortField::DateRun},
            {"daterun",        SortField::DateRun},
            {"run",            SortField::DateRun},
            // Structural
            {"depth",          SortField::Depth},
            {"len",            SortField::NameLength},
            {"namelength",     SortField::NameLength},
            {"rc",             SortField::RunCount},
            {"runcount",       SortField::RunCount},
            {"relevance",      SortField::Relevance},
            {"default",        SortField::Relevance},
            {"bestmatch",      SortField::Relevance},
        };
        for (const auto& e : table) {
            if (arg == e.name) return e.field;
        }
        return SortField::Relevance;
    }

    /// Whether a tokenizer word should be recognised as a sort modifier.
    static bool isSortKeyword(const std::string& lowerName) {
        return lowerName == "sort" || lowerName == "sorted" ||
               lowerName == "sortby" || lowerName == "orderby";
    }
};
