#include "Dialect.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "Scanner.h"
#include "Text.h"

namespace xsv {

namespace {

constexpr size_t kSampleBytes = 1024 * 1024;
constexpr size_t kSampleRecords = 2000;
constexpr size_t kHeaderSampleRows = 200;
constexpr double kMinConsistency = 0.5;

// The first megabyte is plenty to recognise a format, cut at a line end to avoid half a record.
std::string_view takeSample(std::string_view text) {
    if (text.size() <= kSampleBytes) return text;
    std::string_view sample = text.substr(0, kSampleBytes);
    const size_t lastNewline = sample.find_last_of("\r\n");
    return lastNewline == std::string_view::npos ? sample : sample.substr(0, lastNewline + 1);
}

struct WidthProfile {
    size_t records = 0;
    size_t usualWidth = 0;
    size_t usualCount = 0;
    double consistency() const { return records ? static_cast<double>(usualCount) / static_cast<double>(records) : 0; }
    bool perfect() const { return records && usualCount == records; }
};

template <typename OnRecord>
void forEachRecord(std::string_view sample, const Dialect& d, size_t limit, OnRecord&& onRecord) {
    const char* p = sample.data();
    const char* end = p + sample.size();
    size_t unbalanced = 0, count = 0;
    while (p < end && count < limit) {
        if (startsWith(p, end, d.comment) || *p == '\n' || *p == '\r') {
            p = skipLine(p, end);
            continue;
        }
        p = onRecord(p, end, unbalanced);
        ++count;
    }
}

WidthProfile profileWidths(std::string_view sample, const Dialect& d) {
    std::map<size_t, size_t> widths;
    WidthProfile profile;
    forEachRecord(sample, d, kSampleRecords, [&](const char* p, const char* end, size_t& unbalanced) {
        size_t fields = 0;
        const char* next = scanRecord(p, end, d, unbalanced, [&](const char*, const char*, bool, bool) { ++fields; });
        ++widths[fields];
        ++profile.records;
        return next;
    });
    for (const auto& [width, count] : widths) {
        if (count > profile.usualCount) {
            profile.usualWidth = width;
            profile.usualCount = count;
        }
    }
    return profile;
}

std::string defaultDelimiter(std::string_view extension) {
    if (extension == "tsv" || extension == "tab") return "\t";
    if (extension == "psv") return "|";
    return ",";
}

std::string detectDelimiter(std::string_view sample, const Dialect& base, std::string_view extension) {
    struct Candidate {
        std::string delimiter;
        bool perfect;
        double score;
    };
    std::vector<Candidate> candidates;
    const std::string preferred = defaultDelimiter(extension);
    for (const char* delimiter : {"\t", ",", ";", "|"}) {
        Dialect d = base;
        d.delimiter = delimiter;
        const WidthProfile profile = profileWidths(sample, d);
        if (profile.usualWidth < 2 || profile.consistency() < kMinConsistency) continue;
        double score = profile.consistency() * (1.0 + std::log2(static_cast<double>(profile.usualWidth)));
        if (d.delimiter == preferred) score *= 1.5;
        candidates.push_back({d.delimiter, profile.perfect(), score});
    }
    if (candidates.empty()) return preferred;

    // One line that disagrees (usually the header) rules a candidate out against one that agrees
    // everywhere: decimal commas look like a wider delimiter on every row but the header.
    auto best = std::max_element(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.perfect != b.perfect ? b.perfect : a.score < b.score;
    });

    // Semicolons in the same count on every line do not happen by accident; commas do (1,5;2,5).
    if (best->delimiter == ",") {
        auto semicolon = std::find_if(candidates.begin(), candidates.end(), [](const Candidate& c) {
            return c.delimiter == ";" && c.perfect;
        });
        if (semicolon != candidates.end()) best = semicolon;
    }
    return best->delimiter;
}

struct Preamble {
    size_t lines = 0;
    bool lastCouldBeHeader = false;  // ##... lines followed by one #... line, as in VCF
    std::string_view lastLine;
};

Preamble readPreamble(std::string_view sample) {
    Preamble preamble;
    bool allDoubleHash = true;
    const char* p = sample.data();
    const char* end = p + sample.size();
    while (p < end && *p == '#') {
        const char* next = skipLine(p, end);
        const bool doubleHash = end - p >= 2 && p[1] == '#';
        preamble.lastCouldBeHeader = allDoubleHash && !doubleHash;
        allDoubleHash = allDoubleHash && doubleHash;
        preamble.lastLine = std::string_view(p, static_cast<size_t>(next - p));
        ++preamble.lines;
        p = next;
    }
    return preamble;
}

size_t countFields(std::string_view line, const Dialect& d) {
    size_t fields = 0, unbalanced = 0;
    scanRecord(line.data(), line.data() + line.size(), d, unbalanced,
               [&](const char*, const char*, bool, bool) { ++fields; });
    return fields;
}

std::vector<std::vector<std::string>> readSampleRows(std::string_view sample, const Dialect& d) {
    std::vector<std::vector<std::string>> rows;
    forEachRecord(sample, d, kHeaderSampleRows, [&](const char* p, const char* end, size_t& unbalanced) {
        rows.emplace_back();
        return scanRecord(p, end, d, unbalanced, [&](const char* b, const char* e, bool, bool) {
            rows.back().emplace_back(b, e);
        });
    });
    return rows;
}

bool isDecimal(std::string_view cell, char separator) {
    size_t i = (!cell.empty() && (cell[0] == '-' || cell[0] == '+')) ? 1 : 0;
    const size_t digitsBefore = i;
    while (i < cell.size() && std::isdigit(static_cast<unsigned char>(cell[i]))) ++i;
    if (i == digitsBefore || i >= cell.size() || cell[i] != separator) return false;
    const size_t digitsAfter = ++i;
    while (i < cell.size() && std::isdigit(static_cast<unsigned char>(cell[i]))) ++i;
    return i == cell.size() && i > digitsAfter;
}

// 709,037 is a decimal in Vienna and a thousand-grouped number in Boston.
bool couldBeGroupedThousands(std::string_view cell) {
    const size_t comma = cell.find(',');
    const size_t sign = !cell.empty() && (cell[0] == '-' || cell[0] == '+') ? 1 : 0;
    return comma - sign <= 3 && cell.size() - comma - 1 == 3;
}

// Semicolon files come from decimal-comma locales, so there the ambiguous shape counts as well.
bool looksLikeDecimalComma(const std::vector<std::vector<std::string>>& rows, bool semicolons) {
    size_t commas = 0, dots = 0;
    for (const auto& row : rows) {
        for (const std::string& cell : row) {
            commas += isDecimal(cell, ',') && (semicolons || !couldBeGroupedThousands(cell));
            dots += isDecimal(cell, '.');
        }
    }
    return commas > dots;
}

// A delimiter at the end of each data row gives the same widths as R's short header, except that
// the extra last cell is never filled.
bool lastCellAlwaysEmpty(const std::vector<std::vector<std::string>>& rows, size_t width) {
    for (size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].size() == width && !rows[i].back().empty()) return false;
    }
    return true;
}

