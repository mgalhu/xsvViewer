#include "Grid.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "Plugin.h"
#include "TableHost.h"

using namespace xsv;

namespace {

constexpr wchar_t kClassName[] = L"xsvViewerGrid";
constexpr UINT_PTR kTimerDragScroll = 1;
constexpr UINT_PTR kTimerFilter = 2;
constexpr UINT kDragScrollMs = 60;
constexpr UINT kFilterDelayMs = 250;
constexpr int kDragThreshold = 4;
constexpr int kResizeGrip = 4;
constexpr int kMaxCellChars = 1024;     // nobody reads more than this in one cell on screen
constexpr int kMaxTipChars = 4000;
constexpr size_t kMaxFitCells = 2000000;
constexpr int kDefaultManualWidth = 120;

enum Command : UINT {
    CmdSortAsc = 1, CmdSortDesc, CmdSortClear, CmdFilterColumn, CmdFitColumn, CmdFitAll, CmdSetWidth,
    CmdFreeze, CmdUnfreeze, CmdHide, CmdShowAll, CmdColumns, CmdSelectColumn,
    CmdCopy, CmdCopyHeader, CmdCopyCsv, CmdCopyMarkdown, CmdFilterValue, CmdExcludeValue, CmdClearFilters, CmdShowText,
    CmdAlignBase = 100,      // + Align
    CmdDecimalsBase = 200,   // + 2 + decimals: 200 inherit, 201 raw, 202 = 0 decimals ...
};

COLORREF mix(COLORREF a, COLORREF b, double t) {
    const auto channel = [&](int shift) {
        const double from = (a >> shift) & 0xFF, to = (b >> shift) & 0xFF;
        return static_cast<int>(std::lround(from + (to - from) * t));
    };
    return RGB(channel(0), channel(8), channel(16));
}

double luminance(COLORREF c) {
    return 0.299 * GetRValue(c) + 0.587 * GetGValue(c) + 0.114 * GetBValue(c);
}

void fill(HDC dc, const RECT& r, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void fill(HDC dc, int left, int top, int right, int bottom, COLORREF color) {
    const RECT r{left, top, right, bottom};
    fill(dc, r, color);
}

// Single-line display: line breaks and tabs must not disturb the row.
void flatten(wchar_t* text, int length) {
    for (int i = 0; i < length; ++i) {
        if (text[i] == L'\n') text[i] = L'\x21B5';
        else if (text[i] == L'\r' || text[i] == L'\t') text[i] = L' ';
    }
}

COLORREF scaleColor(const Options& o, double position) {
    return position < 0.5 ? mix(o.scaleLow, o.scaleMid, position * 2) : mix(o.scaleMid, o.scaleHigh, (position - 0.5) * 2);
}

std::wstring editText(HWND edit) {
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
    const int length = GetWindowTextW(edit, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<size_t>(length));
    return text;
}

}  // namespace

void Grid::registerClass() {
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = plugin::instance();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

HWND Grid::create(HWND parent, TableHost* host) {
    host_ = host;
    CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_CLIPCHILDREN | WS_TABSTOP, 0, 0,
                    0, 0, parent, nullptr, plugin::instance(), this);
    return hwnd_;
}

LRESULT CALLBACK Grid::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    Grid* self;
    if (message == WM_NCCREATE) {
        self = static_cast<Grid*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Grid*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Grid::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            dpi_ = static_cast<int>(GetDpiForWindow(hwnd_));
            SetPropW(hwnd_, L"xsvViewerGrid", this);
            rebuildFonts();
            tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
                                   hwnd_, nullptr, plugin::instance(), nullptr);
            TOOLINFOW info{sizeof info};
            info.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_IDISHWND;
            info.hwnd = hwnd_;
            info.uId = reinterpret_cast<UINT_PTR>(hwnd_);
            info.lpszText = const_cast<wchar_t*>(L"");
            SendMessageW(tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
            SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, MulDiv(640, dpi_, 96));
            return 0;
        }
        case WM_DESTROY:
            RemovePropW(hwnd_, L"xsvViewerGrid");
            if (bufferDc_) DeleteDC(bufferDc_);
            if (buffer_) DeleteObject(buffer_);
            if (font_) DeleteObject(font_);
            if (headerFont_) DeleteObject(headerFont_);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            paint(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            paint(reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_SIZE:
            updateScrollBars();
            placeFilterEdit();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED_AFTERPARENT:
            dpi_ = static_cast<int>(GetDpiForWindow(hwnd_));
            optionsChanged();
            return 0;
        case WM_SETFOCUS:
            plugin::activateViewOf(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_KILLFOCUS:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_COPY:  // what Notepad++ sends for its own Copy command
            copySelection(ExportFormat::Tsv, false);
            return 0;
        case WM_VSCROLL: {
            if (!doc_) return 0;
            SCROLLINFO info{sizeof info, SIF_ALL};
            GetScrollInfo(hwnd_, SB_VERT, &info);
            long long row = static_cast<long long>(doc_->scrollRow);
            switch (LOWORD(wParam)) {
                case SB_LINEUP: row -= 1; break;
                case SB_LINEDOWN: row += 1; break;
                case SB_PAGEUP: row -= info.nPage; break;
                case SB_PAGEDOWN: row += info.nPage; break;
                case SB_TOP: row = 0; break;
                case SB_BOTTOM: row = info.nMax; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: row = info.nTrackPos; break;
                default: return 0;
            }
            scrollTo(static_cast<size_t>(std::max(row, 0LL)), doc_->scrollX);
            return 0;
        }
        case WM_HSCROLL: {
            if (!doc_) return 0;
            SCROLLINFO info{sizeof info, SIF_ALL};
            GetScrollInfo(hwnd_, SB_HORZ, &info);
            int x = doc_->scrollX;
            switch (LOWORD(wParam)) {
                case SB_LINELEFT: x -= scale(40); break;
                case SB_LINERIGHT: x += scale(40); break;
                case SB_PAGELEFT: x -= static_cast<int>(info.nPage); break;
                case SB_PAGERIGHT: x += static_cast<int>(info.nPage); break;
                case SB_LEFT: x = 0; break;
                case SB_RIGHT: x = info.nMax; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: x = info.nTrackPos; break;
                default: return 0;
            }
            scrollTo(doc_->scrollRow, x);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (!doc_) return 0;
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const WORD keys = GET_KEYSTATE_WPARAM(wParam);
            if (keys & MK_CONTROL) {
                zoom(delta > 0 ? 1 : -1);
            } else if (keys & MK_SHIFT) {
                scrollTo(doc_->scrollRow, doc_->scrollX - MulDiv(delta, scale(120), WHEEL_DELTA));
            } else {
                UINT lines = 3;
                SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
                const long long step = lines == WHEEL_PAGESCROLL ? visibleRows() : static_cast<long long>(lines);
                const long long row = static_cast<long long>(doc_->scrollRow) - delta * step / WHEEL_DELTA;
                scrollTo(static_cast<size_t>(std::max(row, 0LL)), doc_->scrollX);
            }
            return 0;
        }
        case WM_MOUSEHWHEEL:
            if (doc_) scrollTo(doc_->scrollRow, doc_->scrollX + MulDiv(GET_WHEEL_DELTA_WPARAM(wParam), scale(120), WHEEL_DELTA));
            return 0;
        case WM_LBUTTONDOWN:
            onLeftDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam);
            return 0;
        case WM_LBUTTONUP:
            onLeftUp();
            return 0;
        case WM_LBUTTONDBLCLK:
            onLeftDouble(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_RBUTTONDOWN:
            SetFocus(hwnd_);
            return 0;
        case WM_RBUTTONUP:
            onContextMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEMOVE:
            onMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam);
            return 0;
        case WM_MOUSEHOVER:
            trackingMouse_ = false;
            onHover(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            hideTip();
            return 0;
        case WM_SETCURSOR: {
            if (LOWORD(lParam) != HTCLIENT) break;
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd_, &p);
            const bool resizing = drag_ == Drag::Resize || hitTest(p.x, p.y).zone == Zone::ResizeEdge;
            SetCursor(LoadCursor(nullptr, resizing ? IDC_SIZEWE : IDC_ARROW));
            return TRUE;
        }
        case WM_TIMER:
            if (wParam == kTimerDragScroll) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd_, &p);
                extendDragSelection(p.x, p.y);
            } else if (wParam == kTimerFilter) {
                KillTimer(hwnd_, kTimerFilter);
                if (filterEdit_ && doc_ && filterPosition_ < doc_->visibleColumns())
                    doc_->setColumnFilter(doc_->fileColumn(filterPosition_), doc_->encode(editText(filterEdit_)));
            }
            return 0;
        case WM_COMMAND:
            if (reinterpret_cast<HWND>(lParam) == filterEdit_ && HIWORD(wParam) == EN_CHANGE && filterEdit_)
                SetTimer(hwnd_, kTimerFilter, kFilterDelayMs, nullptr);
            return 0;
        case WM_CAPTURECHANGED:
            KillTimer(hwnd_, kTimerDragScroll);
            drag_ = Drag::None;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

