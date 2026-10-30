#include "Export.h"

namespace xsv {

namespace {

void appendDelimited(std::string& out, std::string_view cell, char delimiter) {
    const bool needsQuotes = cell.find(delimiter) != std::string_view::npos ||
                             cell.find_first_of("\r\n") != std::string_view::npos ||
                             (delimiter == ',' ? cell.find('"') != std::string_view::npos
                                               : (!cell.empty() && cell.front() == '"'));
    if (!needsQuotes) {
        out.append(cell);
        return;
    }
    out.push_back('"');
    for (const char c : cell) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
}

void appendMarkdown(std::string& out, std::string_view cell) {
    for (size_t i = 0; i < cell.size(); ++i) {
        const char c = cell[i];
        if (c == '|') {
            out += "\\|";
        } else if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < cell.size() && cell[i + 1] == '\n') ++i;
            out += "<br>";
        } else {
            out.push_back(c);
        }
    }
}

class Writer {
public:
    Writer(std::string& out, ExportFormat format) : out_(out), format_(format) {}

    void beginRow() {
        first_ = true;
        if (format_ == ExportFormat::Markdown) out_ += "|";
    }

    void cell(std::string_view text) {
        switch (format_) {
            case ExportFormat::Csv:
                if (!first_) out_.push_back(',');
                appendDelimited(out_, text, ',');
                break;
            case ExportFormat::Tsv:
                if (!first_) out_.push_back('\t');
                appendDelimited(out_, text, '\t');
                break;
            case ExportFormat::Markdown:
                out_.push_back(' ');
                appendMarkdown(out_, text);
                out_ += " |";
                break;
        }
        first_ = false;
    }

    void endRow() { out_ += "\r\n"; }

private:
    std::string& out_;
    ExportFormat format_;
    bool first_ = true;
};

}  // namespace

std::string exportTable(const Table& table, const std::vector<size_t>* view, size_t firstRow, size_t lastRow,
                        const std::vector<size_t>& columns, ExportFormat format, bool includeHeader) {
    std::string out;
    Writer writer(out, format);

    if (includeHeader) {
        writer.beginRow();
        for (const size_t column : columns) writer.cell(table.columnNames()[column]);
        writer.endRow();
        if (format == ExportFormat::Markdown) {
            writer.beginRow();
            for (size_t i = 0; i < columns.size(); ++i) out += " --- |";
            writer.endRow();
        }
    }

    const size_t count = view ? view->size() : table.rowCount();
    std::vector<Cell> cells;
    std::string scratch;
    for (size_t position = firstRow; position <= lastRow && position < count; ++position) {
        table.readRow(view ? (*view)[position] : position, cells);
        writer.beginRow();
        for (const size_t column : columns)
            writer.cell(column < cells.size() ? table.cellText(cells[column], scratch) : std::string_view{});
        writer.endRow();
    }
    return out;
}

}  // namespace xsv