size_t usualDataWidth(const std::vector<std::vector<std::string>>& rows) {
    std::map<size_t, size_t> widths;
    for (size_t i = 1; i < rows.size(); ++i) ++widths[rows[i].size()];
    size_t width = 0, best = 0;
    for (const auto& [w, count] : widths) {
        if (count > best) {
            width = w;
            best = count;
        }
    }
    return width;
}

// A first row that is text above numeric columns is a header; numbers above numbers are data.
// With no numeric column to judge by, a header is by far the likelier case.
bool looksLikeHeader(const std::vector<std::vector<std::string>>& rows, size_t shift, bool decimalComma) {
    if (rows.size() < 2) return false;
    size_t evidenceFor = 0, evidenceAgainst = 0;
    const std::vector<std::string>& first = rows[0];
    for (size_t i = 0; i < first.size(); ++i) {
        const size_t column = i + shift;
        size_t numbers = 0, others = 0;
        for (size_t r = 1; r < rows.size(); ++r) {
            if (column >= rows[r].size() || rows[r][column].empty()) continue;
            double value;
            (parseNumber(rows[r][column], decimalComma, value) ? numbers : others) += 1;
        }
        if (numbers == 0 || numbers < others * 4) continue;
        double value;
        (parseNumber(first[i], decimalComma, value) ? evidenceAgainst : evidenceFor) += 1;
    }
    return evidenceAgainst == 0 || evidenceFor > evidenceAgainst;
}

}  // namespace

Dialect detectDialect(std::string_view text, const ParseOptions& options, std::string_view extension) {
    Dialect d;
    d.quote = options.quote;
    d.doubleQuote = options.doubleQuote;
    d.escape = options.escape;
    d.skipLines = options.skipLines;
    d.trimCells = options.trimCells;
    d.skipBlankLines = options.skipBlankLines;
    d.comment = options.commentAuto ? std::string() : options.comment;

    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    {
        const char* p = text.data();
        const char* end = p + text.size();
        for (size_t i = 0; i < options.skipLines && p < end; ++i) p = skipLine(p, end);
        text.remove_prefix(static_cast<size_t>(p - text.data()));
    }
    const std::string_view sample = takeSample(text);

    Preamble preamble;
    if (options.commentAuto) {
        preamble = readPreamble(sample);
        if (preamble.lines) d.comment = "#";
    }

    d.delimiter = options.delimiter.empty() ? detectDelimiter(sample, d, extension) : options.delimiter;

    // A #-line as wide as the data below it is the header of a VCF-style file, not a comment.
    if (preamble.lastCouldBeHeader) {
        const WidthProfile body = profileWidths(sample, d);
        const size_t width = countFields(preamble.lastLine.substr(0, preamble.lastLine.find_last_not_of("\r\n") + 1), d);
        if (width >= 2 && width == body.usualWidth) d.comment = "##";
    }

    const std::vector<std::vector<std::string>> rows = readSampleRows(sample, d);

    d.decimalComma = options.decimalComma == Tri::Auto ? d.delimiter != "," && looksLikeDecimalComma(rows, d.delimiter == ";")
                                                       : options.decimalComma == Tri::Yes;

    const size_t dataWidth = rows.size() >= 2 ? usualDataWidth(rows) : 0;
    const bool shiftPossible = rows.size() >= 2 && rows[0].size() + 1 == dataWidth && !lastCellAlwaysEmpty(rows, dataWidth);
    const bool shift = options.rowNames == Tri::Auto ? shiftPossible : options.rowNames == Tri::Yes;
    d.hasHeader = options.header == Tri::Auto ? looksLikeHeader(rows, shift ? 1 : 0, d.decimalComma)
                                              : options.header == Tri::Yes;
    d.rowNamesHeader = d.hasHeader && shift;
    return d;
}

}  // namespace xsv
