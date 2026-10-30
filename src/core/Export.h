// Writes a part of a table back out as text, for the clipboard and for exporting a view.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "Table.h"

namespace xsv {

enum class ExportFormat { Csv, Tsv, Markdown };

// Rows are the positions [firstRow, lastRow] of view (or of the table itself if view is null).
std::string exportTable(const Table& table, const std::vector<size_t>* view, size_t firstRow, size_t lastRow,
                        const std::vector<size_t>& columns, ExportFormat format, bool includeHeader);

}  // namespace xsv
