#include "TableHost.h"

#include <commctrl.h>
#include <uxtheme.h>

#include <algorithm>

#include "Plugin.h"
#include "dockingResource.h"

using namespace xsv;

namespace {

constexpr wchar_t kClassName[] = L"xsvViewerHost";
constexpr UINT_PTR kTimerFilter = 1;
constexpr UINT_PTR kTimerStats = 2;
constexpr UINT kFilterDelayMs = 250;
constexpr UINT kStatsDelayMs = 120;
constexpr size_t kMaxStatCells = 4000000;

enum ControlId { IdDelimiter = 1, IdQuote, IdHeader, IdFilter, IdFilterRow, IdFit, IdColumns, IdOptions, IdText };

struct Named {
    const wchar_t* name;
    const char* delimiter;
};
constexpr Named kDelimiters[] = {{L"Tab", "\t"}, {L"Comma", ","}, {L"Semicolon", ";"}, {L"Pipe", "|"}, {L"Space", " "}};

std::wstring delimiterName(const std::string& delimiter) {
    for (const Named& entry : kDelimiters) {
        if (delimiter == entry.delimiter) return entry.name;
    }
    return plugin::widen(delimiter);
}

std::wstring grouped(size_t value) {
    std::wstring digits = std::to_wstring(value), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out.push_back(L',');
        out.push_back(digits[i]);
    }
    return out;
}

std::wstring compact(double value) {
    wchar_t buffer[48];
    swprintf_s(buffer, L"%.15g", value);
    return buffer;
}

std::wstring codePageName(UINT codePage) {
    if (codePage == CP_UTF8) return L"UTF-8";
    if (codePage == CP_ACP) return L"ANSI";
    return L"CP" + std::to_wstring(codePage);
}

}  // namespace

void TableHost::registerClass() {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = plugin::instance();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

TableHost::~TableHost() {
    if (doc_) doc_->removeObserver(this);
    if (controlFont_) DeleteObject(controlFont_);
    if (toolbarBrush_) DeleteObject(toolbarBrush_);
    if (fieldBrush_) DeleteObject(fieldBrush_);
}

HWND TableHost::create(HWND parent) {
    CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, nullptr,
                    plugin::instance(), this);
    return hwnd_;
}

LRESULT CALLBACK TableHost::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    TableHost* self;
    if (message == WM_NCCREATE) {
        self = static_cast<TableHost*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TableHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT TableHost::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            createControls();
            return 0;
        case WM_SIZE:
            layoutChildren();
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
        case WM_COMMAND:
            if (lParam) onCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case WM_CTLCOLORBTN:
            if (toolbarBrush_) return reinterpret_cast<LRESULT>(toolbarBrush_);
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            if (fieldBrush_) {
                const Palette& p = plugin::palette();
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, p.text);
                SetBkColor(dc, p.background);
                return reinterpret_cast<LRESULT>(fieldBrush_);
            }
            break;
        case WM_TIMER:
            KillTimer(hwnd_, wParam);
            if (wParam == kTimerFilter) applyRowFilter();
            else if (wParam == kTimerStats) updateSelectionStats();
            return 0;
        case WM_DPICHANGED_AFTERPARENT:
            updateControlFont();
            layoutChildren();
            return 0;
        case WM_NOTIFY: {
            const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
            if (mode_ == Mode::Panel && LOWORD(header->code) == DMN_CLOSE) {
                plugin::panelClosed();
                return 0;
            }
            break;
        }
        case WM_SETFOCUS:
            SetFocus(grid_.hwnd());
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