// ---- Geometry --------------------------------------------------------------------------------

int Grid::scale(int value) const { return MulDiv(value, dpi_, 96); }

void Grid::rebuildFonts() {
    const Options& o = plugin::options();
    if (font_) DeleteObject(font_);
    if (headerFont_) DeleteObject(headerFont_);
    const std::wstring face = plugin::widen(o.fontFace);
    const int height = -MulDiv(o.fontSize, dpi_, 72);
    const auto make = [&](int weight) {
        return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face.c_str());
    };
    font_ = make(FW_NORMAL);
    headerFont_ = make(o.headerBold ? FW_SEMIBOLD : FW_NORMAL);

    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_);
    TEXTMETRICW metrics;
    GetTextMetricsW(dc, &metrics);
    textHeight_ = metrics.tmHeight;
    wideCharWidth_ = metrics.tmMaxCharWidth > 0 ? std::min<int>(metrics.tmMaxCharWidth, metrics.tmHeight) : metrics.tmHeight;
    GetCharWidth32W(dc, 0, 127, charWidths_);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
}

void Grid::measure() {
    const Options& o = plugin::options();
    cellPadding_ = scale(o.cellPadding);
    rowHeight_ = textHeight_ * o.linesPerRow + 2 * scale(o.rowPadding);
    headerHeight_ = textHeight_ + 2 * scale(o.rowPadding + 2);
    filterHeight_ = textHeight_ + 2 * scale(o.rowPadding);

    gutterWidth_ = 0;
    if (o.showRowNumbers && doc_) {
        const std::string digits = std::to_string(std::max<size_t>(doc_->table().rowCount(), 999));
        gutterWidth_ = static_cast<int>(digits.size()) * charWidths_['0'] + 2 * cellPadding_;
    }
}

// Exact for ASCII from the font's width table, close enough elsewhere, and cheap enough to run
// over thousands of cells without a GDI call each.
int Grid::measureWidth(std::string_view text) const {
    int width = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 128) width += charWidths_[c];
        else if ((c & 0xC0) != 0x80) width += wideCharWidth_;  // lead byte of a multi-byte character
    }
    return width;
}

void Grid::computeAutoWidths() {
    if (!doc_ || autoWidthsFor_ == doc_->contentVersion()) return;
    autoWidthsFor_ = doc_->contentVersion();

    const Table& table = doc_->table();
    const size_t columns = table.columnCount();
    const Options& o = plugin::options();
    autoWidths_.assign(columns, 0);
    if (!columns) return;

    const int padding = 2 * cellPadding_;
    const int sortRoom = scale(16);
    for (size_t c = 0; c < columns; ++c) {
        autoWidths_[c] = measureWidth(table.columnNames()[c]) * 9 / 8 + padding + sortRoom;  // header font is heavier
    }
    const size_t rows = std::min({static_cast<size_t>(o.autoFitRows), table.rowCount(), kMaxFitCells / columns});
    for (size_t r = 0; r < rows; ++r) {
        table.readRow(r, cells_);
        for (size_t c = 0; c < cells_.size(); ++c) {
            const CellText cell = doc_->present(c, table.cellText(cells_[c], scratch_), format_);
            autoWidths_[c] = std::max(autoWidths_[c], measureWidth(cell.text) + padding);
        }
    }
    const int low = o.minColumnWidth, high = o.maxAutoColumnWidth;
    for (int& width : autoWidths_) width = std::clamp(MulDiv(width, 96, dpi_), low, std::max(low, high));
}

int Grid::columnWidth(size_t fileColumn) const {
    const int manual = doc_->layout().columns[fileColumn].width;
    return scale(manual > 0 ? manual : autoWidths_[fileColumn]);
}

void Grid::rebuildColumnEdges() {
    const size_t count = doc_ ? doc_->visibleColumns() : 0;
    edges_.resize(count + 1);
    edges_[0] = 0;
    for (size_t i = 0; i < count; ++i) edges_[i + 1] = edges_[i] + columnWidth(doc_->fileColumn(i));
}

int Grid::bodyTop() const {
    return headerHeight_ + (plugin::options().showFilterRow ? filterHeight_ : 0);
}

int Grid::visibleRows() const {
    RECT client;
    GetClientRect(hwnd_, &client);
    return std::max(0, static_cast<int>(client.bottom) - bodyTop()) / std::max(rowHeight_, 1);
}

int Grid::viewportWidth() const {
    RECT client;
    GetClientRect(hwnd_, &client);
    const long long frozen = doc_ ? edges_[doc_->frozenColumns()] : 0;
    return static_cast<int>(std::max<long long>(0, client.right - gutterWidth_ - frozen));
}

void Grid::updateScrollBars() {
    if (!doc_) {
        ShowScrollBar(hwnd_, SB_BOTH, FALSE);
        return;
    }
    // Showing one scroll bar can make the other necessary, so settle over two passes.
    for (int pass = 0; pass < 2; ++pass) {
        SCROLLINFO info{sizeof info, SIF_ALL};
        info.nMax = static_cast<int>(doc_->viewRows()) - 1;
        info.nPage = static_cast<UINT>(std::max(visibleRows(), 1));
        info.nPos = static_cast<int>(doc_->scrollRow);
        SetScrollInfo(hwnd_, SB_VERT, &info, TRUE);

        const long long frozen = edges_[doc_->frozenColumns()];
        const long long total = edges_.back() - frozen;
        SCROLLINFO horizontal{sizeof horizontal, SIF_ALL};
        horizontal.nMax = static_cast<int>(std::min<long long>(total, INT_MAX)) - 1;
        horizontal.nPage = static_cast<UINT>(viewportWidth());
        horizontal.nPos = doc_->scrollX;
        SetScrollInfo(hwnd_, SB_HORZ, &horizontal, TRUE);
    }
    const long long maxRow = static_cast<long long>(doc_->viewRows()) - std::max(visibleRows(), 1);
    doc_->scrollRow = static_cast<size_t>(std::clamp<long long>(static_cast<long long>(doc_->scrollRow), 0, std::max(maxRow, 0LL)));
    const long long maxX = edges_.back() - edges_[doc_->frozenColumns()] - viewportWidth();
    doc_->scrollX = static_cast<int>(std::clamp<long long>(doc_->scrollX, 0, std::max(maxX, 0LL)));
}

int Grid::columnScreenX(size_t position) const {
    const long long x = edges_[position];
    const long long screen = gutterWidth_ + x - (position < doc_->frozenColumns() ? 0 : doc_->scrollX);
    return static_cast<int>(std::clamp<long long>(screen, INT_MIN / 2, INT_MAX / 2));
}

