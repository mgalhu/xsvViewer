// The column manager, and the small number prompt used by the column menu.
#include <windows.h>

#include <commctrl.h>

#include <string>
#include <vector>

#include "Document.h"
#include "Notepad_plus_msgs.h"
#include "Plugin.h"
#include "Text.h"
#include "resource.h"

namespace {

struct ColumnsState {
    Document* document = nullptr;
    HWND list = nullptr;
    bool filling = false;
};

std::wstring searchText(HWND dialog) {
    wchar_t buffer[256] = {};
    GetDlgItemTextW(dialog, IDC_COLUMN_SEARCH, buffer, 256);
    return buffer;
}

void fillList(HWND dialog, ColumnsState& state, int selectFileColumn) {
    Document& document = *state.document;
    const std::string needle = xsv::foldCase(plugin::narrow(searchText(dialog)));
    const auto& names = document.table().columnNames();

    state.filling = true;
    SendMessageW(state.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(state.list);
    int row = 0;
    for (const int column : document.layout().order) {
        const std::string& name = names[static_cast<size_t>(column)];
        if (!needle.empty() && xsv::foldCase(name).find(needle) == std::string::npos) continue;
        const std::wstring label = plugin::widen(name);
        LVITEMW item{LVIF_TEXT | LVIF_PARAM};
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(label.c_str());
        item.lParam = column;
        ListView_InsertItem(state.list, &item);
        ListView_SetCheckState(state.list, row, !document.layout().columns[static_cast<size_t>(column)].hidden);
        if (column == selectFileColumn) {
            ListView_SetItemState(state.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(state.list, row, FALSE);
        }
        ++row;
    }
    ListView_SetColumnWidth(state.list, 0, LVSCW_AUTOSIZE_USEHEADER);
    SendMessageW(state.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state.list, nullptr, TRUE);
    state.filling = false;
}

int selectedFileColumn(const ColumnsState& state) {
    const int row = ListView_GetNextItem(state.list, -1, LVNI_SELECTED);
    if (row < 0) return -1;
    LVITEMW item{LVIF_PARAM};
    item.iItem = row;
    ListView_GetItem(state.list, &item);
    return static_cast<int>(item.lParam);
}

void moveSelected(HWND dialog, ColumnsState& state, int direction) {
    if (!searchText(dialog).empty()) return;  // positions mean nothing in a filtered list
    const int column = selectedFileColumn(state);
    if (column < 0) return;
    const auto& order = state.document->layout().order;
    const size_t index = static_cast<size_t>(std::find(order.begin(), order.end(), column) - order.begin());
    const long long target = static_cast<long long>(index) + direction;
    if (target < 0 || target >= static_cast<long long>(order.size())) return;
    state.document->moveInOrder(index, static_cast<size_t>(target));
    fillList(dialog, state, column);
}

INT_PTR CALLBACK columnsProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    ColumnsState* state = reinterpret_cast<ColumnsState*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    switch (message) {
        case WM_INITDIALOG: {
            state = reinterpret_cast<ColumnsState*>(lParam);
            SetWindowLongPtrW(hwnd, DWLP_USER, lParam);
            state->list = GetDlgItem(hwnd, IDC_COLUMN_LIST);
            ListView_SetExtendedListViewStyle(state->list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            LVCOLUMNW column{LVCF_WIDTH};
            column.cx = 200;
            ListView_InsertColumn(state->list, 0, &column);
            SendDlgItemMessageW(hwnd, IDC_COLUMN_SEARCH, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search columns"));
            fillList(hwnd, *state, -1);
            SendMessageW(plugin::notepad(), NPPM_DARKMODESUBCLASSANDTHEME, NppDarkMode::dmfInit, reinterpret_cast<LPARAM>(hwnd));
            return TRUE;
        }
        case WM_NOTIFY: {
            const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header->idFrom != IDC_COLUMN_LIST || state->filling) break;
            if (header->code == LVN_ITEMCHANGED) {
                const NMLISTVIEW* change = reinterpret_cast<const NMLISTVIEW*>(lParam);
                if ((change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK)) {
                    const bool checked = ListView_GetCheckState(state->list, change->iItem) != 0;
                    state->document->setHidden(static_cast<size_t>(change->lParam), !checked);
                }
            } else if (header->code == NM_DBLCLK) {
                const int column = selectedFileColumn(*state);
                if (column >= 0) {
                    state->document->requestRevealColumn(static_cast<size_t>(column));
                    EndDialog(hwnd, IDOK);
                }
            }
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK:
                case IDCANCEL:
                    EndDialog(hwnd, IDOK);
                    return TRUE;
                case IDC_COLUMN_SEARCH:
                    if (HIWORD(wParam) == EN_CHANGE) fillList(hwnd, *state, -1);
                    return TRUE;
                case IDC_COLUMN_SHOW_ALL:
                    state->document->setAllHidden(false);
                    fillList(hwnd, *state, -1);
                    return TRUE;
                case IDC_COLUMN_HIDE_ALL:
                    state->document->setAllHidden(true);
                    fillList(hwnd, *state, -1);
                    return TRUE;
                case IDC_COLUMN_UP:
                    moveSelected(hwnd, *state, -1);
                    return TRUE;
                case IDC_COLUMN_DOWN:
                    moveSelected(hwnd, *state, 1);
                    return TRUE;
                default:
                    break;
            }
            break;
        default:
            break;
    }
    return FALSE;
}

struct PromptState {
    const wchar_t* label;
    int* value;
};

INT_PTR CALLBACK promptProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    PromptState* state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    switch (message) {
        case WM_INITDIALOG:
            state = reinterpret_cast<PromptState*>(lParam);
            SetWindowLongPtrW(hwnd, DWLP_USER, lParam);
            SetDlgItemTextW(hwnd, IDC_PROMPT_LABEL, state->label);
            SetDlgItemInt(hwnd, IDC_PROMPT_VALUE, static_cast<UINT>(*state->value), FALSE);
            SendDlgItemMessageW(hwnd, IDC_PROMPT_VALUE, EM_SETSEL, 0, -1);
            SendMessageW(plugin::notepad(), NPPM_DARKMODESUBCLASSANDTHEME, NppDarkMode::dmfInit, reinterpret_cast<LPARAM>(hwnd));
            SetFocus(GetDlgItem(hwnd, IDC_PROMPT_VALUE));
            return FALSE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                BOOL ok = FALSE;
                const UINT value = GetDlgItemInt(hwnd, IDC_PROMPT_VALUE, &ok, FALSE);
                if (ok) *state->value = static_cast<int>(value);
                EndDialog(hwnd, ok);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(hwnd, FALSE);
                return TRUE;
            }
            break;
        default:
            break;
    }
    return FALSE;
}

}  // namespace

namespace plugin {

void showColumnsDialog(Document& document, HWND owner) {
    ColumnsState state;
    state.document = &document;
    DialogBoxParamW(instance(), MAKEINTRESOURCEW(IDD_COLUMNS), owner, columnsProc, reinterpret_cast<LPARAM>(&state));
}

bool promptNumber(HWND owner, const wchar_t* label, int& value) {
    PromptState state{label, &value};
    return DialogBoxParamW(instance(), MAKEINTRESOURCEW(IDD_PROMPT), owner, promptProc, reinterpret_cast<LPARAM>(&state)) != FALSE;
}

}  // namespace plugin
