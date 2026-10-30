#include "Document.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <numeric>

#include "Plugin.h"
#include "Scintilla.h"

using namespace xsv;

namespace {

constexpr size_t kTypeSampleRows = 5000;

// Decimals shorten fractions; whole numbers (counts, years, IDs) stay as they are.
bool hasFraction(std::string_view text, bool decimalComma) {
    return text.find_first_of(decimalComma ? ",eE" : ".eE") != std::string_view::npos;
}

}  // namespace

void Document::setPath(std::wstring path) { path_ = std::move(path); }

std::string Document::extension() const {
    const size_t dot = path_.rfind(L'.');
    const size_t slash = path_.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    std::string out;
    for (size_t i = dot + 1; i < path_.size(); ++i) {
        const wint_t c = std::towlower(path_[i]);
        if (c >= 128) return {};
        out.push_back(static_cast<char>(c));
    }
    return out;
}

bool Document::readFrom(HWND scintilla) {
    const size_t length = static_cast<size_t>(SendMessageW(scintilla, SCI_GETLENGTH, 0, 0));
    const char* text = reinterpret_cast<const char*>(SendMessageW(scintilla, SCI_GETCHARACTERPOINTER, 0, 0));
    if (!text) text = "";
    const UINT reported = static_cast<UINT>(SendMessageW(scintilla, SCI_GETCODEPAGE, 0, 0));
    const UINT codePage = reported ? reported : CP_ACP;

    const std::string& current = table_.text();
    if (loaded_ && length == current.size() && std::memcmp(text, current.data(), length) == 0) {
        if (codePage == codePage_) return false;
        // Same bytes, read differently: Encoding > Encode in ... changes nothing else.
        codePage_ = codePage;
        ++contentVersion_;
        notify(true);
        return true;
    }

    codePage_ = codePage;
    parseText(std::string(text, length));
    return true;
}

