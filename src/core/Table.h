// A delimited document, indexed but not split: only row offsets are kept in memory, and rows
// are parsed when they are needed. That is what keeps multi-million-row files cheap.
#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "Dialect.h"

namespace xsv {

struct Cell {
    size_t offset = 0;     // byte offset of the first content byte in the source text
    size_t length = 0;     // content bytes, outer quotes excluded
    bool escaped = false;  // content holds escapes that cellText() has to collapse
};

class Table {
public:
    void load(std::string text, Dialect dialect);

    const std::string& text() const { return text_; }
    const Dialect& dialect() const { return dialect_; }

    size_t rowCount() const { return rows_.size(); }  // data rows, header excluded
    size_t columnCount() const { return names_.size(); }
    size_t raggedRows() const { return raggedRows_; }  // rows whose field count differs from the table's
    size_t unbalancedQuotes() const { return unbalancedQuotes_; }
    const std::vector<std::string>& columnNames() const { return names_; }

    // Fills cells with the row's fields. A short row yields fewer cells than columnCount().
    void readRow(size_t row, std::vector<Cell>& cells) const;

    // Content of a cell. Uses scratch only when the cell has escapes to collapse.
    std::string_view cellText(const Cell& cell, std::string& scratch) const;

    size_t rowOffset(size_t row) const { return rows_[row]; }

    // Finds the cell holding a byte offset of the source text. False if it is in no data row.
    bool locate(size_t offset, size_t& row, size_t& column) const;

private:
    std::string text_;
    Dialect dialect_;
    std::vector<size_t> rows_;
    std::vector<std::string> names_;
    size_t raggedRows_ = 0;
    size_t unbalancedQuotes_ = 0;
};

}  // namespace xsv