size_t Grid::positionAt(int x) const {
    const size_t count = doc_->visibleColumns();
    const size_t frozen = doc_->frozenColumns();
    long long content = x - gutterWidth_;
    if (content >= edges_[frozen]) content += doc_->scrollX;
    const auto found = std::upper_bound(edges_.begin(), edges_.end(), content);
    const size_t position = static_cast<size_t>(found - edges_.begin());
    return position == 0 || position > count ? SIZE_MAX : position - 1;
}

size_t Grid::rowAt(int y) const {
    const long long row = static_cast<long long>(doc_->scrollRow) + (y - bodyTop()) / rowHeight_;
    return static_cast<size_t>(std::clamp<long long>(row, 0, static_cast<long long>(doc_->viewRows()) - 1));
}

Grid::Hit Grid::hitTest(int x, int y) const {
    Hit hit;
    if (!doc_) return hit;
    const bool inGutter = x < gutterWidth_;
    if (y < headerHeight_) {
        if (inGutter) {
            hit.zone = Zone::Corner;
            return hit;
        }
        // The grip belongs to the column left of the line, and reaches a little into the next one.
        for (int offset : {kResizeGrip, -kResizeGrip}) {
            const size_t position = positionAt(x + scale(offset));
            if (position == SIZE_MAX) continue;
            const int right = columnScreenX(position) + columnWidth(doc_->fileColumn(position));
            if (std::abs(x - right) <= scale(kResizeGrip)) {
                hit.zone = Zone::ResizeEdge;
                hit.position = position;
                return hit;
            }
        }
        hit.position = positionAt(x);
        hit.zone = hit.position == SIZE_MAX ? Zone::None : Zone::Header;
        return hit;
    }
    if (plugin::options().showFilterRow && y < bodyTop()) {
        hit.position = inGutter ? SIZE_MAX : positionAt(x);
        hit.zone = hit.position == SIZE_MAX ? Zone::None : Zone::Filter;
        return hit;
    }
    if (doc_->viewRows() == 0 || y < bodyTop()) return hit;
    if (static_cast<size_t>((y - bodyTop()) / rowHeight_) + doc_->scrollRow >= doc_->viewRows()) return hit;
    hit.row = rowAt(y);
    if (inGutter) {
        hit.zone = Zone::Gutter;
        return hit;
    }
    hit.position = positionAt(x);
    hit.zone = hit.position == SIZE_MAX ? Zone::None : Zone::Cell;
    return hit;
}

// ---- Public state ----------------------------------------------------------------------------

void Grid::setDocument(Document* document) {
    commitFilterEdit(false);
    hideTip();
    doc_ = document;
    autoWidthsFor_ = SIZE_MAX;
    refresh(true);
}