LRESULT CALLBACK TableHost::editProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    TableHost* self = reinterpret_cast<TableHost*>(data);
    const bool isFilter = hwnd == self->filter_;
    if (message == WM_KEYDOWN) {
        if (wParam == VK_RETURN || wParam == VK_TAB) {
            if (isFilter) {
                KillTimer(self->hwnd_, kTimerFilter);
                self->applyRowFilter();
            } else {
                self->commitDelimiterText();
            }
            SetFocus(self->grid_.hwnd());
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (isFilter) SetWindowTextW(hwnd, L"");
            SetFocus(self->grid_.hwnd());
            return 0;
        }
    } else if (message == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_TAB)) {
        return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void TableHost::createControls() {
    const auto make = [&](const wchar_t* type, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), plugin::instance(), nullptr);
    };
    delimiter_ = make(WC_COMBOBOXW, L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, IdDelimiter);
    quote_ = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IdQuote);
    header_ = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IdHeader);
    filter_ = make(WC_EDITW, L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, IdFilter);
    filterRow_ = make(WC_BUTTONW, L"Column filters", BS_PUSHBUTTON | WS_TABSTOP, IdFilterRow);
    fit_ = make(WC_BUTTONW, L"Fit", BS_PUSHBUTTON | WS_TABSTOP, IdFit);
    columns_ = make(WC_BUTTONW, L"Columns", BS_PUSHBUTTON | WS_TABSTOP, IdColumns);
    options_ = make(WC_BUTTONW, L"Options", BS_PUSHBUTTON | WS_TABSTOP, IdOptions);
    if (mode_ == Mode::InTab) text_ = make(WC_BUTTONW, L"Text", BS_PUSHBUTTON | WS_TABSTOP, IdText);

    SendMessageW(filter_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Filter rows  (Ctrl+F)  e.g.  abc   >0.5   /regex/"));
    SendMessageW(delimiter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto"));
    for (const Named& entry : kDelimiters) SendMessageW(delimiter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.name));
    for (const wchar_t* item : {L"Quote: \"", L"Quote: '", L"Quote: none"})
        SendMessageW(quote_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    for (const wchar_t* item : {L"Header: auto", L"Header: yes", L"Header: no"})
        SendMessageW(header_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));

    COMBOBOXINFO info{sizeof info};
    GetComboBoxInfo(delimiter_, &info);
    delimiterEdit_ = info.hwndItem;
    SetWindowSubclass(delimiterEdit_, editProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(filter_, editProc, 1, reinterpret_cast<DWORD_PTR>(this));
    plugin::registerEdit(delimiterEdit_);
    plugin::registerEdit(filter_);

    grid_.create(hwnd_, this);
    updateControlFont();
}

void TableHost::updateControlFont() {
    const UINT dpi = GetDpiForWindow(hwnd_);
    NONCLIENTMETRICSW metrics{sizeof metrics};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0, dpi);
    if (controlFont_) DeleteObject(controlFont_);
    controlFont_ = CreateFontIndirectW(&metrics.lfMessageFont);
    for (HWND control : {delimiter_, quote_, header_, filter_, filterRow_, fit_, columns_, options_, text_}) {
        if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont_), TRUE);
    }
}

int TableHost::scale(int value) const { return MulDiv(value, static_cast<int>(GetDpiForWindow(hwnd_)), 96); }
int TableHost::toolbarHeight() const { return plugin::options().showToolbar ? scale(30) : 0; }
int TableHost::statusHeight() const { return plugin::options().showStatusBar ? scale(22) : 0; }