std::string Document::encode(std::wstring_view text) const {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(codePage_, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(codePage_, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring Document::decode(std::string_view text) const {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(codePage_, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codePage_, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

const ParseOptions& Document::parseOptions() const {
    return layout_.hasParse ? layout_.parse : plugin::options().parse;
}

void Document::setParseOptions(const ParseOptions& options) {
    layout_.parse = options;
    layout_.hasParse = true;
    touchLayout();
    parseText(table_.text());
}

void Document::parseText(std::string text) {
    Dialect dialect = detectDialect(text, parseOptions(), extension());
    missing_ = MissingSet(plugin::options().missingTokens);
    table_.load(std::move(text), std::move(dialect));
    loaded_ = true;
    haveFullStats_ = false;
    ++contentVersion_;
    sampleStats_ = computeStats(table_, missing_, kTypeSampleRows);
    normalizeColumns();
    rebuildView();
    notify(true);
}

void Document::refreshForOptions() {
    if (!loaded_) return;
    if (!layout_.hasParse) {
        parseText(table_.text());
        return;
    }
    missing_ = MissingSet(plugin::options().missingTokens);
    sampleStats_ = computeStats(table_, missing_, kTypeSampleRows);
    haveFullStats_ = false;
    ++contentVersion_;
    rebuildView();
    notify(true);
}

void Document::resetLayout() {
    layout_ = DocLayout{};
    sort_.clear();
    columnFilters_.clear();
    globalFilter_.clear();
    touchLayout();
    parseText(table_.text());
}

void Document::normalizeColumns() {
    const size_t count = table_.columnCount();
    layout_.columns.resize(count);

    std::vector<int> order;
    order.reserve(count);
    std::vector<char> seen(count, 0);
    for (const int column : layout_.order) {
        if (column >= 0 && static_cast<size_t>(column) < count && !seen[static_cast<size_t>(column)]) {
            seen[static_cast<size_t>(column)] = 1;
            order.push_back(column);
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (!seen[i]) order.push_back(static_cast<int>(i));
    }
    layout_.order = std::move(order);

    columnFilters_.resize(count);
    columnFilterValid_.assign(count, 1);
    std::erase_if(sort_, [&](const SortKey& key) { return key.column >= count; });
    rebuildVisible();
}

void Document::rebuildVisible() {
    visible_.clear();
    visible_.reserve(layout_.order.size());
    for (const int column : layout_.order) {
        if (!layout_.columns[static_cast<size_t>(column)].hidden) visible_.push_back(static_cast<size_t>(column));
    }
    layout_.frozenColumns = std::min(layout_.frozenColumns, static_cast<int>(visible_.size()));
    clampSelection();
}

void Document::rebuildView() {
    const Options& options = plugin::options();
    const bool decimalComma = table_.dialect().decimalComma;

    ViewSpec spec;
    columnFilterValid_.assign(table_.columnCount(), 1);
    for (size_t column = 0; column < columnFilters_.size(); ++column) {
        if (columnFilters_[column].empty()) continue;
        Filter filter = Filter::parse(columnFilters_[column], options.caseSensitiveFilter, decimalComma);
        columnFilterValid_[column] = filter.valid();
        spec.columnFilters.emplace_back(column, std::move(filter));
    }
    spec.globalFilter = Filter::parse(globalFilter_, options.caseSensitiveFilter, decimalComma);
    globalValid_ = spec.globalFilter.valid();
    spec.sort = sort_;
    spec.naturalSort = options.naturalSort;
    spec.missing = &missing_;

    // The cursor stays on the same row of data while the view changes around it.
    const bool hadRows = viewRows() > 0 && selection.cursorRow < viewRows();
    const size_t cursorSource = hadRows ? sourceRow(selection.cursorRow) : 0;

    ++viewVersion_;
    identity_ = spec.columnFilters.empty() && spec.globalFilter.empty() && sort_.empty();
    if (identity_)
        view_.clear();
    else
        view_ = buildView(table_, spec);

    if (hadRows) {
        if (identity_) {
            selection.cursorRow = std::min(cursorSource, table_.rowCount() ? table_.rowCount() - 1 : 0);
        } else {
            auto found = std::find(view_.begin(), view_.end(), cursorSource);
            selection.cursorRow = found == view_.end() ? 0 : static_cast<size_t>(found - view_.begin());
        }
        selection.anchorRow = selection.cursorRow;
        selection.wholeRows = false;
    }
    clampSelection();
}

void Document::clampSelection() {
    const size_t rows = viewRows();
    const size_t columns = visible_.size();
    const auto clampTo = [](size_t& value, size_t count) { value = count ? std::min(value, count - 1) : 0; };
    clampTo(selection.anchorRow, rows);
    clampTo(selection.cursorRow, rows);
    clampTo(selection.anchorColumn, columns);
    clampTo(selection.cursorColumn, columns);
    clampTo(scrollRow, rows);
}

void Document::touchLayout() { layoutDirty_ = true; }

bool Document::revealRow(size_t source) {
    size_t position = source;
    if (!identity_) {
        const auto found = std::find(view_.begin(), view_.end(), source);
        if (found == view_.end()) return false;
        position = static_cast<size_t>(found - view_.begin());
    }
    if (position >= viewRows()) return false;
    selection.cursorRow = selection.anchorRow = position;
    selection.wholeRows = selection.wholeColumns = false;
    return true;
}

void Document::setFrozen(size_t columns) {
    layout_.frozenColumns = static_cast<int>(std::min(columns, visible_.size()));
    touchLayout();
    notify(true);
}

void Document::setWidth(size_t fileColumn, int width) {
    if (fileColumn >= layout_.columns.size()) return;  // the table changed during a drag or a dialog
    layout_.columns[fileColumn].width = width;
    touchLayout();
    notify(true);
}

void Document::fitAllColumns() {
    for (ColumnLayout& column : layout_.columns) column.width = 0;
    touchLayout();
    notify(true);
}

void Document::setAlign(size_t fileColumn, Align align) {
    if (fileColumn >= layout_.columns.size()) return;
    layout_.columns[fileColumn].align = align;
    touchLayout();
    notify(true);
}

void Document::setDecimals(size_t fileColumn, int decimals) {
    if (fileColumn >= layout_.columns.size()) return;
    layout_.columns[fileColumn].decimals = decimals;
    ++contentVersion_;
    touchLayout();
    notify(true);
}

void Document::setHidden(size_t fileColumn, bool hidden) {
    if (fileColumn >= layout_.columns.size()) return;
    layout_.columns[fileColumn].hidden = hidden;
    touchLayout();
    rebuildVisible();
    notify(true);
}

void Document::setAllHidden(bool hidden) {
    for (ColumnLayout& column : layout_.columns) column.hidden = hidden;
    touchLayout();
    rebuildVisible();
    notify(true);
}

void Document::moveColumn(size_t fromPosition, size_t toPosition) {
    if (fromPosition >= visible_.size() || fromPosition == toPosition) return;
    const int moved = static_cast<int>(visible_[fromPosition]);
    const int before = toPosition < visible_.size() ? static_cast<int>(visible_[toPosition]) : -1;

    auto& order = layout_.order;
    order.erase(std::find(order.begin(), order.end(), moved));
    const auto target = before < 0 ? order.end() : std::find(order.begin(), order.end(), before);
    order.insert(target, moved);

    const size_t keepCursor = visible_[selection.cursorColumn];
    touchLayout();
    rebuildVisible();
    selection.cursorColumn = selection.anchorColumn =
        static_cast<size_t>(std::find(visible_.begin(), visible_.end(), keepCursor) - visible_.begin());
    notify(true);
}

void Document::moveInOrder(size_t fromIndex, size_t toIndex) {
    auto& order = layout_.order;
    if (fromIndex >= order.size() || toIndex >= order.size()) return;
    std::swap(order[fromIndex], order[toIndex]);
    touchLayout();
    rebuildVisible();
    notify(true);
}

void Document::requestRevealColumn(size_t fileColumn) {
    revealColumn_ = fileColumn;
    notify(false);
}

bool Document::takeRevealColumn(size_t& position) {
    if (revealColumn_ == SIZE_MAX) return false;
    const auto found = std::find(visible_.begin(), visible_.end(), revealColumn_);
    revealColumn_ = SIZE_MAX;
    if (found == visible_.end()) return false;
    position = static_cast<size_t>(found - visible_.begin());
    return true;
}

Align Document::alignOf(size_t fileColumn) const {
    const Align align = layout_.columns[fileColumn].align;
    if (align != Align::Auto) return align;
    return plugin::options().rightAlignNumbers && isNumeric(fileColumn) ? Align::Right : Align::Left;
}

bool Document::isNumeric(size_t fileColumn) const {
    return fileColumn < sampleStats_.size() && sampleStats_[fileColumn].numeric();
}

void Document::toggleSort(size_t fileColumn, bool additive) {
    const auto found =
        std::find_if(sort_.begin(), sort_.end(), [&](const SortKey& k) { return k.column == fileColumn; });
    if (found == sort_.end()) {
        if (!additive) sort_.clear();
        sort_.push_back({fileColumn, false});
    } else if (!found->descending) {
        found->descending = true;
        if (!additive) {
            const SortKey key = *found;
            sort_.assign(1, key);
        }
    } else if (additive) {
        sort_.erase(found);
    } else {
        sort_.clear();
    }
    rebuildView();
    notify(true);
}

void Document::setSort(size_t fileColumn, bool descending) {
    sort_.assign(1, SortKey{fileColumn, descending});
    rebuildView();
    notify(true);
}

void Document::clearSort() {
    sort_.clear();
    rebuildView();
    notify(true);
}

const std::string& Document::columnFilter(size_t fileColumn) const {
    static const std::string none;
    return fileColumn < columnFilters_.size() ? columnFilters_[fileColumn] : none;
}

bool Document::columnFilterValid(size_t fileColumn) const {
    return fileColumn >= columnFilterValid_.size() || columnFilterValid_[fileColumn];
}

void Document::setColumnFilter(size_t fileColumn, const std::string& filter) {
    if (fileColumn >= columnFilters_.size() || columnFilters_[fileColumn] == filter) return;
    columnFilters_[fileColumn] = filter;
    rebuildView();
    notify(true);
}

void Document::setGlobalFilter(const std::string& filter) {
    if (globalFilter_ == filter) return;
    globalFilter_ = filter;
    rebuildView();
    notify(true);
}

void Document::clearFilters() {
    for (std::string& filter : columnFilters_) filter.clear();
    globalFilter_.clear();
    rebuildView();
    notify(true);
}

bool Document::filtered() const {
    return !globalFilter_.empty() ||
           std::any_of(columnFilters_.begin(), columnFilters_.end(), [](const std::string& f) { return !f.empty(); });
}

CellText Document::present(size_t fileColumn, std::string_view raw, std::string& scratch) const {
    CellText cell;
    cell.text = raw;
    if (missing_.contains(raw)) {
        cell.missing = true;
        return cell;
    }
    if (!isNumeric(fileColumn)) return cell;

    const bool decimalComma = table_.dialect().decimalComma;
    cell.numeric = parseNumber(raw, decimalComma, cell.value);
    if (!cell.numeric) return cell;

    const Options& options = plugin::options();
    const int own = layout_.columns[fileColumn].decimals;
    const int decimals = own == kDecimalsInherit ? options.decimals : own;
    const bool shorten = decimals >= 0 && hasFraction(raw, decimalComma);
    if (!shorten && !options.thousands) return cell;

    if (!hasFraction(raw, decimalComma)) {
        scratch = groupDigits(raw, decimalComma);
        cell.text = scratch;
        return cell;
    }

    NumberFormat format;
    format.decimals = shorten ? decimals : kDecimalsRaw;
    format.thousands = options.thousands;
    format.decimalComma = decimalComma;
    scratch = formatNumber(cell.value, format);
    cell.text = scratch;
    return cell;
}

void Document::ensureFullStats() {
    if (haveFullStats_) return;
    fullStats_ = computeStats(table_, missing_, 0);
    haveFullStats_ = true;
    tableMin_ = std::numeric_limits<double>::infinity();
    tableMax_ = -std::numeric_limits<double>::infinity();
    for (const ColumnStats& stats : fullStats_) {
        if (!stats.numeric() || !stats.hasRange()) continue;
        tableMin_ = std::min(tableMin_, stats.min);
        tableMax_ = std::max(tableMax_, stats.max);
    }
}

bool Document::scalePosition(size_t fileColumn, double value, double& position) {
    const ColorScale scale = plugin::options().colorScale;
    if (scale == ColorScale::Off || !isNumeric(fileColumn) || !std::isfinite(value)) return false;
    ensureFullStats();
    const double low = scale == ColorScale::PerColumn ? fullStats_[fileColumn].min : tableMin_;
    const double high = scale == ColorScale::PerColumn ? fullStats_[fileColumn].max : tableMax_;
    if (!(low < high)) return false;
    if (low < 0 && high > 0) {
        // Signed data (log fold changes, z-scores): the middle colour belongs at zero.
        position = value < 0 ? 0.5 * (1.0 - value / low) : 0.5 * (1.0 + value / high);
    } else {
        position = (value - low) / (high - low);
    }
    position = std::clamp(position, 0.0, 1.0);
    return true;
}

void Document::addObserver(DocumentObserver* observer) {
    if (std::find(observers_.begin(), observers_.end(), observer) == observers_.end()) observers_.push_back(observer);
}

void Document::removeObserver(DocumentObserver* observer) { std::erase(observers_, observer); }

void Document::notify(bool structure) {
    for (DocumentObserver* observer : observers_) observer->documentChanged(structure);
}
