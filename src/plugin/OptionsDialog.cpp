// The options dialog. Every field is one entry in buildFields(); controls are generated from that
// list, and each change applies at once so the table behind the dialog works as a live preview.
#include <windows.h>

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "Notepad_plus_msgs.h"
#include "Options.h"
#include "Plugin.h"
#include "resource.h"

using namespace xsv;

namespace {

constexpr int kFirstControlId = 2000;
constexpr int kLabelWidth = 150;       // dialog units
constexpr int kControlWidth = 150;
constexpr int kRowHeight = 15;
constexpr int kPageLeft = 16;
constexpr int kPageTop = 28;

enum class Kind { Check, Number, Text, Choice, Color, ColorPair, Font };

struct Field {
    int page = 0;
    const wchar_t* label = L"";
    Kind kind = Kind::Check;
    std::function<int()> get;
    std::function<void(int)> set;
    std::string* text = nullptr;
    Color* color = nullptr;
    Color* secondColor = nullptr;
    std::vector<std::wstring> choices;
    HWND labelWindow = nullptr, control = nullptr, secondControl = nullptr;
};

const wchar_t* const kPages[] = {L"Opening", L"Parsing", L"Table", L"Values", L"Colours"};

Field flag(int page, const wchar_t* label, bool& value) {
    Field f;
    f.page = page;
    f.label = label;
    f.kind = Kind::Check;
    f.get = [&value] { return value ? 1 : 0; };
    f.set = [&value](int v) { value = v != 0; };
    return f;
}

Field number(int page, const wchar_t* label, int& value, int low, int high) {
    Field f;
    f.page = page;
    f.label = label;
    f.kind = Kind::Number;
    f.get = [&value] { return value; };
    f.set = [&value, low, high](int v) { value = std::clamp(v, low, high); };
    return f;
}

Field text(int page, const wchar_t* label, std::string& value) {
    Field f;
    f.page = page;
    f.label = label;
    f.kind = Kind::Text;
    f.text = &value;
    return f;
}

template <typename E>
Field choice(int page, const wchar_t* label, E& value, std::vector<std::wstring> choices) {
    Field f;
    f.page = page;
    f.label = label;
    f.kind = Kind::Choice;
    f.choices = std::move(choices);
    f.get = [&value] { return static_cast<int>(value); };
    f.set = [&value](int v) { value = static_cast<E>(v); };
    return f;
}

Field color(int page, const wchar_t* label, Color& value) {
    Field f;
    f.page = page;
    f.label = label;
    f.kind = Kind::Color;
    f.color = &value;
    return f;
}

Field colorPair(const wchar_t* label, Color& light, Color& dark) {
    Field f;
    f.page = 4;
    f.label = label;
    f.kind = Kind::ColorPair;
    f.color = &light;
    f.secondColor = &dark;
    return f;
}

std::vector<Field> buildFields(Options& o) {
    enum { Opening, Parsing, Table, Values, Colours };
    const std::vector<std::wstring> detect = {L"Detect", L"Yes", L"No"};
    std::vector<Field> f;

    f.push_back(flag(Opening, L"Open these files as a table automatically", o.autoOpen));
    f.push_back(text(Opening, L"File extensions (separated by ;)", o.extensions));
    f.push_back(flag(Opening, L"Update the table when the text changes", o.autoRefresh));
    f.push_back(flag(Opening, L"Remember columns and parsing for each file", o.rememberLayout));
    f.push_back(flag(Opening, L"Panel selects the cell at the text caret", o.followCaret));

    {
        Field delimiter;
        delimiter.page = Parsing;
        delimiter.label = L"Delimiter";
        delimiter.kind = Kind::Choice;
        delimiter.choices = {L"Detect", L"Tab", L"Comma", L"Semicolon", L"Pipe", L"Space"};
        static const char* const values[] = {"", "\t", ",", ";", "|", " "};
        delimiter.get = [&o] {
            for (int i = 0; i < 6; ++i) {
                if (o.parse.delimiter == values[i]) return i;
            }
            return 0;
        };
        delimiter.set = [&o](int i) { o.parse.delimiter = values[i]; };
        f.push_back(std::move(delimiter));

        Field quote;
        quote.page = Parsing;
        quote.label = L"Quote character";
        quote.kind = Kind::Choice;
        quote.choices = {L"Double quote", L"Single quote", L"None"};
        quote.get = [&o] { return o.parse.quote == '"' ? 0 : (o.parse.quote == '\'' ? 1 : 2); };
        quote.set = [&o](int i) { o.parse.quote = i == 0 ? '"' : (i == 1 ? '\'' : '\0'); };
        f.push_back(std::move(quote));

        Field escape;
        escape.page = Parsing;
        escape.label = L"Escape character";
        escape.kind = Kind::Choice;
        escape.choices = {L"None", L"Backslash"};
        escape.get = [&o] { return o.parse.escape ? 1 : 0; };
        escape.set = [&o](int i) { o.parse.escape = i ? '\\' : '\0'; };
        f.push_back(std::move(escape));
    }
    f.push_back(flag(Parsing, L"Doubled quotes stand for one quote", o.parse.doubleQuote));
    f.push_back(choice(Parsing, L"First row is a header", o.parse.header, detect));
    f.push_back(choice(Parsing, L"Header one field short (R row names)", o.parse.rowNames, detect));
    f.push_back(choice(Parsing, L"Decimal comma", o.parse.decimalComma, detect));
    f.push_back(flag(Parsing, L"Detect # comment lines", o.parse.commentAuto));
    f.push_back(text(Parsing, L"Comment prefix (when not detected)", o.parse.comment));
    {
        Field skip;
        skip.page = Parsing;
        skip.label = L"Lines to skip at the start";
        skip.kind = Kind::Number;
        skip.get = [&o] { return static_cast<int>(o.parse.skipLines); };
        skip.set = [&o](int v) { o.parse.skipLines = static_cast<size_t>(std::max(v, 0)); };
        f.push_back(std::move(skip));
    }
    f.push_back(flag(Parsing, L"Trim spaces around cells", o.parse.trimCells));
    f.push_back(flag(Parsing, L"Skip blank lines", o.parse.skipBlankLines));
    f.push_back(text(Parsing, L"Missing values (separated by ;)", o.missingTokens));

    {
        Field font;
        font.page = Table;
        font.label = L"Font";
        font.kind = Kind::Font;
        f.push_back(std::move(font));
    }
    f.push_back(flag(Table, L"Bold column headers", o.headerBold));
    f.push_back(number(Table, L"Lines per row (wraps long text)", o.linesPerRow, 1, 20));
    f.push_back(number(Table, L"Row padding", o.rowPadding, 0, 40));
    f.push_back(number(Table, L"Cell padding", o.cellPadding, 0, 40));
    f.push_back(number(Table, L"Smallest column width", o.minColumnWidth, 10, 400));
    f.push_back(number(Table, L"Largest automatic column width", o.maxAutoColumnWidth, 40, 4000));
    f.push_back(number(Table, L"Rows measured for column widths", o.autoFitRows, 1, 1000000));
    f.push_back(flag(Table, L"Toolbar", o.showToolbar));
    f.push_back(flag(Table, L"Status bar", o.showStatusBar));
    f.push_back(flag(Table, L"Column filter row", o.showFilterRow));
    f.push_back(flag(Table, L"Row numbers", o.showRowNumbers));
    f.push_back(flag(Table, L"Horizontal grid lines", o.showGridH));
    f.push_back(flag(Table, L"Vertical grid lines", o.showGridV));
    f.push_back(flag(Table, L"Alternating row colours", o.zebra));
    f.push_back(flag(Table, L"Show cut-off text when hovering", o.tooltips));

    f.push_back(flag(Values, L"Right-align number columns", o.rightAlignNumbers));
    {
        Field decimals;
        decimals.page = Values;
        decimals.label = L"Decimals shown";
        decimals.kind = Kind::Choice;
        decimals.choices = {L"As in the file", L"0", L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8"};
        decimals.get = [&o] { return o.decimals + 1; };
        decimals.set = [&o](int i) { o.decimals = i - 1; };
        f.push_back(std::move(decimals));
    }
    f.push_back(flag(Values, L"Thousands separators", o.thousands));
    f.push_back(flag(Values, L"Dim missing values", o.dimMissing));
    f.push_back(choice(Values, L"Colour numbers by value", o.colorScale, {L"Off", L"Per column", L"Whole table"}));
    f.push_back(color(Values, L"Colour for low values", o.scaleLow));
    f.push_back(color(Values, L"Colour for middle values", o.scaleMid));
    f.push_back(color(Values, L"Colour for high values", o.scaleHigh));
    f.push_back(flag(Values, L"Natural sort (s2 before s10)", o.naturalSort));
    f.push_back(flag(Values, L"Case-sensitive filters", o.caseSensitiveFilter));

    f.push_back(choice(Colours, L"Theme", o.theme, {L"Follow Notepad++", L"Light", L"Dark"}));
    const struct {
        const wchar_t* label;
        Color Palette::*member;
    } colors[] = {{L"Background", &Palette::background},
                  {L"Text", &Palette::text},
                  {L"Header background", &Palette::headerBackground},
                  {L"Header text", &Palette::headerText},
                  {L"Grid lines", &Palette::grid},
                  {L"Alternate rows", &Palette::zebra},
                  {L"Selection", &Palette::selection},
                  {L"Selected text", &Palette::selectionText},
                  {L"Cell cursor", &Palette::cursor},
                  {L"Row number background", &Palette::gutterBackground},
                  {L"Row number text", &Palette::gutterText},
                  {L"Frozen column line", &Palette::frozenLine},
                  {L"Missing values", &Palette::missingText},
                  {L"Accent", &Palette::accent}};
    for (const auto& entry : colors) f.push_back(colorPair(entry.label, o.light.*entry.member, o.dark.*entry.member));
    return f;
}

struct DialogState {
    Options original;
    std::vector<Field> fields;
    HWND hwnd = nullptr;
    HWND tabs = nullptr;
    int page = 0;
    bool loading = false;
};

std::wstring fontLabel(const Options& o) { return plugin::widen(o.fontFace) + L", " + std::to_wstring(o.fontSize) + L" pt"; }

void loadControls(DialogState& state) {
    state.loading = true;
    for (Field& f : state.fields) {
        switch (f.kind) {
            case Kind::Check:
                SendMessageW(f.control, BM_SETCHECK, f.get() ? BST_CHECKED : BST_UNCHECKED, 0);
                break;
            case Kind::Number:
                SetWindowTextW(f.control, std::to_wstring(f.get()).c_str());
                break;
            case Kind::Text:
                SetWindowTextW(f.control, plugin::widen(*f.text).c_str());
                break;
            case Kind::Choice:
                SendMessageW(f.control, CB_SETCURSEL, static_cast<WPARAM>(f.get()), 0);
                break;
            case Kind::Font:
                SetWindowTextW(f.control, fontLabel(plugin::options()).c_str());
                break;
            case Kind::Color:
            case Kind::ColorPair:
                InvalidateRect(f.control, nullptr, TRUE);
                if (f.secondControl) InvalidateRect(f.secondControl, nullptr, TRUE);
                break;
        }
    }
    state.loading = false;
}

void showPage(DialogState& state, int page) {
    state.page = page;
    for (Field& f : state.fields) {
        const int show = f.page == page ? SW_SHOW : SW_HIDE;
        for (HWND window : {f.labelWindow, f.control, f.secondControl}) {
            if (window) ShowWindow(window, show);
        }
    }
}

void readField(Field& f) {
    wchar_t buffer[1024] = {};
    if (f.kind == Kind::Number) {
        GetWindowTextW(f.control, buffer, 1024);
        f.set(_wtoi(buffer));
    } else if (f.kind == Kind::Text) {
        GetWindowTextW(f.control, buffer, 1024);
        *f.text = plugin::narrow(buffer);
    }
}

void applyEdits(DialogState& state) {
    for (Field& f : state.fields) readField(f);
    plugin::optionsChanged();
}

void createControls(DialogState& state) {
    HWND hwnd = state.hwnd;
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    const auto toPixels = [&](int x, int y, int width, int height) {
        RECT r{x, y, x + width, y + height};
        MapDialogRect(hwnd, &r);
        return r;
    };
    const auto make = [&](const wchar_t* type, const wchar_t* title, DWORD style, RECT r, int id) {
        HWND control = CreateWindowExW(0, type, title, WS_CHILD | style, r.left, r.top, r.right - r.left, r.bottom - r.top, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), plugin::instance(), nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    };

    int rows[std::size(kPages)] = {};
    int id = kFirstControlId;
    for (Field& f : state.fields) {
        const int row = rows[f.page]++;
        const int y = kPageTop + row * kRowHeight;
        const int controlLeft = kPageLeft + kLabelWidth;
        if (f.kind == Kind::Check) {
            f.control = make(WC_BUTTONW, f.label, BS_AUTOCHECKBOX | WS_TABSTOP, toPixels(kPageLeft, y, kLabelWidth + kControlWidth, 12), id);
        } else {
            f.labelWindow = make(WC_STATICW, f.label, SS_LEFT, toPixels(kPageLeft, y + 2, kLabelWidth - 4, 10), 0);
            switch (f.kind) {
                case Kind::Number:
                    f.control = make(WC_EDITW, L"", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, toPixels(controlLeft, y, 50, 12), id);
                    break;
                case Kind::Text:
                    f.control = make(WC_EDITW, L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, toPixels(controlLeft, y, kControlWidth, 12), id);
                    break;
                case Kind::Choice:
                    f.control = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, toPixels(controlLeft, y, kControlWidth, 100), id);
                    for (const std::wstring& item : f.choices) SendMessageW(f.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
                    break;
                case Kind::Font:
                    f.control = make(WC_BUTTONW, L"", BS_PUSHBUTTON | WS_TABSTOP, toPixels(controlLeft, y, kControlWidth, 13), id);
                    break;
                case Kind::Color:
                    f.control = make(WC_BUTTONW, L"", BS_OWNERDRAW | WS_TABSTOP, toPixels(controlLeft, y, 50, 12), id);
                    break;
                case Kind::ColorPair:
                    f.control = make(WC_BUTTONW, L"", BS_OWNERDRAW | WS_TABSTOP, toPixels(controlLeft, y, 50, 12), id);
                    f.secondControl = make(WC_BUTTONW, L"", BS_OWNERDRAW | WS_TABSTOP, toPixels(controlLeft + 56, y, 50, 12), id + 1);
                    break;
                default:
                    break;
            }
        }
        id += 2;
    }
}

Field* fieldFor(DialogState& state, int id, bool& second) {
    const int index = id - kFirstControlId;
    if (index < 0 || index / 2 >= static_cast<int>(state.fields.size())) return nullptr;
    second = index % 2 == 1;
    return &state.fields[static_cast<size_t>(index / 2)];
}

void pickColor(DialogState& state, Color& target) {
    static COLORREF custom[16] = {};
    CHOOSECOLORW chooser{sizeof chooser};
    chooser.hwndOwner = state.hwnd;
    chooser.rgbResult = target;
    chooser.lpCustColors = custom;
    chooser.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&chooser)) return;
    target = chooser.rgbResult;
    loadControls(state);
    plugin::optionsChanged();
}