void TableHost::layoutChildren() {
    RECT client;
    GetClientRect(hwnd_, &client);
    const int toolbar = toolbarHeight();
    const int status = statusHeight();
    const bool show = plugin::options().showToolbar;

    for (HWND control : {delimiter_, quote_, header_, filter_, filterRow_, fit_, columns_, options_, text_}) {
        if (control) ShowWindow(control, show ? SW_SHOWNA : SW_HIDE);
    }
    if (show) {
        const int gap = scale(4);
        const int controlHeight = scale(23);
        const int y = (toolbar - controlHeight) / 2;

        struct Slot {
            HWND control;
            int width;
            bool combo;
            bool shown;
        };
        // Left to right, then the buttons at the right edge. In a narrow view (a split, a side panel)
        // the rarely used ones give way first, so controls never overlap.
        Slot slots[] = {{delimiter_, 128, true, true}, {quote_, 82, true, true},     {header_, 92, true, true},
                        {filterRow_, 92, false, true}, {fit_, 36, false, true},      {columns_, 62, false, true},
                        {options_, 58, false, true},   {text_, 48, false, text_ != nullptr}};
        int used = gap + scale(120);  // the filter box needs some room to be of use
        for (const Slot& slot : slots) used += slot.shown ? scale(slot.width) + gap : 0;
        for (int index : {1, 2, 3, 4, 5}) {
            if (used <= client.right) break;
            used -= scale(slots[index].width) + gap;
            slots[index].shown = false;
        }

        int left = gap;
        int right = client.right - gap;
        for (const Slot& slot : slots) {
            if (!slot.control) continue;
            ShowWindow(slot.control, slot.shown ? SW_SHOWNA : SW_HIDE);
            if (!slot.shown) continue;
            const int width = scale(slot.width);
            if (slot.combo) {
                MoveWindow(slot.control, left, y, width, scale(160), TRUE);
                SendMessageW(slot.control, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), controlHeight - scale(6));
                left += width + gap;
            }
        }
        for (auto slot = std::rbegin(slots); slot != std::rend(slots); ++slot) {
            if (slot->combo || !slot->control || !slot->shown) continue;
            right -= scale(slot->width);
            MoveWindow(slot->control, right, y, scale(slot->width), controlHeight, TRUE);
            right -= gap;
        }
        MoveWindow(filter_, left, y, std::max(right - left, 0), controlHeight, TRUE);

        // Resizing a combo box selects the text of its edit part.
        if (GetFocus() != delimiterEdit_) SendMessageW(delimiter_, CB_SETEDITSEL, 0, MAKELPARAM(-1, 0));
    }
    MoveWindow(grid_.hwnd(), 0, toolbar, client.right, std::max<int>(client.bottom - toolbar - status, 0), TRUE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void TableHost::attach(Document* document) {
    if (doc_ == document) return;

    // Anything typed so far belongs to the table it was typed for.
    if (doc_) {
        if (KillTimer(hwnd_, kTimerFilter)) applyRowFilter();
        if (GetFocus() == delimiterEdit_) commitDelimiterText();
        doc_->removeObserver(this);
    }
    doc_ = document;
    if (doc_) doc_->addObserver(this);
    haveStats_ = false;
    grid_.setDocument(doc_);
    syncing_ = true;
    if (!doc_) SetWindowTextW(filter_, L"");
    syncing_ = false;
    syncToolbar(true);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void TableHost::applyRowFilter() {
    if (!doc_) return;
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(filter_)) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(filter_, text.data(), static_cast<int>(text.size()))));
    doc_->setGlobalFilter(doc_->encode(text));
}

void TableHost::syncToolbar(bool replaceTypedText) {
    if (!doc_ || !doc_->loaded()) return;
    syncing_ = true;

    // Filters can also be cleared elsewhere, and the box must not claim one that is gone.
    if (replaceTypedText || GetFocus() != filter_) {
        const std::wstring applied = doc_->decode(doc_->globalFilter());
        std::wstring shown(static_cast<size_t>(GetWindowTextLengthW(filter_)) + 1, L'\0');
        shown.resize(static_cast<size_t>(GetWindowTextW(filter_, shown.data(), static_cast<int>(shown.size()))));
        if (shown != applied) SetWindowTextW(filter_, applied.c_str());
    }

    const ParseOptions& parse = doc_->parseOptions();
    const std::wstring detected = delimiterName(doc_->table().dialect().delimiter);

    // The "Auto" entry names what was detected, so the guess is visible.
    const std::wstring autoLabel = L"Auto (" + detected + L")";
    SendMessageW(delimiter_, CB_DELETESTRING, 0, 0);
    SendMessageW(delimiter_, CB_INSERTSTRING, 0, reinterpret_cast<LPARAM>(autoLabel.c_str()));
    if (replaceTypedText || GetFocus() != delimiterEdit_) {
        SetWindowTextW(delimiter_, parse.delimiter.empty() ? autoLabel.c_str() : delimiterName(parse.delimiter).c_str());
        SendMessageW(delimiter_, CB_SETEDITSEL, 0, MAKELPARAM(-1, 0));  // no selection left showing in the box
    }
    SendMessageW(quote_, CB_SETCURSEL, parse.quote == '"' ? 0 : (parse.quote == '\'' ? 1 : 2), 0);
    SendMessageW(header_, CB_SETCURSEL, static_cast<WPARAM>(parse.header), 0);
    syncing_ = false;
}

void TableHost::changeParse(void (*change)(ParseOptions&, int), int value) {
    if (!doc_) return;
    ParseOptions parse = doc_->parseOptions();
    change(parse, value);
    const HCURSOR previous = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    doc_->setParseOptions(parse);
    SetCursor(previous);
}

void TableHost::commitDelimiterText() {
    if (!doc_) return;
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(delimiter_)) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(delimiter_, text.data(), static_cast<int>(text.size()))));

    std::string delimiter;
    if (text.rfind(L"Auto", 0) != 0) {
        delimiter = plugin::narrow(text);
        for (const Named& entry : kDelimiters) {
            if (text == entry.name) delimiter = entry.delimiter;
        }
        if (delimiter == "\\t") delimiter = "\t";
    }
    if (delimiter == doc_->parseOptions().delimiter) return;
    ParseOptions parse = doc_->parseOptions();
    parse.delimiter = delimiter;
    doc_->setParseOptions(parse);
}