void Grid::refresh(bool structure) {
    if (!hwnd_) return;
    if (structure) {
        measure();
        if (doc_) computeAutoWidths();
        rebuildColumnEdges();
        updateScrollBars();
        placeFilterEdit();
    }
    size_t reveal;
    if (doc_ && doc_->takeRevealColumn(reveal)) {
        doc_->selection.cursorColumn = doc_->selection.anchorColumn = reveal;
        ensureVisible(doc_->selection.cursorRow, reveal);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Grid::optionsChanged() {
    SetWindowTheme(hwnd_, plugin::darkMode() ? L"DarkMode_Explorer" : nullptr, nullptr);
    rebuildFonts();
    autoWidthsFor_ = SIZE_MAX;
    refresh(true);
}

void Grid::fitColumns() {
    if (doc_) doc_->fitAllColumns();
}

// ---- Painting --------------------------------------------------------------------------------

int Grid::toWide(std::string_view text, wchar_t* out, int capacity) const {
    if (text.empty()) return 0;
    const int bytes = static_cast<int>(std::min<size_t>(text.size(), static_cast<size_t>(capacity)));
    const int length = MultiByteToWideChar(doc_->codePage(), 0, text.data(), bytes, out, capacity);
    return length > 0 ? length : 0;
}

void Grid::drawText(HDC dc, const wchar_t* text, int length, const RECT& cell, Align align, COLORREF color) {
    if (!length) return;
    SetTextColor(dc, color);
    RECT area{cell.left + cellPadding_, cell.top, cell.right - cellPadding_, cell.bottom};
    if (area.right <= area.left) return;

    if (plugin::options().linesPerRow > 1) {
        UINT flags = DT_NOPREFIX | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS;
        if (align == Align::Right) flags |= DT_RIGHT;
        else if (align == Align::Center) flags |= DT_CENTER;
        area.top += (rowHeight_ - textHeight_ * plugin::options().linesPerRow) / 2;
        DrawTextW(dc, text, length, &area, flags);
        return;
    }

    if (wide_.size() < static_cast<size_t>(length) + 2) wide_.resize(static_cast<size_t>(length) + 2);
    std::copy(text, text + length, wide_.begin());
    flatten(wide_.data(), length);

    int fit = 0;
    SIZE size{};
    const int available = area.right - area.left;
    GetTextExtentExPointW(dc, wide_.data(), length, available, &fit, nullptr, &size);
    int shown = length;
    if (fit < length) {
        // Leave room for the ellipsis.
        SIZE dots{};
        GetTextExtentPoint32W(dc, L"\x2026", 1, &dots);
        GetTextExtentExPointW(dc, wide_.data(), length, std::max<int>(available - dots.cx, 0), &fit, nullptr, &size);
        shown = fit;
        wide_[static_cast<size_t>(shown++)] = L'\x2026';
        GetTextExtentPoint32W(dc, wide_.data(), shown, &size);
    }
    int x = area.left;
    if (align == Align::Right) x = area.right - size.cx;
    else if (align == Align::Center) x = area.left + (available - size.cx) / 2;
    const int y = cell.top + (rowHeight_ - textHeight_) / 2;
    ExtTextOutW(dc, std::max<int>(x, area.left), y, ETO_CLIPPED, &area, wide_.data(), static_cast<UINT>(shown), nullptr);
}

void Grid::selectedRange(size_t& firstRow, size_t& lastRow, size_t& firstColumn, size_t& lastColumn) const {
    const Selection& s = doc_->selection;
    firstRow = std::min(s.anchorRow, s.cursorRow);
    lastRow = std::max(s.anchorRow, s.cursorRow);
    firstColumn = std::min(s.anchorColumn, s.cursorColumn);
    lastColumn = std::max(s.anchorColumn, s.cursorColumn);
    if (s.wholeRows) {
        firstColumn = 0;
        lastColumn = doc_->visibleColumns() ? doc_->visibleColumns() - 1 : 0;
    }
    if (s.wholeColumns) {
        firstRow = 0;
        lastRow = doc_->viewRows() ? doc_->viewRows() - 1 : 0;
    }
}

bool Grid::isSelected(size_t viewRow, size_t position) const {
    size_t firstRow, lastRow, firstColumn, lastColumn;
    selectedRange(firstRow, lastRow, firstColumn, lastColumn);
    return viewRow >= firstRow && viewRow <= lastRow && position >= firstColumn && position <= lastColumn;
}

void Grid::paintRowCells(HDC dc, size_t rowIndex, size_t viewRow, int y, size_t first, size_t last, int clipLeft, int clipRight) {
    const Options& options = plugin::options();
    const Palette& p = plugin::palette();
    const Table& table = doc_->table();
    const COLORREF rowBackground = options.zebra && (rowIndex + doc_->scrollRow) % 2 ? p.zebra : p.background;
    const bool focused = GetFocus() == hwnd_;
    wchar_t buffer[kMaxCellChars];

    for (size_t position = first; position <= last && position < doc_->visibleColumns(); ++position) {
        const size_t column = doc_->fileColumn(position);
        const int left = columnScreenX(position);
        const int right = left + columnWidth(column);
        if (right <= clipLeft || left >= clipRight) continue;

        RECT cell{std::max(left, clipLeft), y, std::min(right, clipRight), y + rowHeight_};
        CellText shown;
        if (column < cells_.size()) shown = doc_->present(column, table.cellText(cells_[column], scratch_), format_);
        else shown.missing = true;

        COLORREF background = rowBackground;
        COLORREF color = shown.missing && options.dimMissing ? p.missingText : p.text;
        double position01;
        if (shown.numeric && doc_->scalePosition(column, shown.value, position01)) {
            background = scaleColor(options, position01);
            color = luminance(background) < 140 ? RGB(255, 255, 255) : RGB(0, 0, 0);
        }
        if (isSelected(viewRow, position)) {
            background = focused ? p.selection : mix(p.selection, p.background, 0.45);
            color = p.selectionText;
        }
        fill(dc, cell, background);

        const int length = toWide(shown.text, buffer, kMaxCellChars);
        RECT textCell{left, y, right, y + rowHeight_};
        // Clipping at the frozen boundary is done by the caller through the DC's clip region.
        drawText(dc, buffer, length, textCell, doc_->alignOf(column), color);

        if (options.showGridV) fill(dc, cell.right - 1, y, cell.right, y + rowHeight_, p.grid);
    }
}

void Grid::paintBody(HDC dc, const RECT& client) {
    const Options& options = plugin::options();
    const Palette& p = plugin::palette();
    const int top = bodyTop();
    const size_t rows = doc_->viewRows();
    if (doc_->visibleColumns() == 0) return;

    const size_t frozen = doc_->frozenColumns();
    const int frozenRight = gutterWidth_ + static_cast<int>(edges_[frozen]);
    const size_t firstScrolled = std::max(frozen, positionAt(frozenRight) == SIZE_MAX ? frozen : positionAt(frozenRight));
    const size_t lastScrolled = positionAt(client.right - 1) == SIZE_MAX ? doc_->visibleColumns() - 1 : positionAt(client.right - 1);

    const size_t count = static_cast<size_t>(std::max<long>(client.bottom - top, 0) / rowHeight_) + 1;
    int y = top;
    for (size_t i = 0; i < count && doc_->scrollRow + i < rows; ++i, y += rowHeight_) {
        const size_t viewRow = doc_->scrollRow + i;
        doc_->table().readRow(doc_->sourceRow(viewRow), cells_);

        HRGN clip = CreateRectRgn(frozenRight, top, client.right, client.bottom);
        SelectClipRgn(dc, clip);
        paintRowCells(dc, i, viewRow, y, firstScrolled, lastScrolled, frozenRight, client.right);
        if (frozen) {
            SetRectRgn(clip, gutterWidth_, top, frozenRight, client.bottom);
            SelectClipRgn(dc, clip);
            paintRowCells(dc, i, viewRow, y, 0, frozen - 1, gutterWidth_, frozenRight);
        }
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);

        if (options.showGridH) fill(dc, gutterWidth_, y + rowHeight_ - 1, client.right, y + rowHeight_, p.grid);
    }

    if (frozen) fill(dc, frozenRight - 1, top, frozenRight + 1, client.bottom, p.frozenLine);

    // Cursor frame around the current cell.
    const Selection& s = doc_->selection;
    if (rows && s.cursorRow >= doc_->scrollRow && s.cursorRow < doc_->scrollRow + count && s.cursorColumn < doc_->visibleColumns()) {
        const int left = columnScreenX(s.cursorColumn);
        const int right = left + columnWidth(doc_->fileColumn(s.cursorColumn));
        const int rowTop = top + static_cast<int>(s.cursorRow - doc_->scrollRow) * rowHeight_;
        const int clipLeft = s.cursorColumn < frozen ? gutterWidth_ : frozenRight;
        const COLORREF frame = GetFocus() == hwnd_ ? p.cursor : mix(p.cursor, p.background, 0.5);
        const int t = std::max(scale(2), 1);
        const int l = std::max(left, clipLeft), r = std::max(right, clipLeft);
        if (r > l) {
            fill(dc, l, rowTop, r, rowTop + t, frame);
            fill(dc, l, rowTop + rowHeight_ - t, r, rowTop + rowHeight_, frame);
            if (left >= clipLeft) fill(dc, left, rowTop, left + t, rowTop + rowHeight_, frame);
            fill(dc, r - t, rowTop, r, rowTop + rowHeight_, frame);
        }
    }
}

void Grid::paintHeaderCell(HDC dc, size_t position, int clipLeft, int clipRight) {
    const Palette& p = plugin::palette();
    const size_t column = doc_->fileColumn(position);
    const int left = columnScreenX(position);
    const int right = left + columnWidth(column);
    if (right <= clipLeft || left >= clipRight) return;

    const std::string& name = doc_->table().columnNames()[column];
    wchar_t buffer[kMaxCellChars];
    const int length = toWide(name, buffer, kMaxCellChars);
    flatten(buffer, length);

    // Sorted columns show a small triangle, with the key's rank if several are in use.
    int arrowSpace = 0;
    const auto& sort = doc_->sort();
    const auto key = std::find_if(sort.begin(), sort.end(), [&](const SortKey& k) { return k.column == column; });
    if (key != sort.end()) arrowSpace = scale(14 + (sort.size() > 1 ? 8 : 0));

    SelectObject(dc, headerFont_);
    SetTextColor(dc, p.headerText);
    RECT area{left + cellPadding_, 0, right - cellPadding_ - arrowSpace, headerHeight_};
    if (area.right > area.left) {
        UINT flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;
        const Align align = doc_->alignOf(column);
        if (align == Align::Right) flags |= DT_RIGHT;
        else if (align == Align::Center) flags |= DT_CENTER;
        DrawTextW(dc, buffer, length, &area, flags);
    }

    if (key != sort.end()) {
        const int half = scale(4);
        const int cx = right - cellPadding_ - half - (sort.size() > 1 ? scale(8) : 0);
        const int cy = headerHeight_ / 2;
        const bool up = !key->descending;
        const POINT triangle[3] = {{cx - half, cy + (up ? half / 2 : -half / 2)},
                                   {cx + half, cy + (up ? half / 2 : -half / 2)},
                                   {cx, cy + (up ? -half / 2 - 1 : half / 2 + 1)}};
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, p.accent);
        SetDCPenColor(dc, p.accent);
        Polygon(dc, triangle, 3);
        if (sort.size() > 1) {
            const wchar_t rank = static_cast<wchar_t>(L'1' + std::min<ptrdiff_t>(key - sort.begin(), 8));
            SetTextColor(dc, p.accent);
            RECT rankArea{cx + half + 1, 0, right, headerHeight_};
            SelectObject(dc, font_);
            DrawTextW(dc, &rank, 1, &rankArea, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        }
    }

    // A filtered column is marked along the bottom of its header.
    if (!doc_->columnFilter(column).empty()) {
        fill(dc, left, headerHeight_ - scale(3), right - 1, headerHeight_, p.accent);
    }
    fill(dc, right - 1, scale(4), right, headerHeight_ - scale(4), p.grid);
}

void Grid::paintHeader(HDC dc, const RECT& client) {
    const Palette& p = plugin::palette();
    fill(dc, 0, 0, client.right, headerHeight_, p.headerBackground);
    if (doc_->visibleColumns()) {
        const size_t frozen = doc_->frozenColumns();
        const int frozenRight = gutterWidth_ + static_cast<int>(edges_[frozen]);

        HRGN clip = CreateRectRgn(frozenRight, 0, client.right, headerHeight_);
        SelectClipRgn(dc, clip);
        const size_t first = positionAt(frozenRight);
        for (size_t position = first == SIZE_MAX ? frozen : std::max(first, frozen); position < doc_->visibleColumns(); ++position) {
            if (columnScreenX(position) >= client.right) break;
            paintHeaderCell(dc, position, frozenRight, client.right);
        }
        if (frozen) {
            SetRectRgn(clip, gutterWidth_, 0, frozenRight, headerHeight_);
            SelectClipRgn(dc, clip);
            for (size_t position = 0; position < frozen; ++position) paintHeaderCell(dc, position, gutterWidth_, frozenRight);
        }
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
        if (frozen) fill(dc, frozenRight - 1, 0, frozenRight + 1, headerHeight_, p.frozenLine);
    }
    fill(dc, 0, headerHeight_ - 1, client.right, headerHeight_, p.grid);

    if (drag_ == Drag::Reorder && doc_->visibleColumns()) {
        const int x = dropPosition_ < doc_->visibleColumns()
                          ? columnScreenX(dropPosition_)
                          : columnScreenX(doc_->visibleColumns() - 1) + columnWidth(doc_->fileColumn(doc_->visibleColumns() - 1));
        fill(dc, x - scale(1), 0, x + scale(1), client.bottom, p.accent);
    }
}

void Grid::paintFilterRow(HDC dc, const RECT& client) {
    const Palette& p = plugin::palette();
    const int top = headerHeight_;
    const int bottom = top + filterHeight_;
    fill(dc, 0, top, client.right, bottom, p.background);
    SelectObject(dc, font_);

    const size_t frozen = doc_->frozenColumns();
    const int frozenRight = gutterWidth_ + static_cast<int>(edges_[frozen]);
    const auto drawCell = [&](size_t position, int clipLeft, int clipRight) {
        const size_t column = doc_->fileColumn(position);
        const int left = columnScreenX(position);
        const int right = left + columnWidth(column);
        if (right <= clipLeft || left >= clipRight) return;
        RECT cell{std::max(left, clipLeft), top, std::min(right, clipRight), bottom};
        const std::string& filter = doc_->columnFilter(column);
        if (!doc_->columnFilterValid(column)) fill(dc, cell, mix(p.background, RGB(220, 50, 50), 0.3));
        wchar_t buffer[256];
        const int length = filter.empty() ? 0 : toWide(filter, buffer, 256);
        const RECT textCell{left, top, right, bottom};
        const int saved = rowHeight_;
        rowHeight_ = filterHeight_;
        if (length) drawText(dc, buffer, length, textCell, Align::Left, p.text);
        else drawText(dc, L"\x2026", 1, textCell, Align::Left, p.missingText);
        rowHeight_ = saved;
        fill(dc, cell.right - 1, top, cell.right, bottom, p.grid);
    };

    HRGN clip = CreateRectRgn(frozenRight, top, client.right, bottom);
    SelectClipRgn(dc, clip);
    const size_t first = positionAt(frozenRight);
    for (size_t position = first == SIZE_MAX ? frozen : std::max(first, frozen); position < doc_->visibleColumns(); ++position) {
        if (columnScreenX(position) >= client.right) break;
        drawCell(position, frozenRight, client.right);
    }
    if (frozen) {
        SetRectRgn(clip, gutterWidth_, top, frozenRight, bottom);
        SelectClipRgn(dc, clip);
        for (size_t position = 0; position < frozen; ++position) drawCell(position, gutterWidth_, frozenRight);
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    fill(dc, 0, bottom - 1, client.right, bottom, p.grid);
}

void Grid::paintGutter(HDC dc, const RECT& client) {
    if (!gutterWidth_) return;
    const Palette& p = plugin::palette();
    const int top = bodyTop();
    fill(dc, 0, top, gutterWidth_, client.bottom, p.gutterBackground);
    SelectObject(dc, font_);

    size_t firstRow, lastRow, firstColumn, lastColumn;
    selectedRange(firstRow, lastRow, firstColumn, lastColumn);
    const size_t rows = doc_->viewRows();
    int y = top;
    for (size_t row = doc_->scrollRow; row < rows && y < client.bottom; ++row, y += rowHeight_) {
        const bool selected = row >= firstRow && row <= lastRow;
        if (selected) fill(dc, 0, y, gutterWidth_, y + rowHeight_, mix(p.gutterBackground, p.accent, 0.25));
        wchar_t digits[24];
        const int length = swprintf_s(digits, L"%zu", doc_->sourceRow(row) + 1);
        const RECT cell{0, y, gutterWidth_, y + rowHeight_};
        drawText(dc, digits, length, cell, Align::Right, p.gutterText);
    }
    fill(dc, gutterWidth_ - 1, 0, gutterWidth_, client.bottom, p.grid);
}

void Grid::paint(HDC target) {
    RECT client;
    GetClientRect(hwnd_, &client);
    const int width = std::max<int>(client.right, 1), height = std::max<int>(client.bottom, 1);

    if (!bufferDc_) bufferDc_ = CreateCompatibleDC(target);
    if (!buffer_ || bufferSize_.cx < width || bufferSize_.cy < height) {
        bufferSize_ = {std::max<LONG>(bufferSize_.cx, width), std::max<LONG>(bufferSize_.cy, height)};
        HBITMAP bitmap = CreateCompatibleBitmap(target, bufferSize_.cx, bufferSize_.cy);
        SelectObject(bufferDc_, bitmap);    // a bitmap can only be deleted once it is out of the DC
        if (buffer_) DeleteObject(buffer_);
        buffer_ = bitmap;
    }
    HDC dc = bufferDc_;
    const Palette& p = plugin::palette();
    fill(dc, client, p.background);
    SetBkMode(dc, TRANSPARENT);

    if (doc_ && doc_->loaded()) {
        SelectObject(dc, font_);
        paintBody(dc, client);
        paintGutter(dc, client);
        if (plugin::options().showFilterRow) paintFilterRow(dc, client);
        paintHeader(dc, client);
        fill(dc, 0, 0, gutterWidth_, headerHeight_, p.headerBackground);
        fill(dc, 0, headerHeight_ - 1, gutterWidth_, headerHeight_, p.grid);
        if (gutterWidth_) fill(dc, gutterWidth_ - 1, 0, gutterWidth_, headerHeight_, p.grid);
    }
    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, GetStockObject(SYSTEM_FONT));  // our fonts must be free to be deleted on zoom
}

// ---- Scrolling and cursor --------------------------------------------------------------------

void Grid::scrollTo(size_t row, int x) {
    if (!doc_) return;
    const long long maxRow = static_cast<long long>(doc_->viewRows()) - std::max(visibleRows(), 1);
    row = static_cast<size_t>(std::clamp<long long>(static_cast<long long>(row), 0, std::max(maxRow, 0LL)));
    const long long maxX = edges_.back() - edges_[doc_->frozenColumns()] - viewportWidth();
    x = static_cast<int>(std::clamp<long long>(x, 0, std::max(maxX, 0LL)));
    if (row == doc_->scrollRow && x == doc_->scrollX) return;
    commitFilterEdit(false);
    hideTip();
    doc_->scrollRow = row;
    doc_->scrollX = x;
    updateScrollBars();
    doc_->notify(false);
    UpdateWindow(hwnd_);
}

void Grid::ensureVisible(size_t row, size_t position) {
    if (!doc_) return;
    size_t top = doc_->scrollRow;
    const size_t page = static_cast<size_t>(std::max(visibleRows(), 1));
    if (row < top) top = row;
    else if (row >= top + page) top = row - page + 1;

    int x = doc_->scrollX;
    if (position < doc_->visibleColumns() && position >= doc_->frozenColumns()) {
        const long long frozen = edges_[doc_->frozenColumns()];
        const long long left = edges_[position] - frozen;
        const long long right = edges_[position + 1] - frozen;
        const long long viewport = viewportWidth();
        if (left < x) x = static_cast<int>(left);
        else if (right > x + viewport) x = static_cast<int>(std::min(left, right - viewport));
    }
    scrollTo(top, x);
}

void Grid::setCursor(size_t row, size_t position, bool extend) {
    Selection& s = doc_->selection;
    s.cursorRow = row;
    s.cursorColumn = position;
    if (!extend) {
        s.anchorRow = row;
        s.anchorColumn = position;
        s.wholeRows = s.wholeColumns = false;
    }
    ensureVisible(row, position);
    selectionChanged();
}

void Grid::moveCursor(long long rows, long long columns, bool extend) {
    if (!doc_ || !doc_->viewRows() || !doc_->visibleColumns()) return;
    const Selection& s = doc_->selection;
    const long long row = std::clamp<long long>(static_cast<long long>(s.cursorRow) + rows, 0, static_cast<long long>(doc_->viewRows()) - 1);
    const long long column = std::clamp<long long>(static_cast<long long>(s.cursorColumn) + columns, 0, static_cast<long long>(doc_->visibleColumns()) - 1);
    setCursor(static_cast<size_t>(row), static_cast<size_t>(column), extend);
}

void Grid::selectionChanged() {
    if (doc_) doc_->notify(false);
}

bool Grid::handleKey(UINT key, bool ctrl, bool shift) {
    if (!doc_) return false;
    const long long page = std::max(visibleRows() - 1, 1);
    const long long rows = static_cast<long long>(doc_->viewRows());
    const long long columns = static_cast<long long>(doc_->visibleColumns());
    switch (key) {
        case VK_UP: moveCursor(ctrl ? -rows : -1, 0, shift); return true;
        case VK_DOWN: moveCursor(ctrl ? rows : 1, 0, shift); return true;
        case VK_LEFT: moveCursor(0, ctrl ? -columns : -1, shift); return true;
        case VK_RIGHT: moveCursor(0, ctrl ? columns : 1, shift); return true;
        case VK_PRIOR:
        case VK_NEXT:
            if (ctrl) return false;  // Ctrl+PgUp/PgDn switch tabs in Notepad++
            moveCursor(key == VK_NEXT ? page : -page, 0, shift);
            return true;
        case VK_HOME: moveCursor(ctrl ? -rows : 0, -columns, shift); return true;
        case VK_END: moveCursor(ctrl ? rows : 0, columns, shift); return true;
        case VK_TAB:
            if (ctrl) return false;  // Ctrl+Tab belongs to Notepad++
            moveCursor(0, shift ? -1 : 1, false);
            return true;
        case VK_RETURN:
            if (doc_->viewRows() && doc_->visibleColumns())
                plugin::showCellInText(*doc_, doc_->sourceRow(doc_->selection.cursorRow), doc_->fileColumn(doc_->selection.cursorColumn));
            return true;
        case VK_ESCAPE:
            if (doc_->filtered()) doc_->clearFilters();
            return true;
        case VK_F2:
            if (plugin::options().showFilterRow) beginFilterEdit(doc_->selection.cursorColumn);
            return true;
        case VK_OEM_PLUS:
        case VK_ADD:
            if (!ctrl) return false;
            zoom(1);
            return true;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            if (!ctrl) return false;
            zoom(-1);
            return true;
        default:
            break;
    }
    if (!ctrl) return false;
    switch (key) {
        case 'A':
            doc_->selection.wholeRows = doc_->selection.wholeColumns = true;
            selectionChanged();
            return true;
        case 'C':
            copySelection(ExportFormat::Tsv, shift);
            return true;
        case 'F':
            if (shift) return false;
            host_->focusFilter();
            return true;
        case 'L':
            if (!shift) return false;
            plugin::options().showFilterRow = !plugin::options().showFilterRow;
            plugin::optionsChanged();
            return true;
        default:
            return false;
    }
}

void Grid::zoom(int steps) {
    Options& o = plugin::options();
    const int size = std::clamp(o.fontSize + steps, 6, 40);
    if (size == o.fontSize) return;
    o.fontSize = size;
    plugin::optionsChanged();
}

// ---- Mouse -----------------------------------------------------------------------------------

void Grid::onLeftDown(int x, int y, WPARAM keys) {
    if (filterEdit_) commitFilterEdit(false);
    SetFocus(hwnd_);
    if (!doc_) return;
    hideTip();
    const Hit hit = hitTest(x, y);
    const bool shift = (keys & MK_SHIFT) != 0;
    const bool ctrl = (keys & MK_CONTROL) != 0;
    dragOrigin_ = {x, y};

    switch (hit.zone) {
        case Zone::Corner:
            doc_->selection.wholeRows = doc_->selection.wholeColumns = true;
            selectionChanged();
            break;
        case Zone::ResizeEdge:
            drag_ = Drag::Resize;
            dragPosition_ = hit.position;
            dragStartWidth_ = columnWidth(doc_->fileColumn(hit.position));
            SetCapture(hwnd_);
            break;
        case Zone::Header:
            if (ctrl) {
                Selection& s = doc_->selection;
                s.anchorColumn = s.cursorColumn = hit.position;
                s.wholeColumns = true;
                s.wholeRows = false;
                selectionChanged();
                break;
            }
            drag_ = Drag::HeaderPress;
            dragPosition_ = hit.position;
            shiftAtPress_ = shift;
            SetCapture(hwnd_);
            break;
        case Zone::Filter:
            beginFilterEdit(hit.position);
            break;
        case Zone::Gutter:
            doc_->selection.cursorRow = hit.row;
            if (!shift) doc_->selection.anchorRow = hit.row;
            doc_->selection.wholeRows = true;
            doc_->selection.wholeColumns = false;
            drag_ = Drag::Rows;
            SetCapture(hwnd_);
            SetTimer(hwnd_, kTimerDragScroll, kDragScrollMs, nullptr);
            selectionChanged();
            break;
        case Zone::Cell:
            setCursor(hit.row, hit.position, shift);
            drag_ = Drag::Cells;
            SetCapture(hwnd_);
            SetTimer(hwnd_, kTimerDragScroll, kDragScrollMs, nullptr);
            break;
        default:
            break;
    }
}

void Grid::extendDragSelection(int x, int y) {
    if (!doc_ || !doc_->viewRows() || !doc_->visibleColumns()) return;
    Selection& s = doc_->selection;
    RECT client;
    GetClientRect(hwnd_, &client);

    size_t row = s.cursorRow;
    if (y < bodyTop()) row = row ? row - 1 : 0;
    else if (y >= client.bottom) row = std::min(row + 1, doc_->viewRows() - 1);
    else row = rowAt(y);

    size_t position = s.cursorColumn;
    if (drag_ == Drag::Cells) {
        if (x >= client.right) {
            position = std::min(position + 1, doc_->visibleColumns() - 1);
        } else if (x < gutterWidth_) {
            position = position ? position - 1 : 0;
        } else {
            const size_t found = positionAt(x);
            position = found == SIZE_MAX ? doc_->visibleColumns() - 1 : found;
        }
    }
    if (row == s.cursorRow && position == s.cursorColumn) return;
    s.cursorRow = row;
    s.cursorColumn = position;
    ensureVisible(row, position);
    selectionChanged();
}

void Grid::onMouseMove(int x, int y, WPARAM keys) {
    if (!trackingMouse_ && plugin::options().tooltips) {
        TRACKMOUSEEVENT track{sizeof track, TME_HOVER | TME_LEAVE, hwnd_, 500};
        TrackMouseEvent(&track);
        trackingMouse_ = true;
    }
    if (tipVisible_) {
        const Hit now = hitTest(x, y);
        if (now.zone != tipCell_.zone || now.position != tipCell_.position || now.row != tipCell_.row) hideTip();
    }
    if (!doc_ || !(keys & MK_LBUTTON)) return;

    if ((drag_ == Drag::Resize || drag_ == Drag::HeaderPress || drag_ == Drag::Reorder) && dragPosition_ >= doc_->visibleColumns()) {
        return;  // the table was refreshed under the drag
    }
    switch (drag_) {
        case Drag::Resize: {
            const size_t column = doc_->fileColumn(dragPosition_);
            const int width = std::max<int>(dragStartWidth_ + x - dragOrigin_.x, scale(plugin::options().minColumnWidth / 2));
            doc_->setWidth(column, MulDiv(width, 96, dpi_));
            UpdateWindow(hwnd_);
            break;
        }
        case Drag::HeaderPress:
            if (std::abs(x - dragOrigin_.x) > scale(kDragThreshold)) drag_ = Drag::Reorder;
            [[fallthrough]];
        case Drag::Reorder:
            if (drag_ == Drag::Reorder) {
                const size_t over = positionAt(x);
                if (over == SIZE_MAX) {
                    dropPosition_ = x < gutterWidth_ ? 0 : doc_->visibleColumns();
                } else {
                    const int middle = columnScreenX(over) + columnWidth(doc_->fileColumn(over)) / 2;
                    dropPosition_ = x < middle ? over : over + 1;
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            break;
        case Drag::Cells:
        case Drag::Rows:
            extendDragSelection(x, y);
            break;
        default:
            break;
    }
}

void Grid::onLeftUp() {
    const Drag drag = drag_;
    drag_ = Drag::None;
    if (GetCapture() == hwnd_) ReleaseCapture();
    KillTimer(hwnd_, kTimerDragScroll);
    if (!doc_) return;

    switch (drag) {
        case Drag::HeaderPress:
            if (dragPosition_ < doc_->visibleColumns()) doc_->toggleSort(doc_->fileColumn(dragPosition_), shiftAtPress_);
            break;
        case Drag::Reorder:
            if (dropPosition_ != dragPosition_ && dropPosition_ != dragPosition_ + 1) doc_->moveColumn(dragPosition_, dropPosition_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        default:
            break;
    }
}

void Grid::onLeftDouble(int x, int y) {
    if (!doc_) return;
    const Hit hit = hitTest(x, y);
    if (hit.zone == Zone::ResizeEdge) {
        doc_->setWidth(doc_->fileColumn(hit.position), 0);
    } else if (hit.zone == Zone::Cell) {
        plugin::showCellInText(*doc_, doc_->sourceRow(hit.row), doc_->fileColumn(hit.position));
    } else {
        onLeftDown(x, y, 0);
    }
}

void Grid::onHover(int x, int y) {
    if (!doc_ || !plugin::options().tooltips || drag_ != Drag::None) return;
    const Hit hit = hitTest(x, y);
    std::string_view text;
    size_t column = 0;
    if (hit.zone == Zone::Header) {
        column = doc_->fileColumn(hit.position);
        text = doc_->table().columnNames()[column];
    } else if (hit.zone == Zone::Cell) {
        column = doc_->fileColumn(hit.position);
        doc_->table().readRow(doc_->sourceRow(hit.row), cells_);
        if (column >= cells_.size()) return;
        text = doc_->table().cellText(cells_[column], scratch_);
    } else {
        return;
    }
    const int width = columnWidth(column) - 2 * cellPadding_;
    if (measureWidth(text) <= width && text.find_first_of("\r\n") == std::string_view::npos) return;

    tipText_.resize(kMaxTipChars);
    tipText_.resize(static_cast<size_t>(toWide(text, tipText_.data(), kMaxTipChars)));
    TOOLINFOW info{sizeof info};
    info.hwnd = hwnd_;
    info.uId = reinterpret_cast<UINT_PTR>(hwnd_);
    info.lpszText = tipText_.data();
    SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));

    POINT p{x, y + rowHeight_};
    ClientToScreen(hwnd_, &p);
    SendMessageW(tip_, TTM_TRACKPOSITION, 0, MAKELPARAM(p.x, p.y));
    SendMessageW(tip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&info));
    tipVisible_ = true;
    tipCell_ = hit;
}

void Grid::hideTip() {
    if (!tipVisible_) return;
    TOOLINFOW info{sizeof info};
    info.hwnd = hwnd_;
    info.uId = reinterpret_cast<UINT_PTR>(hwnd_);
    SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&info));
    tipVisible_ = false;
}

