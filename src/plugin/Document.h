// One open file seen as a table: its parsed text, its layout, and what is currently shown.
#pragma once
#include <windows.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Options.h"
#include "Table.h"
#include "View.h"

struct Selection {
    size_t anchorRow = 0, cursorRow = 0;        // positions in the view
    size_t anchorColumn = 0, cursorColumn = 0;  // positions among the visible columns
    bool wholeRows = false;
    bool wholeColumns = false;
    bool operator==(const Selection&) const = default;
};

// A cell the way it is drawn: number formatting applied, kind known.
struct CellText {
    std::string_view text;
    bool missing = false;
    bool numeric = false;
    double value = 0;
};

class DocumentObserver {
public:
    virtual ~DocumentObserver() = default;
    // structure == true: rows, columns or sizes changed. false: only the selection did.
    virtual void documentChanged(bool structure) = 0;
};

class Document {
public:
    explicit Document(UINT_PTR buffer) : buffer_(buffer) {}

    UINT_PTR buffer() const { return buffer_; }
    const std::wstring& path() const { return path_; }
    void setPath(std::wstring path);
    std::string extension() const;

    // Reads the text of a Scintilla view showing this document. Unchanged text keeps everything.
    bool readFrom(HWND scintilla);
    bool loaded() const { return loaded_; }

    const xsv::Table& table() const { return table_; }
    UINT codePage() const { return codePage_; }

    // Between what the UI shows and the bytes of this document, which need not be UTF-8.
    std::string encode(std::wstring_view text) const;
    std::wstring decode(std::string_view text) const;

    // Bumped when cell contents or their formatting change, so measured widths can be reused.
    size_t contentVersion() const { return contentVersion_; }
    // Bumped when rows or their order change.
    size_t viewVersion() const { return viewVersion_; }

    const xsv::ParseOptions& parseOptions() const;
    void setParseOptions(const xsv::ParseOptions& options);
    void refreshForOptions();   // global options changed

    xsv::DocLayout& layout() { return layout_; }
    void resetLayout();
    bool layoutDirty() const { return layoutDirty_; }
    void markLayoutSaved() { layoutDirty_ = false; }

    // Rows
    size_t viewRows() const { return identity_ ? table_.rowCount() : view_.size(); }
    size_t sourceRow(size_t position) const { return identity_ ? position : view_[position]; }
    const std::vector<size_t>* viewOrNull() const { return identity_ ? nullptr : &view_; }
    bool revealRow(size_t sourceRow);  // moves the cursor to a source row; false if it is filtered out

    // Columns
    size_t visibleColumns() const { return visible_.size(); }
    size_t fileColumn(size_t position) const { return visible_[position]; }
    const std::vector<size_t>& visibleList() const { return visible_; }
    size_t frozenColumns() const { return static_cast<size_t>(layout_.frozenColumns); }
    void setFrozen(size_t columns);
    void setWidth(size_t fileColumn, int width);
    void fitAllColumns();
    void setAlign(size_t fileColumn, xsv::Align align);
    void setDecimals(size_t fileColumn, int decimals);
    void setHidden(size_t fileColumn, bool hidden);
    void setAllHidden(bool hidden);
    void moveColumn(size_t fromPosition, size_t toPosition);  // among visible columns
    void moveInOrder(size_t fromIndex, size_t toIndex);       // among all columns
    void requestRevealColumn(size_t fileColumn);
    bool takeRevealColumn(size_t& position);

    xsv::Align alignOf(size_t fileColumn) const;
    bool isNumeric(size_t fileColumn) const;

    // Sorting and filtering
    const std::vector<xsv::SortKey>& sort() const { return sort_; }
    void toggleSort(size_t fileColumn, bool additive);
    void setSort(size_t fileColumn, bool descending);
    void clearSort();
    const std::string& columnFilter(size_t fileColumn) const;
    bool columnFilterValid(size_t fileColumn) const;
    void setColumnFilter(size_t fileColumn, const std::string& filter);
    const std::string& globalFilter() const { return globalFilter_; }
    bool globalFilterValid() const { return globalValid_; }
    void setGlobalFilter(const std::string& filter);
    void clearFilters();
    bool filtered() const;

    // Display
    CellText present(size_t fileColumn, std::string_view raw, std::string& scratch) const;
    const xsv::MissingSet& missing() const { return missing_; }
    // Position of a value on the colour scale in [0, 1]; false if the cell gets no colour.
    bool scalePosition(size_t fileColumn, double value, double& position);

    // Live view state, shared by every window showing this document
    Selection selection;
    size_t scrollRow = 0;
    int scrollX = 0;

    bool tableMode = false;
    bool readOnlyApplied = false;
    bool wasReadOnly = false;

    void addObserver(DocumentObserver* observer);
    void removeObserver(DocumentObserver* observer);
    void notify(bool structure);

private:
    void parseText(std::string text);
    void normalizeColumns();
    void rebuildVisible();
    void rebuildView();
    void clampSelection();
    void touchLayout();
    void ensureFullStats();

    UINT_PTR buffer_;
    std::wstring path_;
    xsv::Table table_;
    UINT codePage_ = CP_UTF8;
    bool loaded_ = false;
    size_t contentVersion_ = 0;
    size_t viewVersion_ = 0;

    xsv::DocLayout layout_;
    bool layoutDirty_ = false;
    std::vector<size_t> visible_;
    size_t revealColumn_ = SIZE_MAX;

    bool identity_ = true;
    std::vector<size_t> view_;
    std::vector<xsv::SortKey> sort_;
    std::vector<std::string> columnFilters_;
    std::vector<char> columnFilterValid_;
    std::string globalFilter_;
    bool globalValid_ = true;

    xsv::MissingSet missing_;
    std::vector<xsv::ColumnStats> sampleStats_;
    std::vector<xsv::ColumnStats> fullStats_;
    bool haveFullStats_ = false;
    double tableMin_ = 0, tableMax_ = 0;

    std::vector<DocumentObserver*> observers_;
};