void TableHost::onCommand(int id, int code) {
    if (syncing_) return;
    switch (id) {
        case IdDelimiter:
            if (code == CBN_SELCHANGE) {
                const int index = static_cast<int>(SendMessageW(delimiter_, CB_GETCURSEL, 0, 0));
                if (index < 0) break;
                changeParse([](ParseOptions& p, int i) { p.delimiter = i == 0 ? "" : kDelimiters[i - 1].delimiter; }, index);
                SetFocus(grid_.hwnd());
            } else if (code == CBN_KILLFOCUS) {
                commitDelimiterText();
            }
            break;
        case IdQuote:
            if (code == CBN_SELCHANGE) {
                changeParse([](ParseOptions& p, int i) { p.quote = i == 0 ? '"' : (i == 1 ? '\'' : '\0'); },
                            static_cast<int>(SendMessageW(quote_, CB_GETCURSEL, 0, 0)));
                SetFocus(grid_.hwnd());
            }
            break;
        case IdHeader:
            if (code == CBN_SELCHANGE) {
                changeParse([](ParseOptions& p, int i) { p.header = static_cast<Tri>(i); },
                            static_cast<int>(SendMessageW(header_, CB_GETCURSEL, 0, 0)));
                SetFocus(grid_.hwnd());
            }
            break;
        case IdFilter:
            if (code == EN_CHANGE) SetTimer(hwnd_, kTimerFilter, kFilterDelayMs, nullptr);
            else if (code == EN_KILLFOCUS && KillTimer(hwnd_, kTimerFilter)) applyRowFilter();
            break;
        case IdFilterRow:
            plugin::options().showFilterRow = !plugin::options().showFilterRow;
            plugin::optionsChanged();
            SetFocus(grid_.hwnd());
            break;
        case IdFit:
            grid_.fitColumns();
            SetFocus(grid_.hwnd());
            break;
        case IdColumns:
            if (doc_) plugin::showColumnsDialog(*doc_, hwnd_);
            break;
        case IdOptions:
            plugin::showOptionsDialog(hwnd_);
            break;
        case IdText:
            plugin::toggleTable();
            break;
        default:
            break;
    }
}

void TableHost::documentChanged(bool structure) {
    grid_.refresh(structure);
    if (structure) {
        syncToolbar();
        statsStale_ = true;  // hidden or moved columns change what a selection covers
    }
    SetTimer(hwnd_, kTimerStats, kStatsDelayMs, nullptr);
    RECT client;
    GetClientRect(hwnd_, &client);
    client.top = client.bottom - statusHeight();
    InvalidateRect(hwnd_, &client, FALSE);
}

void TableHost::updateSelectionStats() {
    // Scrolling lands here too; a large selection should not be summed again for that.
    if (doc_ && haveStats_ && doc_->selection == statsSelection_ && doc_->viewVersion() == statsView_ && !statsStale_) return;
    statsStale_ = false;
    haveStats_ = false;
    if (doc_ && doc_->viewRows() && doc_->visibleColumns()) {
        const Selection& s = doc_->selection;
        statsSelection_ = s;
        statsView_ = doc_->viewVersion();
        size_t firstRow = std::min(s.anchorRow, s.cursorRow), lastRow = std::max(s.anchorRow, s.cursorRow);
        size_t firstColumn = std::min(s.anchorColumn, s.cursorColumn), lastColumn = std::max(s.anchorColumn, s.cursorColumn);
        if (s.wholeRows) {
            firstColumn = 0;
            lastColumn = doc_->visibleColumns() - 1;
        }
        if (s.wholeColumns) {
            firstRow = 0;
            lastRow = doc_->viewRows() - 1;
        }
        const size_t cells = (lastRow - firstRow + 1) * (lastColumn - firstColumn + 1);
        if (cells > 1 && cells <= kMaxStatCells) {
            std::vector<size_t> columns;
            for (size_t i = firstColumn; i <= lastColumn; ++i) columns.push_back(doc_->fileColumn(i));
            stats_ = computeSelectionStats(doc_->table(), doc_->viewOrNull(), firstRow, lastRow, columns, doc_->missing());
            haveStats_ = true;
        }
    }
    RECT client;
    GetClientRect(hwnd_, &client);
    client.top = client.bottom - statusHeight();
    InvalidateRect(hwnd_, &client, FALSE);
}