void pickFont(DialogState& state) {
    Options& o = plugin::options();
    HDC screen = GetDC(state.hwnd);
    LOGFONTW logFont{};
    logFont.lfHeight = -MulDiv(o.fontSize, GetDeviceCaps(screen, LOGPIXELSY), 72);
    wcsncpy_s(logFont.lfFaceName, plugin::widen(o.fontFace).c_str(), _TRUNCATE);
    ReleaseDC(state.hwnd, screen);

    CHOOSEFONTW chooser{sizeof chooser};
    chooser.hwndOwner = state.hwnd;
    chooser.lpLogFont = &logFont;
    chooser.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS | CF_NOSCRIPTSEL;
    if (!ChooseFontW(&chooser)) return;
    o.fontFace = plugin::narrow(logFont.lfFaceName);
    o.fontSize = std::max(chooser.iPointSize / 10, 4);
    loadControls(state);
    plugin::optionsChanged();
}

INT_PTR CALLBACK dialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    DialogState* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    switch (message) {
        case WM_INITDIALOG: {
            state = reinterpret_cast<DialogState*>(lParam);
            SetWindowLongPtrW(hwnd, DWLP_USER, lParam);
            state->hwnd = hwnd;
            state->tabs = GetDlgItem(hwnd, IDC_TABS);
            int index = 0;
            for (const wchar_t* name : kPages) {
                TCITEMW item{TCIF_TEXT};
                item.pszText = const_cast<wchar_t*>(name);
                SendMessageW(state->tabs, TCM_INSERTITEMW, static_cast<WPARAM>(index++), reinterpret_cast<LPARAM>(&item));
            }
            createControls(*state);
            loadControls(*state);
            showPage(*state, 0);
            SendMessageW(plugin::notepad(), NPPM_DARKMODESUBCLASSANDTHEME, NppDarkMode::dmfInit, reinterpret_cast<LPARAM>(hwnd));
            return TRUE;
        }
        case WM_NOTIFY: {
            const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header->idFrom == IDC_TABS && header->code == TCN_SELCHANGE) {
                showPage(*state, static_cast<int>(SendMessageW(state->tabs, TCM_GETCURSEL, 0, 0)));
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            bool second = false;
            Field* field = fieldFor(*state, static_cast<int>(wParam), second);
            if (!field || !field->color) break;
            const Color value = second ? *field->secondColor : *field->color;
            HBRUSH brush = CreateSolidBrush(value);
            FillRect(item->hDC, &item->rcItem, brush);
            DeleteObject(brush);
            FrameRect(item->hDC, &item->rcItem, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
            return TRUE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == IDOK) {
                applyEdits(*state);
                plugin::saveOptions();
                EndDialog(hwnd, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) {
                plugin::options() = state->original;
                plugin::optionsChanged();
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            if (id == IDC_RESET) {
                plugin::options() = Options{};
                loadControls(*state);
                plugin::optionsChanged();
                return TRUE;
            }
            bool second = false;
            Field* field = fieldFor(*state, id, second);
            if (!field || state->loading) break;
            switch (field->kind) {
                case Kind::Check:
                    if (code == BN_CLICKED) {
                        field->set(SendMessageW(field->control, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        plugin::optionsChanged();
                    }
                    break;
                case Kind::Choice:
                    if (code == CBN_SELCHANGE) {
                        field->set(static_cast<int>(SendMessageW(field->control, CB_GETCURSEL, 0, 0)));
                        plugin::optionsChanged();
                    }
                    break;
                case Kind::Number:
                case Kind::Text:
                    if (code == EN_KILLFOCUS) {
                        readField(*field);
                        plugin::optionsChanged();
                    }
                    break;
                case Kind::Font:
                    if (code == BN_CLICKED) pickFont(*state);
                    break;
                case Kind::Color:
                case Kind::ColorPair:
                    if (code == BN_CLICKED) pickColor(*state, second ? *field->secondColor : *field->color);
                    break;
            }
            return TRUE;
        }
        default:
            break;
    }
    return FALSE;
}

}  // namespace

namespace plugin {

void showOptionsDialog(HWND owner) {
    DialogState state;
    state.original = options();
    state.fields = buildFields(options());
    DialogBoxParamW(instance(), MAKEINTRESOURCEW(IDD_OPTIONS), owner, dialogProc, reinterpret_cast<LPARAM>(&state));
}

}  // namespace plugin