// ---- Context menus ---------------------------------------------------------------------------

void Grid::onContextMenu(int x, int y) {
    if (!doc_) return;
    const Hit hit = hitTest(x, y);
    POINT screen{x, y};
    ClientToScreen(hwnd_, &screen);
    if (hit.zone == Zone::Header || hit.zone == Zone::ResizeEdge) {
        headerMenu(hit.position, screen);
    } else if (hit.zone == Zone::Cell) {
        if (!isSelected(hit.row, hit.position)) setCursor(hit.row, hit.position, false);
        cellMenu(hit.row, hit.position, screen);
    }
}

void Grid::headerMenu(size_t position, POINT screen) {
    const size_t column = doc_->fileColumn(position);
    const ColumnLayout layout = doc_->layout().columns[column];

    HMENU align = CreatePopupMenu();
    AppendMenuW(align, MF_STRING | (layout.align == Align::Auto ? MF_CHECKED : 0), CmdAlignBase + 0, L"Automatic");
    AppendMenuW(align, MF_STRING | (layout.align == Align::Left ? MF_CHECKED : 0), CmdAlignBase + 1, L"Left");
    AppendMenuW(align, MF_STRING | (layout.align == Align::Center ? MF_CHECKED : 0), CmdAlignBase + 2, L"Center");
    AppendMenuW(align, MF_STRING | (layout.align == Align::Right ? MF_CHECKED : 0), CmdAlignBase + 3, L"Right");

    HMENU decimals = CreatePopupMenu();
    AppendMenuW(decimals, MF_STRING | (layout.decimals == kDecimalsInherit ? MF_CHECKED : 0), CmdDecimalsBase, L"Use default");
    AppendMenuW(decimals, MF_STRING | (layout.decimals == kDecimalsRaw ? MF_CHECKED : 0), CmdDecimalsBase + 1, L"As in the file");
    AppendMenuW(decimals, MF_SEPARATOR, 0, nullptr);
    for (int d = 0; d <= 8; ++d) {
        AppendMenuW(decimals, MF_STRING | (layout.decimals == d ? MF_CHECKED : 0), CmdDecimalsBase + 2 + static_cast<UINT>(d),
                    std::to_wstring(d).c_str());
    }

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CmdSortAsc, L"Sort ascending");
    AppendMenuW(menu, MF_STRING, CmdSortDesc, L"Sort descending");
    AppendMenuW(menu, MF_STRING | (doc_->sort().empty() ? MF_GRAYED : 0), CmdSortClear, L"Clear sort");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdFilterColumn, L"Filter this column...");
    AppendMenuW(menu, MF_STRING, CmdSelectColumn, L"Select column");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(align), L"Alignment");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(decimals), L"Decimals");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdFitColumn, L"Fit width");
    AppendMenuW(menu, MF_STRING, CmdFitAll, L"Fit all columns");
    AppendMenuW(menu, MF_STRING, CmdSetWidth, L"Set width...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdFreeze, L"Freeze up to this column");
    AppendMenuW(menu, MF_STRING | (doc_->frozenColumns() ? 0 : MF_GRAYED), CmdUnfreeze, L"Unfreeze");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (doc_->visibleColumns() > 1 ? 0 : MF_GRAYED), CmdHide, L"Hide column");
    AppendMenuW(menu, MF_STRING, CmdShowAll, L"Show all columns");
    AppendMenuW(menu, MF_STRING, CmdColumns, L"Columns...");

    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    // A menu runs its own message loop, so timers may have changed the table while it was open.
    if (!command || !doc_ || column >= doc_->table().columnCount()) return;
    const auto& visible = doc_->visibleList();
    const auto found = std::find(visible.begin(), visible.end(), column);
    if (found == visible.end()) return;
    position = static_cast<size_t>(found - visible.begin());

    if (command >= CmdDecimalsBase) {
        doc_->setDecimals(column, static_cast<int>(command - CmdDecimalsBase) + kDecimalsInherit);
        return;
    }
    if (command >= CmdAlignBase) {
        doc_->setAlign(column, static_cast<Align>(command - CmdAlignBase));
        return;
    }
    switch (command) {
        case CmdSortAsc: doc_->setSort(column, false); break;
        case CmdSortDesc: doc_->setSort(column, true); break;
        case CmdSortClear: doc_->clearSort(); break;
        case CmdFilterColumn:
            if (!plugin::options().showFilterRow) {
                plugin::options().showFilterRow = true;
                plugin::optionsChanged();
            }
            beginFilterEdit(position);
            break;
        case CmdSelectColumn: {
            Selection& s = doc_->selection;
            s.anchorColumn = s.cursorColumn = position;
            s.wholeColumns = true;
            s.wholeRows = false;
            selectionChanged();
            break;
        }
        case CmdFitColumn: doc_->setWidth(column, 0); break;
        case CmdFitAll: fitColumns(); break;
        case CmdSetWidth: {
            int width = layout.width > 0 ? layout.width : (column < autoWidths_.size() ? autoWidths_[column] : kDefaultManualWidth);
            if (plugin::promptNumber(hwnd_, L"Column width (pixels):", width)) doc_->setWidth(column, std::max(width, 1));
            break;
        }
        case CmdFreeze: doc_->setFrozen(position + 1); break;
        case CmdUnfreeze: doc_->setFrozen(0); break;
        case CmdHide: doc_->setHidden(column, true); break;
        case CmdShowAll: doc_->setAllHidden(false); break;
        case CmdColumns: plugin::showColumnsDialog(*doc_, hwnd_); break;
        default: break;
    }
}

