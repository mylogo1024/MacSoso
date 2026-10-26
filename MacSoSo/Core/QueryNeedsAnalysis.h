#pragma once
#include "QueryAST.h"

/// Result of analyzing which data fields a query AST actually needs.
struct QueryNeeds {
    bool needsName    = false;  // query contains TERM or len: filter
    bool needsPath    = false;  // query contains path:/nopath:/parent:/depth: filter or path-matching TERM
    bool needsType    = false;  // query contains file:/folder:/type: filter
    bool needsSize    = false;  // query contains size: filter
    bool needsModTime = false;  // query contains dm:/dc:/da:/datemodified:/datecreated:/dateaccessed: filter
    bool hasExtFilter = false;  // query contains ext: filter (can use extension index for pre-filtering)

    /// True when the query only uses type/size/modTime — no string access needed.
    bool isPureFilter() const { return !needsName && !needsPath; }
};

/// Walk the query AST and determine which record fields are needed.
inline QueryNeeds analyzeQueryNeeds(const QueryNode& node) {
    QueryNeeds needs;

    switch (node.type) {
    case QueryNodeType::TERM:
        switch (node.target) {
            case MatchTarget::NAME:
            case MatchTarget::STEM:
            case MatchTarget::EXTENSION:
                needs.needsName = true;
                break;
            case MatchTarget::FULL_PATH:
                needs.needsName = true;   // path + "/" + name
                needs.needsPath = true;
                break;
            case MatchTarget::PARENT_NAME:
            case MatchTarget::LOCATION:
            case MatchTarget::ANCESTOR_PATH:
            case MatchTarget::ANCESTOR_NAME:
                needs.needsPath = true;
                break;
        }
        if (node.matchPath) needs.needsPath = true;
        break;

    case QueryNodeType::FILTER: {
        const auto& f = node.filterName;
        if (f == "__tautology") {
            break;
        }
        if (node.dateSegment && *node.dateSegment >= 0) {
            needs.needsModTime = true;
            break;
        }
        if (f == "ext") {
            needs.needsName = true;     // extension is part of the name
            needs.hasExtFilter = true;  // enable extension index pre-filtering
        } else if (f == "len" || f == "name-length" || f == "namelen") {
            needs.needsName = true;
        } else if (f == "size" || f == "fs") {
            needs.needsSize = true;
        } else if (f == "file" || f == "folder") {
            needs.needsType = true;
        } else if (f == "type") {
            needs.needsType = true;
            // Media-type aliases inspect the extension, which needs the name;
            // type:file / type:folder only look at the record type.
            const auto& a = node.filterArg;
            if (a != "file" && a != "folder" && a != "dir" && a != "directory")
                needs.needsName = true;
        } else if (f == "dm" || f == "datemodified" || f == "date-modified" ||
                   f == "dc" || f == "datecreated" || f == "date-created" ||
                   f == "da" || f == "dateaccessed" || f == "date-accessed" ||
                   f == "dr" || f == "daterun") {
            needs.needsModTime = true;
        } else if (f == "path" || f == "nopath" || f == "parent" ||
                   f == "depth" || f == "full-path" || f == "fullpath" ||
                   f == "parent-name" || f == "parentname" ||
                   f == "ancestor" || f == "ancestor-name" ||
                   f == "root" || f == "full-path-length" ||
                   f == "fullpathlen") {
            needs.needsPath = true;
        } else if (f == "extension" || f == "extcontains") {
            needs.needsName = true;
        } else if (f == "__pathseg") {
            needs.needsPath = true;
            needs.needsName = true; // pathSegmentsMatch uses full path including name
        } else if (f == "content") {
            needs.needsName = true;
            needs.needsPath = true;
        } else {
            // Unknown filters are tautologies and need no record data.
        }
        break;
    }

    case QueryNodeType::AND:
    case QueryNodeType::OR:
    case QueryNodeType::NOT:
        for (auto& child : node.children) {
            QueryNeeds childNeeds = analyzeQueryNeeds(*child);
            needs.needsName    = needs.needsName    || childNeeds.needsName;
            needs.needsPath    = needs.needsPath    || childNeeds.needsPath;
            needs.needsType    = needs.needsType    || childNeeds.needsType;
            needs.needsSize    = needs.needsSize    || childNeeds.needsSize;
            needs.needsModTime = needs.needsModTime || childNeeds.needsModTime;
            needs.hasExtFilter = needs.hasExtFilter || childNeeds.hasExtFilter;
        }
        break;
    }

    return needs;
}