std::wstring TableHost::leftStatus() const {
    if (!doc_ || !doc_->loaded()) return L"";
    const Table& table = doc_->table();
    std::wstring text;
    if (doc_->viewRows() == table.rowCount()) text = grouped(table.rowCount()) + L" rows";
    else text = grouped(doc_->viewRows()) + L" of " + grouped(table.rowCount()) + L" rows";
    text += L"  \x00B7  " + grouped(doc_->visibleColumns());
    if (doc_->visibleColumns() != table.columnCount()) text += L" of " + grouped(table.columnCount());
    text += L" columns  \x00B7  " + delimiterName(table.dialect().delimiter) + L"  \x00B7  " + codePageName(doc_->codePage());
    if (table.dialect().decimalComma) text += L"  \x00B7  decimal comma";
    if (table.dialect().rowNamesHeader) text += L"  \x00B7  R row names";
    if (table.raggedRows()) text += L"  \x00B7  " + grouped(table.raggedRows()) + L" uneven rows";
    if (table.unbalancedQuotes()) text += L"  \x00B7  " + grouped(table.unbalancedQuotes()) + L" unclosed quotes";
    return text;
}

std::wstring TableHost::rightStatus() const {
    if (!haveStats_) return L"";
    std::wstring text = grouped(stats_.cells) + L" cells";
    if (stats_.numbers) {
        text += L"  \x00B7  n " + grouped(stats_.numbers) + L"  \x00B7  sum " + compact(stats_.sum) + L"  \x00B7  mean " +
                compact(stats_.mean()) + L"  \x00B7  min " + compact(stats_.min) + L"  \x00B7  max " + compact(stats_.max);
    }
    return text;
}

void TableHost::paint(HDC dc) {
    const Palette& p = plugin::palette();
    RECT client;
    GetClientRect(hwnd_, &client);

    SetDCBrushColor(dc, p.headerBackground);
    const HBRUSH brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    RECT toolbar{0, 0, client.right, toolbarHeight()};
    FillRect(dc, &toolbar, brush);

    const int height = statusHeight();
    if (!height) return;
    RECT status{0, client.bottom - height, client.right, client.bottom};
    FillRect(dc, &status, brush);
    RECT line{0, status.top, client.right, status.top + 1};
    SetDCBrushColor(dc, p.grid);
    FillRect(dc, &line, brush);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, p.gutterText);
    HGDIOBJ old = SelectObject(dc, controlFont_);
    RECT area{status.left + scale(8), status.top, status.right - scale(8), status.bottom};
    const std::wstring right = rightStatus();
    if (!right.empty()) DrawTextW(dc, right.c_str(), static_cast<int>(right.size()), &area, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    if (!right.empty()) {
        RECT measured{};
        DrawTextW(dc, right.c_str(), static_cast<int>(right.size()), &measured, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        area.right -= measured.right + scale(16);
    }
    const std::wstring left = leftStatus();
    DrawTextW(dc, left.c_str(), static_cast<int>(left.size()), &area, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);
}

void TableHost::optionsChanged() {
    if (toolbarBrush_) DeleteObject(toolbarBrush_);
    if (fieldBrush_) DeleteObject(fieldBrush_);
    toolbarBrush_ = CreateSolidBrush(plugin::palette().headerBackground);
    fieldBrush_ = CreateSolidBrush(plugin::palette().background);
    const bool dark = plugin::darkMode();
    for (HWND control : {delimiter_, quote_, header_}) SetWindowTheme(control, dark ? L"DarkMode_CFD" : nullptr, nullptr);
    for (HWND control : {filter_, filterRow_, fit_, columns_, options_, text_}) {
        if (control) SetWindowTheme(control, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    }
    SetWindowTextW(filterRow_, plugin::options().showFilterRow ? L"Hide filters" : L"Column filters");
    layoutChildren();
    grid_.optionsChanged();
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void TableHost::focusFilter() {
    if (!plugin::options().showToolbar) {
        plugin::options().showToolbar = true;
        plugin::optionsChanged();
    }
    SetFocus(filter_);
    SendMessageW(filter_, EM_SETSEL, 0, -1);
}
