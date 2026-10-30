// How a delimited file is written: what the user asked for, and what was resolved from it.
#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace xsv {

enum class Tri : int { Auto = 0, Yes = 1, No = 2 };

// The user's request. Auto/empty fields are resolved from the text by detectDialect().
struct ParseOptions {
    std::string delimiter;       // UTF-8, may be several bytes; empty = detect
    char quote = '"';            // 0 = no quoting
    bool doubleQuote = true;     // "" inside a quoted cell means one quote
    char escape = 0;             // e.g. '\\'; 0 = none
    Tri header = Tri::Auto;
    Tri rowNames = Tri::Auto;    // header one field short of the data, as R's write.table does
    Tri decimalComma = Tri::Auto;
    bool commentAuto = true;     // detect '#'-style preamble lines
    std::string comment;         // used when commentAuto is false; empty = none
    size_t skipLines = 0;
    bool trimCells = false;
    bool skipBlankLines = true;
};

// Fully resolved; this is what the table is actually read with.
struct Dialect {
    std::string delimiter = ",";
    char quote = '"';
    bool doubleQuote = true;
    char escape = 0;
    std::string comment;
    size_t skipLines = 0;
    bool hasHeader = true;
    bool rowNamesHeader = false;
    bool decimalComma = false;
    bool trimCells = false;
    bool skipBlankLines = true;
};

// extension is lower case without the dot ("tsv"); it only breaks ties between candidates.
Dialect detectDialect(std::string_view text, const ParseOptions& options, std::string_view extension);

}  // namespace xsv
