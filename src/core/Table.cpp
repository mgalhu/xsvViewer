#include "Table.h"

#include <algorithm>
#include <map>

#include "Scanner.h"

namespace xsv {

namespace {

bool isBlankRecord(const char* p, const char* end) {
    return p < end && (*p == '\n' || *p == '\r');
}

bool isSpace(char c) { return c == ' ' || c == '\t'; }

}  // namespace

void Table::load(std::string text, Dialect dialect) {
    text_ = std::move(text);
    dialect_ = std::move(dialect);
    if (dialect_.delimiter.empty()) dialect_.delimiter = ",";
    rows_.clear();
    names_.clear();
    raggedRows_ = 0;
    unbalancedQuotes_ = 0;

    const char* begin = text_.data();
    const char* end = begin + text_.size();
    const char* p = begin;
    if (startsWith(p, end, "\xEF\xBB\xBF")) p += 3;
    for (size_t i = 0; i < dialect_.skipLines && p < end; ++i) p = skipLine(p, end);

    std::vector<std::string> header;
    bool headerPending = dialect_.hasHeader;
    size_t widest = 0;

    // Almost every row has the same width, so runs are counted instead of each row.
    std::map<size_t, size_t> widths;
    size_t runWidth = 0, runLength = 0;

    while (p < end) {
        if (startsWith(p, end, dialect_.comment)) {
            p = skipLine(p, end);
            continue;
        }
        if (dialect_.skipBlankLines && isBlankRecord(p, end)) {
            p = skipLine(p, end);
            continue;
        }
        if (headerPending) {
            headerPending = false;
            p = scanRecord(p, end, dialect_, unbalancedQuotes_, [&](const char* b, const char* e, bool, bool escaped) {
                std::string scratch;
                const Cell cell{static_cast<size_t>(b - begin), static_cast<size_t>(e - b), escaped};
                header.emplace_back(cellText(cell, scratch));
            });
            continue;
        }

        const size_t offset = static_cast<size_t>(p - begin);
        size_t fields = 0;
        p = scanRecord(p, end, dialect_, unbalancedQuotes_, [&](const char*, const char*, bool, bool) { ++fields; });
        rows_.push_back(offset);
        widest = std::max(widest, fields);
        if (fields == runWidth) {
            ++runLength;
        } else {
            if (runLength) widths[runWidth] += runLength;
            runWidth = fields;
            runLength = 1;
        }
    }
    if (runLength) widths[runWidth] += runLength;

    size_t usualWidth = 0, usualCount = 0;
    for (const auto& [width, count] : widths) {
        if (count > usualCount) {
            usualWidth = width;
            usualCount = count;
        }
    }
    raggedRows_ = rows_.size() - usualCount;

    if (dialect_.rowNamesHeader && !header.empty() && header.size() + 1 == usualWidth)
        header.insert(header.begin(), "row.names");

    names_.resize(std::max(widest, header.size()));
    for (size_t i = 0; i < names_.size(); ++i) {
        if (i < header.size() && !header[i].empty())
            names_[i] = std::move(header[i]);
        else
            names_[i] = "Column " + std::to_string(i + 1);
    }
}

void Table::readRow(size_t row, std::vector<Cell>& cells) const {
    cells.clear();
    const char* begin = text_.data();
    const char* end = begin + text_.size();
    const bool trim = dialect_.trimCells;
    size_t unbalanced = 0;
    scanRecord(begin + rows_[row], end, dialect_, unbalanced,
               [&](const char* b, const char* e, bool quoted, bool escaped) {
                   if (trim && !quoted) {
                       while (b < e && isSpace(*b)) ++b;
                       while (e > b && isSpace(e[-1])) --e;
                   }
                   cells.push_back({static_cast<size_t>(b - begin), static_cast<size_t>(e - b), escaped});
               });
}

std::string_view Table::cellText(const Cell& cell, std::string& scratch) const {
    const std::string_view raw(text_.data() + cell.offset, cell.length);
    if (!cell.escaped) return raw;

    scratch.clear();
    scratch.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (dialect_.escape && c == dialect_.escape && i + 1 < raw.size()) {
            scratch.push_back(raw[++i]);
        } else if (dialect_.doubleQuote && c == dialect_.quote && i + 1 < raw.size() && raw[i + 1] == c) {
            scratch.push_back(c);
            ++i;
        } else {
            scratch.push_back(c);
        }
    }
    return scratch;
}

bool Table::locate(size_t offset, size_t& row, size_t& column) const {
    if (rows_.empty() || offset < rows_.front()) return false;
    const size_t candidate = static_cast<size_t>(std::upper_bound(rows_.begin(), rows_.end(), offset) - rows_.begin()) - 1;

    const char* begin = text_.data();
    const char* end = begin + text_.size();
    std::vector<Cell> cells;
    readRow(candidate, cells);
    size_t unbalanced = 0;
    const char* next = scanRecord(begin + rows_[candidate], end, dialect_, unbalanced,
                                  [](const char*, const char*, bool, bool) {});
    const size_t recordEnd = static_cast<size_t>(next - begin);
    const bool endsAtEofWithoutNewline =
        recordEnd == text_.size() && !text_.empty() && text_.back() != '\n' && text_.back() != '\r';
    if (offset >= recordEnd && !endsAtEofWithoutNewline) return false;  // a comment or blank line after the row

    row = candidate;
    column = cells.empty() ? 0 : cells.size() - 1;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (offset <= cells[i].offset + cells[i].length) {
            column = i;
            break;
        }
    }
    return true;
}

}  // namespace xsv
