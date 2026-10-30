#include "View.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <numeric>
#include <string>

namespace xsv {

namespace {

enum class Kind : unsigned char { Number, Text, Missing };

struct SortCell {
    double number = 0;
    std::string_view text;
    Kind kind = Kind::Missing;
};

// One sort key extracted for every source row, so comparing never re-parses the document.
struct SortColumn {
    std::vector<SortCell> cells;
    bool descending = false;
};

SortColumn extractKey(const Table& table, const std::vector<size_t>& rows, const SortKey& key, const MissingSet& missing,
                      std::deque<std::string>& unescaped) {
    SortColumn column;
    column.descending = key.descending;
    column.cells.resize(table.rowCount());
    const bool decimalComma = table.dialect().decimalComma;

    std::vector<Cell> cells;
    std::string scratch;
    for (const size_t row : rows) {
        table.readRow(row, cells);
        if (key.column >= cells.size()) continue;
        std::string_view text = table.cellText(cells[key.column], scratch);
        if (cells[key.column].escaped) text = unescaped.emplace_back(text);
        SortCell& out = column.cells[row];
        if (missing.contains(text)) continue;
        out.text = text;
        out.kind = parseNumber(text, decimalComma, out.number) ? Kind::Number : Kind::Text;
    }
    return column;
}

int compareCells(const SortCell& a, const SortCell& b, bool descending, bool natural) {
    if (a.kind == Kind::Missing || b.kind == Kind::Missing) {
        // Direction does not apply: gaps stay at the bottom either way.
        return static_cast<int>(a.kind == Kind::Missing) - static_cast<int>(b.kind == Kind::Missing);
    }
    int order;
    if (a.kind != b.kind) {
        order = a.kind == Kind::Number ? -1 : 1;
    } else if (a.kind == Kind::Number) {
        order = a.number < b.number ? -1 : (a.number > b.number ? 1 : 0);
    } else {
        order = compareText(a.text, b.text, natural);
    }
    return descending ? -order : order;
}

}  // namespace

std::vector<size_t> buildView(const Table& table, const ViewSpec& spec) {
    std::vector<const std::pair<size_t, Filter>*> filters;
    for (const auto& entry : spec.columnFilters) {
        if (!entry.second.empty() && entry.second.valid()) filters.push_back(&entry);
    }
    const bool useGlobal = !spec.globalFilter.empty() && spec.globalFilter.valid();

    std::vector<size_t> rows;
    if (filters.empty() && !useGlobal) {
        rows.resize(table.rowCount());
        std::iota(rows.begin(), rows.end(), size_t{0});
    } else {
        std::vector<Cell> cells;
        std::string scratch;
        for (size_t row = 0; row < table.rowCount(); ++row) {
            table.readRow(row, cells);
            bool keep = true;
            for (const auto* filter : filters) {
                std::string_view text;
                if (filter->first < cells.size()) text = table.cellText(cells[filter->first], scratch);
                if (!filter->second.matches(text)) {
                    keep = false;
                    break;
                }
            }
            if (keep && useGlobal) {
                keep = false;
                for (const Cell& cell : cells) {
                    if (spec.globalFilter.matches(table.cellText(cell, scratch))) {
                        keep = true;
                        break;
                    }
                }
            }
            if (keep) rows.push_back(row);
        }
    }

    if (!spec.sort.empty()) {
        static const MissingSet noTokens;
        const MissingSet& missing = spec.missing ? *spec.missing : noTokens;
        std::deque<std::string> unescaped;
        std::vector<SortColumn> keys;
        keys.reserve(spec.sort.size());
        for (const SortKey& key : spec.sort) keys.push_back(extractKey(table, rows, key, missing, unescaped));

        std::stable_sort(rows.begin(), rows.end(), [&](size_t a, size_t b) {
            for (const SortColumn& key : keys) {
                const int order = compareCells(key.cells[a], key.cells[b], key.descending, spec.naturalSort);
                if (order) return order < 0;
            }
            return false;
        });
    }
    return rows;
}

std::vector<ColumnStats> computeStats(const Table& table, const MissingSet& missing, size_t maxRows) {
    std::vector<ColumnStats> stats(table.columnCount());
    const size_t rows = maxRows ? std::min(maxRows, table.rowCount()) : table.rowCount();
    const bool decimalComma = table.dialect().decimalComma;

    std::vector<Cell> cells;
    std::string scratch;
    for (size_t row = 0; row < rows; ++row) {
        table.readRow(row, cells);
        for (size_t column = 0; column < stats.size(); ++column) {
            ColumnStats& s = stats[column];
            if (column >= cells.size()) {
                ++s.missing;
                continue;
            }
            const std::string_view text = table.cellText(cells[column], scratch);
            double value;
            if (missing.contains(text)) {
                ++s.missing;
            } else if (parseNumber(text, decimalComma, value)) {
                ++s.numbers;
                if (std::isfinite(value)) {
                    s.min = std::min(s.min, value);
                    s.max = std::max(s.max, value);
                }
            } else {
                ++s.texts;
            }
        }
    }
    return stats;
}

SelectionStats computeSelectionStats(const Table& table, const std::vector<size_t>* view, size_t firstRow,
                                     size_t lastRow, const std::vector<size_t>& columns, const MissingSet& missing) {
    SelectionStats stats;
    const size_t count = view ? view->size() : table.rowCount();
    const bool decimalComma = table.dialect().decimalComma;

    std::vector<Cell> cells;
    std::string scratch;
    for (size_t position = firstRow; position <= lastRow && position < count; ++position) {
        table.readRow(view ? (*view)[position] : position, cells);
        for (const size_t column : columns) {
            ++stats.cells;
            if (column >= cells.size()) continue;
            const std::string_view text = table.cellText(cells[column], scratch);
            double value;
            if (missing.contains(text) || !parseNumber(text, decimalComma, value) || !std::isfinite(value)) continue;
            ++stats.numbers;
            stats.sum += value;
            stats.min = std::min(stats.min, value);
            stats.max = std::max(stats.max, value);
        }
    }
    return stats;
}

}  // namespace xsv
