// What is shown of a table: which rows, in which order, and what the data looks like.
#pragma once
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "Filter.h"
#include "Table.h"
#include "Text.h"

namespace xsv {

struct SortKey {
    size_t column = 0;
    bool descending = false;
};

struct ViewSpec {
    std::vector<std::pair<size_t, Filter>> columnFilters;
    Filter globalFilter;
    std::vector<SortKey> sort;
    bool naturalSort = true;
    const MissingSet* missing = nullptr;  // required when sort is not empty
};

// Source row indices in display order. Numbers sort before text, and missing cells stay last in
// either direction. The sort is stable, so ties keep file order.
std::vector<size_t> buildView(const Table& table, const ViewSpec& spec);

struct ColumnStats {
    size_t numbers = 0;
    size_t texts = 0;
    size_t missing = 0;
    double min = std::numeric_limits<double>::infinity();   // finite values only
    double max = -std::numeric_limits<double>::infinity();
    bool numeric() const { return numbers > 0 && texts * 20 <= numbers; }  // tolerates 5% stray text
    bool hasRange() const { return min <= max; }
};

// maxRows == 0 scans the whole table.
std::vector<ColumnStats> computeStats(const Table& table, const MissingSet& missing, size_t maxRows);

struct SelectionStats {
    size_t cells = 0;
    size_t numbers = 0;
    double sum = 0;
    double min = std::numeric_limits<double>::infinity();
    double max = -std::numeric_limits<double>::infinity();
    double mean() const { return numbers ? sum / static_cast<double>(numbers) : 0; }
};

// Rows are the positions [firstRow, lastRow] of view (or of the table itself if view is null).
SelectionStats computeSelectionStats(const Table& table, const std::vector<size_t>* view, size_t firstRow,
                                     size_t lastRow, const std::vector<size_t>& columns,
                                     const MissingSet& missing);

}  // namespace xsv