void Grid::cellMenu(size_t viewRow, size_t position, POINT screen) {
    const size_t column = doc_->fileColumn(position);
    const size_t source = doc_->sourceRow(viewRow);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CmdCopy, L"Copy\tCtrl+C");
    AppendMenuW(menu, MF_STRING, CmdCopyHeader, L"Copy with header\tCtrl+Shift+C");
    AppendMenuW(menu, MF_STRING, CmdCopyCsv, L"Copy as CSV");
    AppendMenuW(menu, MF_STRING, CmdCopyMarkdown, L"Copy as Markdown table");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdFilterValue, L"Show only this value");
    AppendMenuW(menu, MF_STRING, CmdExcludeValue, L"Hide this value");
    AppendMenuW(menu, MF_STRING | (doc_->filtered() ? 0 : MF_GRAYED), CmdClearFilters, L"Clear all filters\tEsc");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdShowText, L"Show in text\tEnter");

    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    // A menu runs its own message loop, so timers may have changed the table while it was open.
    // Positions in the view mean nothing now; the source row and file column still do.
    if (!command || !doc_ || source >= doc_->table().rowCount() || column >= doc_->table().columnCount()) return;

    const auto valueOfCell = [&]() {
        doc_->table().readRow(source, cells_);
        return column < cells_.size() ? std::string(doc_->table().cellText(cells_[column], scratch_)) : std::string();
    };
    switch (command) {
        case CmdCopy: copySelection(ExportFormat::Tsv, false); break;
        case CmdCopyHeader: copySelection(ExportFormat::Tsv, true); break;
        case CmdCopyCsv: copySelection(ExportFormat::Csv, true); break;
        case CmdCopyMarkdown: copySelection(ExportFormat::Markdown, true); break;
        case CmdFilterValue: doc_->setColumnFilter(column, "==" + valueOfCell()); break;
        case CmdExcludeValue: doc_->setColumnFilter(column, "!==" + valueOfCell()); break;
        case CmdClearFilters: doc_->clearFilters(); break;
        case CmdShowText: plugin::showCellInText(*doc_, source, column); break;
        default: break;
    }
}

void Grid::copySelection(ExportFormat format, bool header) {
    if (!doc_ || !doc_->visibleColumns()) return;
    size_t firstRow, lastRow, firstColumn, lastColumn;
    selectedRange(firstRow, lastRow, firstColumn, lastColumn);
    std::vector<size_t> columns;
    for (size_t position = firstColumn; position <= lastColumn; ++position) columns.push_back(doc_->fileColumn(position));

    const HCURSOR previous = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const std::string text = exportTable(doc_->table(), doc_->viewOrNull(), firstRow, lastRow, columns, format, header);
    const int length = MultiByteToWideChar(doc_->codePage(), 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(length) + 1) * sizeof(wchar_t));
    if (memory) {
        wchar_t* out = static_cast<wchar_t*>(GlobalLock(memory));
        MultiByteToWideChar(doc_->codePage(), 0, text.data(), static_cast<int>(text.size()), out, length);
        out[length] = 0;
        GlobalUnlock(memory);
        if (OpenClipboard(hwnd_)) {
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
            CloseClipboard();
        } else {
            GlobalFree(memory);
        }
    }
    SetCursor(previous);
}

// ---- Column filter editor --------------------------------------------------------------------

LRESULT CALLBACK Grid::filterEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    Grid* self = reinterpret_cast<Grid*>(data);
    if (message == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            self->commitFilterEdit(true);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            SetWindowTextW(hwnd, L"");
            self->commitFilterEdit(true);
            return 0;
        }
        if (wParam == VK_TAB) {
            const size_t next = self->filterPosition_ + 1;
            self->commitFilterEdit(false);
            if (next < self->doc_->visibleColumns()) {
                self->ensureVisible(self->doc_->scrollRow, next);
                self->beginFilterEdit(next);
            } else {
                SetFocus(self->hwnd_);
            }
            return 0;
        }
    } else if (message == WM_KILLFOCUS) {
        self->commitFilterEdit(false);
        return DefSubclassProc(hwnd, message, wParam, lParam);
    } else if (message == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_TAB)) {
        return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void Grid::beginFilterEdit(size_t position) {
    if (!doc_ || position >= doc_->visibleColumns() || !plugin::options().showFilterRow) return;
    if (!filterEdit_) {
        filterEdit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | WS_CLIPSIBLINGS, 0, 0, 0, 0, hwnd_, nullptr,
                                      plugin::instance(), nullptr);
        SetWindowSubclass(filterEdit_, filterEditProc, 1, reinterpret_cast<DWORD_PTR>(this));
        plugin::registerEdit(filterEdit_);
    }
    filterPosition_ = position;
    SendMessageW(filterEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    SetWindowTextW(filterEdit_, doc_->decode(doc_->columnFilter(doc_->fileColumn(position))).c_str());
    KillTimer(hwnd_, kTimerFilter);
    placeFilterEdit();
    ShowWindow(filterEdit_, SW_SHOW);
    SetFocus(filterEdit_);
    SendMessageW(filterEdit_, EM_SETSEL, 0, -1);
}

void Grid::placeFilterEdit() {
    if (!filterEdit_ || !IsWindowVisible(filterEdit_) || !doc_ || filterPosition_ >= doc_->visibleColumns()) return;
    const int left = columnScreenX(filterPosition_);
    const int width = columnWidth(doc_->fileColumn(filterPosition_));
    SetWindowPos(filterEdit_, HWND_TOP, left + 1, headerHeight_ + 1, width - 2, filterHeight_ - 2, SWP_NOACTIVATE);
}

void Grid::commitFilterEdit(bool focusGrid) {
    if (!filterEdit_ || filterPosition_ == SIZE_MAX) {  // not by visibility: the whole host may be hidden
        if (focusGrid) SetFocus(hwnd_);
        return;
    }
    KillTimer(hwnd_, kTimerFilter);
    const size_t position = filterPosition_;
    filterPosition_ = SIZE_MAX;
    ShowWindow(filterEdit_, SW_HIDE);
    if (doc_ && position < doc_->visibleColumns()) doc_->setColumnFilter(doc_->fileColumn(position), doc_->encode(editText(filterEdit_)));
    if (focusGrid) SetFocus(hwnd_);
}
