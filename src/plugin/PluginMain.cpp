// Notepad++ integration: the plugin exports, the menu, and keeping tables in step with the editor.
#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

#include "Docking.h"
#include "Document.h"
#include "Grid.h"
#include "Ini.h"
#include "Options.h"
#include "Plugin.h"
#include "PluginInterface.h"
#include "TableHost.h"
#include "menuCmdID.h"

using namespace xsv;

namespace {

constexpr wchar_t kPluginName[] = L"xsvViewer++";
constexpr wchar_t kModuleName[] = L"xsvViewer.dll";
constexpr wchar_t kGuideUrl[] = L"https://github.com/mgalhu/xsvViewer/blob/main/docs/USER_GUIDE.md";
constexpr wchar_t kPropEdit[] = L"xsvViewerEdit";
constexpr wchar_t kPropGrid[] = L"xsvViewerGrid";
constexpr size_t kMaxRememberedFiles = 500;
constexpr UINT kRefreshDelayMs = 300;
constexpr UINT kRefreshDelayLargeMs = 2000;
constexpr size_t kLargeDocument = 32 * 1024 * 1024;

// Notepad++ stores CmdPanel's index to reopen the panel, so that one must keep its position.
enum CommandIndex {
    CmdToggle, CmdPanel, CmdSeparator1, CmdColumns, CmdOptions, CmdSeparator2,
    CmdExportCsv, CmdExportTsv, CmdExportMarkdown, CmdSeparator3, CmdReset, CmdGuide, CmdAbout, CmdCount
};

struct EditorView {
    HWND scintilla = nullptr;
    std::unique_ptr<TableHost> host;
    bool covered = false;
};

HINSTANCE g_instance = nullptr;
NppData g_npp;
FuncItem g_items[CmdCount];
ShortcutKey g_toggleKey{true, true, false, 'T'};
ShortcutKey g_panelKey{true, true, true, 'T'};

Options g_options;
Ini g_layouts;
std::string g_parsingKey;
std::wstring g_configDir;
bool g_dark = false;
bool g_ready = false;
bool g_initialized = false;
bool g_shuttingDown = false;

std::map<UINT_PTR, std::unique_ptr<Document>> g_documents;
EditorView g_views[2];
std::unique_ptr<TableHost> g_panel;
bool g_panelVisible = false;
HHOOK g_hook = nullptr;
UINT_PTR g_refreshTimer = 0;

LRESULT npp(UINT message, WPARAM wParam = 0, LPARAM lParam = 0) {
    return SendMessageW(g_npp._nppHandle, message, wParam, lParam);
}

LRESULT sci(HWND scintilla, UINT message, WPARAM wParam = 0, LPARAM lParam = 0) {
    return SendMessageW(scintilla, message, wParam, lParam);
}

int activeView() { return static_cast<int>(npp(NPPM_GETCURRENTVIEW)) == SUB_VIEW ? SUB_VIEW : MAIN_VIEW; }

std::wstring pathOf(UINT_PTR buffer) {
    const LRESULT length = npp(NPPM_GETFULLPATHFROMBUFFERID, buffer, 0);
    if (length <= 0) return {};
    std::wstring path(static_cast<size_t>(length) + 1, L'\0');
    npp(NPPM_GETFULLPATHFROMBUFFERID, buffer, reinterpret_cast<LPARAM>(path.data()));
    path.resize(static_cast<size_t>(length));
    return path;
}

// ---- Settings on disk ------------------------------------------------------------------------

std::string readFile(const std::wstring& path) {
    std::ifstream file(path.c_str(), std::ios::binary);
    std::stringstream content;
    content << file.rdbuf();
    return content.str();
}

void writeFile(const std::wstring& path, const std::string& content) {
    std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::wstring optionsPath() { return g_configDir + L"\\xsvViewer.ini"; }
std::wstring layoutsPath() { return g_configDir + L"\\xsvViewer.layouts.ini"; }

std::string sectionFor(const Document& document) {
    std::wstring path = document.path();
    std::transform(path.begin(), path.end(), path.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return plugin::narrow(path);
}

bool isFileOnDisk(const Document& document) { return document.path().find_first_of(L"\\/") != std::wstring::npos; }

void rememberLayout(Document& document) {
    if (!g_options.rememberLayout || !document.layoutDirty() || !isFileOnDisk(document) || !document.loaded()) return;
    const std::string section = sectionFor(document);
    g_layouts.removeSection(section);   // re-adding moves it to the end, the most recent place
    saveLayout(document.layout(), g_layouts, section);
    document.markLayoutSaved();

    const std::vector<std::string> sections = g_layouts.sections();
    for (size_t i = 0; i + kMaxRememberedFiles < sections.size(); ++i) g_layouts.removeSection(sections[i]);
}

// ---- Which documents exist -------------------------------------------------------------------

bool matchesAutoOpen(const Document& document) {
    if (!g_options.autoOpen || !isFileOnDisk(document)) return false;
    const std::string extension = document.extension();
    if (extension.empty()) return false;
    std::string_view list = g_options.extensions;
    while (!list.empty()) {
        const size_t split = list.find(';');
        std::string entry(list.substr(0, split));
        std::transform(entry.begin(), entry.end(), entry.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (entry == extension) return true;
        if (split == std::string_view::npos) break;
        list.remove_prefix(split + 1);
    }
    return false;
}

// create == false still creates the entry for a file that opens as a table by itself.
Document* documentFor(UINT_PTR buffer, bool create) {
    const auto found = g_documents.find(buffer);
    if (found != g_documents.end()) return found->second.get();

    auto document = std::make_unique<Document>(buffer);
    document->setPath(pathOf(buffer));
    const bool automatic = matchesAutoOpen(*document);
    if (!automatic && !create) return nullptr;

    document->tableMode = automatic;
    DocLayout layout;
    if (g_options.rememberLayout && isFileOnDisk(*document) && loadLayout(g_layouts, sectionFor(*document), layout))
        document->layout() = std::move(layout);
    return g_documents.emplace(buffer, std::move(document)).first->second.get();
}

UINT_PTR bufferInView(int view) {
    const int index = static_cast<int>(npp(NPPM_GETCURRENTDOCINDEX, 0, view));
    return index < 0 ? 0 : static_cast<UINT_PTR>(npp(NPPM_GETBUFFERIDFROMPOS, static_cast<WPARAM>(index), view));
}

// ---- Keeping the editor and the overlays together --------------------------------------------

void placeOverlay(EditorView& view) {
    if (!view.host || !view.host->hwnd()) return;
    RECT rect;
    GetWindowRect(view.scintilla, &rect);
    MapWindowPoints(HWND_DESKTOP, GetParent(view.scintilla), reinterpret_cast<POINT*>(&rect), 2);
    SetWindowPos(view.host->hwnd(), HWND_TOP, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE);
}

void lockDocument(Document& document, HWND scintilla) {
    if (!document.readOnlyApplied) {
        document.wasReadOnly = sci(scintilla, SCI_GETREADONLY) != 0;
        document.readOnlyApplied = true;
    }
    sci(scintilla, SCI_SETREADONLY, TRUE);
}

void unlockDocument(Document& document, HWND scintilla) {
    if (!document.readOnlyApplied) return;
    sci(scintilla, SCI_SETREADONLY, document.wasReadOnly ? TRUE : FALSE);
    document.readOnlyApplied = false;
}

void cover(EditorView& view, Document& document) {
    const HCURSOR previous = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    document.readFrom(view.scintilla);
    SetCursor(previous);
    lockDocument(document, view.scintilla);
    view.host->attach(&document);
    view.covered = true;
    placeOverlay(view);
    ShowWindow(view.host->hwnd(), SW_SHOWNA);
    if (GetFocus() == view.scintilla) SetFocus(view.host->grid().hwnd());
}

void uncover(EditorView& view) {
    if (!view.covered) return;
    view.covered = false;
    const HWND focus = GetFocus();
    const bool hadFocus = view.host->hwnd() == focus || IsChild(view.host->hwnd(), focus);
    ShowWindow(view.host->hwnd(), SW_HIDE);
    view.host->attach(nullptr);
    InvalidateRect(view.scintilla, nullptr, TRUE);
    if (hadFocus) SetFocus(view.scintilla);
}

void syncView(int index) {
    EditorView& view = g_views[index];
    if (!view.host) return;
    const UINT_PTR buffer = IsWindowVisible(view.scintilla) ? bufferInView(index) : 0;
    Document* document = buffer ? documentFor(buffer, false) : nullptr;
    if (document && document->tableMode) cover(view, *document);
    else uncover(view);
}

void syncPanel() {
    if (!g_panel || !g_panelVisible) return;
    const int view = activeView();
    const UINT_PTR buffer = bufferInView(view);
    Document* document = buffer ? documentFor(buffer, true) : nullptr;
    if (document) document->readFrom(g_views[view].scintilla);
    g_panel->attach(document);
}

void updateMenuChecks() {
    const UINT_PTR buffer = bufferInView(activeView());
    const auto found = g_documents.find(buffer);
    npp(NPPM_SETMENUITEMCHECK, g_items[CmdToggle]._cmdID, found != g_documents.end() && found->second->tableMode);
    npp(NPPM_SETMENUITEMCHECK, g_items[CmdPanel]._cmdID, g_panelVisible);
}

void syncAll() {
    if (!g_ready || g_shuttingDown) return;
    syncView(MAIN_VIEW);
    syncView(SUB_VIEW);
    syncPanel();
    updateMenuChecks();
}

LRESULT CALLBACK scintillaProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    EditorView& view = *reinterpret_cast<EditorView*>(data);
    switch (message) {
        case WM_PAINT:
            if (view.covered) {
                ValidateRect(hwnd, nullptr);  // it could otherwise draw over the table
                return 0;
            }
            break;
        case WM_SETFOCUS:
            if (view.covered) {
                SetFocus(view.host->grid().hwnd());
                return 0;
            }
            break;
        case WM_WINDOWPOSCHANGED: {
            const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
            if (view.covered) {
                placeOverlay(view);
                ShowWindow(view.host->hwnd(), IsWindowVisible(hwnd) ? SW_SHOWNA : SW_HIDE);
            }
            return result;
        }
        default:
            break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void moveCaretTo(HWND scintilla, size_t offset) {
    sci(scintilla, SCI_GOTOPOS, offset);
    sci(scintilla, SCI_SCROLLCARET);
}

size_t offsetOfCell(const Document& document, size_t row, size_t column) {
    const Table& table = document.table();
    if (row >= table.rowCount()) return 0;
    std::vector<Cell> cells;
    table.readRow(row, cells);
    return column < cells.size() ? cells[column].offset : table.rowOffset(row);
}

void selectCaretCell(Document& document, HWND scintilla) {
    size_t row = 0, column = 0;
    const size_t caret = static_cast<size_t>(sci(scintilla, SCI_GETCURRENTPOS));
    if (document.table().locate(caret, row, column) && document.revealRow(row)) document.requestRevealColumn(column);
}

void leaveTable(Document& document, int index, bool keepCursorCell) {
    EditorView& view = g_views[index];
    size_t offset = SIZE_MAX;
    if (keepCursorCell && document.viewRows() && document.visibleColumns()) {
        offset = offsetOfCell(document, document.sourceRow(document.selection.cursorRow),
                              document.fileColumn(document.selection.cursorColumn));
    }
    document.tableMode = false;
    unlockDocument(document, view.scintilla);
    uncover(view);
    if (offset != SIZE_MAX) moveCaretTo(view.scintilla, offset);
    SetFocus(view.scintilla);
}

// ---- Plugin lifetime -------------------------------------------------------------------------

// A menu reads the same message queue, with focus still on the grid behind it.
bool menuIsOpen() {
    GUITHREADINFO info{sizeof info};
    return GetGUIThreadInfo(GetCurrentThreadId(), &info) && (info.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE));
}

LRESULT CALLBACK messageHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == PM_REMOVE) {
        MSG* msg = reinterpret_cast<MSG*>(lParam);
        if (msg->message == WM_KEYDOWN && !menuIsOpen()) {
            const HWND focus = GetFocus();
            const bool ctrl = GetKeyState(VK_CONTROL) < 0;
            const bool shift = GetKeyState(VK_SHIFT) < 0;
            const bool alt = GetKeyState(VK_MENU) < 0;
            if (Grid* grid = static_cast<Grid*>(GetPropW(focus, kPropGrid))) {
                if (!alt && grid->handleKey(static_cast<UINT>(msg->wParam), ctrl, shift)) msg->message = WM_NULL;
            } else if (GetPropW(focus, kPropEdit)) {
                // Text fields keep their own Ctrl+C/V/X/Z/A instead of Notepad++'s commands.
                const bool notepadKey = (ctrl && msg->wParam == VK_TAB) || (msg->wParam >= VK_F1 && msg->wParam <= VK_F24) || alt;
                if (!notepadKey) {
                    TranslateMessage(msg);
                    DispatchMessageW(msg);
                    msg->message = WM_NULL;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void applyTheme() {
    g_dark = g_options.theme == Theme::Dark ||
             (g_options.theme == Theme::FollowNotepad && npp(NPPM_ISDARKMODEENABLED) != 0);
}

void initialize() {
    if (g_initialized) return;
    g_initialized = true;

    wchar_t directory[MAX_PATH] = {};
    npp(NPPM_GETPLUGINSCONFIGDIR, MAX_PATH, reinterpret_cast<LPARAM>(directory));
    g_configDir = directory;
    CreateDirectoryW(g_configDir.c_str(), nullptr);

    Ini ini;
    ini.parse(readFile(optionsPath()));
    loadOptions(ini, g_options);
    g_layouts.parse(readFile(layoutsPath()));
    applyTheme();

    Grid::registerClass();
    TableHost::registerClass();

    g_views[MAIN_VIEW].scintilla = g_npp._scintillaMainHandle;
    g_views[SUB_VIEW].scintilla = g_npp._scintillaSecondHandle;
    for (EditorView& view : g_views) {
        view.host = std::make_unique<TableHost>(TableHost::Mode::InTab);
        view.host->create(GetParent(view.scintilla));
        view.host->optionsChanged();
        npp(NPPM_DARKMODESUBCLASSANDTHEME, NppDarkMode::dmfInit, reinterpret_cast<LPARAM>(view.host->hwnd()));
        SetWindowLongPtrW(view.scintilla, GWL_STYLE, GetWindowLongPtrW(view.scintilla, GWL_STYLE) | WS_CLIPSIBLINGS);
        SetWindowSubclass(view.scintilla, scintillaProc, 1, reinterpret_cast<DWORD_PTR>(&view));
    }
    g_hook = SetWindowsHookExW(WH_GETMESSAGE, messageHook, nullptr, GetCurrentThreadId());
}

void shutdown() {
    g_shuttingDown = true;
    if (g_refreshTimer) KillTimer(nullptr, g_refreshTimer);
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;

    for (auto& [buffer, document] : g_documents) rememberLayout(*document);
    plugin::saveOptions();
    writeFile(layoutsPath(), g_layouts.serialize());

    for (EditorView& view : g_views) {
        if (view.scintilla) RemoveWindowSubclass(view.scintilla, scintillaProc, 1);
        if (view.host) view.host->attach(nullptr);
    }
    if (g_panel) g_panel->attach(nullptr);
}

void refreshTables() {
    if (g_refreshTimer) KillTimer(nullptr, g_refreshTimer);
    g_refreshTimer = 0;
    if (g_shuttingDown) return;
    for (EditorView& view : g_views) {
        Document* document = view.covered ? view.host->document() : nullptr;
        if (!document) continue;
        if (g_options.autoRefresh) document->readFrom(view.scintilla);
        lockDocument(*document, view.scintilla);  // a reload from disk clears the flag
    }
    if (g_options.autoRefresh) syncPanel();
}

void CALLBACK refreshTimerProc(HWND, UINT, UINT_PTR, DWORD) { refreshTables(); }

void scheduleRefresh(HWND scintilla) {
    const size_t length = static_cast<size_t>(sci(scintilla, SCI_GETLENGTH));
    g_refreshTimer = SetTimer(nullptr, g_refreshTimer, length > kLargeDocument ? kRefreshDelayLargeMs : kRefreshDelayMs, refreshTimerProc);
}

EditorView* viewOf(HWND scintilla) {
    for (EditorView& view : g_views) {
        if (view.scintilla == scintilla) return &view;
    }
    return nullptr;
}

void forgetDocument(UINT_PTR buffer) {
    const auto found = g_documents.find(buffer);
    if (found == g_documents.end()) return;
    rememberLayout(*found->second);
    writeFile(layoutsPath(), g_layouts.serialize());
    for (EditorView& view : g_views) {
        if (view.host && view.host->document() == found->second.get()) {
            // A clone may live on in the other view, and the lock is on the shared text.
            unlockDocument(*found->second, view.scintilla);
            uncover(view);
        }
    }
    if (g_panel && g_panel->document() == found->second.get()) g_panel->attach(nullptr);
    g_documents.erase(found);
}

// ---- Menu commands ---------------------------------------------------------------------------

void commandToggle() { plugin::toggleTable(); }

void commandPanel() {
    initialize();
    if (!g_panel) {
        g_panel = std::make_unique<TableHost>(TableHost::Mode::Panel);
        g_panel->create(g_npp._nppHandle);
        g_panel->optionsChanged();

        static DockedWidgetData data;
        data.hClient = g_panel->hwnd();
        data.pszName = kPluginName;
        data.dlgID = CmdPanel;
        data.uMask = DWS_DF_CONT_BOTTOM;
        data.pszModuleName = kModuleName;
        npp(NPPM_DMMREGASDCKDLG, 0, reinterpret_cast<LPARAM>(&data));
        g_panelVisible = true;
    } else {
        g_panelVisible = !g_panelVisible;
        npp(g_panelVisible ? NPPM_DMMSHOW : NPPM_DMMHIDE, 0, reinterpret_cast<LPARAM>(g_panel->hwnd()));
    }
    syncPanel();
    updateMenuChecks();

    // The panel accompanies the text, so typing carries on where it was.
    EditorView& view = g_views[activeView()];
    if (g_ready && !view.covered) SetFocus(view.scintilla);
}

Document* activeDocument() {
    const int view = activeView();
    const UINT_PTR buffer = bufferInView(view);
    Document* document = buffer ? documentFor(buffer, true) : nullptr;
    if (document) document->readFrom(g_views[view].scintilla);
    return document;
}

void commandColumns() {
    if (Document* document = activeDocument()) plugin::showColumnsDialog(*document, g_npp._nppHandle);
}

void commandOptions() { plugin::showOptionsDialog(g_npp._nppHandle); }

void commandExport(ExportFormat format) {
    if (Document* document = activeDocument()) plugin::exportView(*document, format);
}
void commandExportCsv() { commandExport(ExportFormat::Csv); }
void commandExportTsv() { commandExport(ExportFormat::Tsv); }
void commandExportMarkdown() { commandExport(ExportFormat::Markdown); }
void commandReset() { plugin::resetLayout(); }
void commandGuide() { ShellExecuteW(g_npp._nppHandle, L"open", kGuideUrl, nullptr, nullptr, SW_SHOWNORMAL); }

void commandAbout() {
    MessageBoxW(g_npp._nppHandle,
                L"xsvViewer++ 1.0.1\n\n"
                L"Shows delimited text files as an adjustable table.\n\n"
                L"Ctrl+Alt+T\ttable / text\n"
                L"Ctrl+Alt+Shift+T\tdocked panel\n"
                L"Ctrl+F\tfilter rows\n"
                L"Ctrl+Shift+L\tcolumn filters\n"
                L"Enter\tshow the cell in the text\n"
                L"Ctrl+C\tcopy,  Ctrl+Shift+C  with header\n"
                L"Ctrl+wheel\tzoom\n\n"
                L"Right-click a column header for its settings.\n"
                L"Plugins > xsvViewer++ > User Guide has everything else.\n\n"
                L"Copyright (C) 2026 Markus Galhuber. GPL-3.0.",
                kPluginName, MB_OK | MB_ICONINFORMATION);
}

void addCommand(CommandIndex index, const wchar_t* name, PFUNCPLUGINCMD function, ShortcutKey* key = nullptr) {
    FuncItem& item = g_items[index];
    wcsncpy_s(item._itemName, name, _TRUNCATE);
    item._pFunc = function;
    item._pShKey = key;
}

}  // namespace

// ---- plugin:: API ----------------------------------------------------------------------------

namespace plugin {

HINSTANCE instance() { return g_instance; }
HWND notepad() { return g_npp._nppHandle; }
Options& options() { return g_options; }
const Palette& palette() { return g_dark ? g_options.dark : g_options.light; }
bool darkMode() { return g_dark; }

void optionsChanged() {
    applyTheme();

    // Most options only change how things are drawn. Zooming must not re-read every open file.
    Ini parsing;
    xsv::Options comparable;
    comparable.parse = g_options.parse;
    comparable.missingTokens = g_options.missingTokens;
    comparable.naturalSort = g_options.naturalSort;
    comparable.caseSensitiveFilter = g_options.caseSensitiveFilter;
    xsv::saveOptions(comparable, parsing);
    std::string key = parsing.serialize();
    if (key != g_parsingKey) {
        g_parsingKey = std::move(key);
        for (auto& [buffer, document] : g_documents) document->refreshForOptions();
    }

    for (EditorView& view : g_views) {
        if (view.host) view.host->optionsChanged();
    }
    if (g_panel) g_panel->optionsChanged();
}

void saveOptions() {
    if (g_configDir.empty()) return;
    Ini ini;
    xsv::saveOptions(g_options, ini);
    writeFile(optionsPath(), ini.serialize());
}

void toggleTable() {
    initialize();
    const int index = activeView();
    const UINT_PTR buffer = bufferInView(index);
    if (!buffer) return;
    Document* document = documentFor(buffer, true);
    HWND scintilla = g_views[index].scintilla;

    if (document->tableMode) {
        leaveTable(*document, index, true);
    } else {
        document->tableMode = true;
        document->readFrom(scintilla);
        cover(g_views[index], *document);
        selectCaretCell(*document, scintilla);
        SetFocus(g_views[index].host->grid().hwnd());
    }
    syncAll();  // a clone of this document in the other view changes with it
}

void showCellInText(Document& document, size_t row, size_t column) {
    const size_t offset = offsetOfCell(document, row, column);
    for (int index : {MAIN_VIEW, SUB_VIEW}) {
        if (bufferInView(index) != document.buffer()) continue;
        if (document.tableMode) {
            document.tableMode = false;
            unlockDocument(document, g_views[index].scintilla);
            uncover(g_views[index]);
        }
        moveCaretTo(g_views[index].scintilla, offset);
        SetFocus(g_views[index].scintilla);
        break;
    }
    syncAll();
}

void exportView(Document& document, ExportFormat format) {
    const size_t rows = document.viewRows();
    const std::string text = exportTable(document.table(), document.viewOrNull(), 0, rows ? rows - 1 : 0, document.visibleList(), format, true);
    const std::wstring wide = document.decode(text);

    npp(NPPM_MENUCOMMAND, 0, IDM_FILE_NEW);
    HWND scintilla = g_views[activeView()].scintilla;

    // The new tab has the encoding the user set for new documents; the text is written in that.
    const UINT codePage = static_cast<UINT>(sci(scintilla, SCI_GETCODEPAGE));
    const UINT target = codePage ? codePage : CP_ACP;
    const int length = WideCharToMultiByte(target, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string converted(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(target, 0, wide.data(), static_cast<int>(wide.size()), converted.data(), length, nullptr, nullptr);
    sci(scintilla, SCI_ADDTEXT, converted.size(), reinterpret_cast<LPARAM>(converted.data()));
    sci(scintilla, SCI_GOTOPOS, 0);
}

void resetLayout() {
    initialize();
    Document* document = activeDocument();
    if (!document) return;
    g_layouts.removeSection(sectionFor(*document));
    document->resetLayout();
    document->markLayoutSaved();
}

void panelClosed() {
    g_panelVisible = false;
    if (g_panel) g_panel->attach(nullptr);
    updateMenuChecks();
}

void activateViewOf(HWND grid) {
    for (int index : {MAIN_VIEW, SUB_VIEW}) {
        const EditorView& view = g_views[index];
        if (view.host && view.host->grid().hwnd() == grid && activeView() != index) {
            // IDM_VIEW_SWITCHTO_OTHER_VIEW would not do: it only switches with focus in the editor.
            npp(NPPM_ACTIVATEDOC, index, npp(NPPM_GETCURRENTDOCINDEX, 0, index));
            return;
        }
    }
}

void registerEdit(HWND edit) { SetPropW(edit, kPropEdit, reinterpret_cast<HANDLE>(1)); }

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
    return out;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), length, nullptr, nullptr);
    return out;
}

}  // namespace plugin

// ---- Notepad++ exports -----------------------------------------------------------------------

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_instance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void setInfo(NppData data) {
    g_npp = data;
    addCommand(CmdToggle, L"Table / Text", commandToggle, &g_toggleKey);
    addCommand(CmdPanel, L"Table Panel", commandPanel, &g_panelKey);
    addCommand(CmdSeparator1, L"", nullptr);
    addCommand(CmdColumns, L"Columns...", commandColumns);
    addCommand(CmdOptions, L"Options...", commandOptions);
    addCommand(CmdSeparator2, L"", nullptr);
    addCommand(CmdExportCsv, L"Export View as CSV", commandExportCsv);
    addCommand(CmdExportTsv, L"Export View as TSV", commandExportTsv);
    addCommand(CmdExportMarkdown, L"Export View as Markdown", commandExportMarkdown);
    addCommand(CmdSeparator3, L"", nullptr);
    addCommand(CmdReset, L"Reset Layout of This File", commandReset);
    addCommand(CmdGuide, L"User Guide", commandGuide);
    addCommand(CmdAbout, L"About", commandAbout);
}

extern "C" __declspec(dllexport) const wchar_t* getName() { return kPluginName; }

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* count) {
    *count = CmdCount;
    return g_items;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* notification) {
    const auto& header = notification->nmhdr;
    switch (header.code) {
        case NPPN_READY:
            initialize();
            g_ready = true;
            syncAll();
            break;
        case NPPN_BUFFERACTIVATED:
            syncAll();
            break;
        case NPPN_FILERENAMED:
        case NPPN_FILESAVED: {  // Save As changes the path too
            const auto found = g_documents.find(header.idFrom);
            if (found != g_documents.end()) found->second->setPath(pathOf(header.idFrom));
            break;
        }
        case NPPN_FILEBEFORECLOSE:
            forgetDocument(header.idFrom);
            break;
        case NPPN_READONLYCHANGED: {
            const auto found = g_documents.find(reinterpret_cast<UINT_PTR>(header.hwndFrom));  // buffer, not a window
            if (found == g_documents.end() || !found->second->tableMode) break;
            // Notepad++ just rewrote the flag; remember the user's choice and keep the table locked.
            found->second->wasReadOnly = (header.idFrom & DOCSTATUS_READONLY) != 0;
            for (EditorView& view : g_views) {
                if (view.covered && view.host->document() == found->second.get()) sci(view.scintilla, SCI_SETREADONLY, TRUE);
            }
            break;
        }
        case NPPN_DARKMODECHANGED:
            plugin::optionsChanged();
            for (EditorView& view : g_views) {
                if (view.host) npp(NPPM_DARKMODESUBCLASSANDTHEME, NppDarkMode::dmfHandleChange, reinterpret_cast<LPARAM>(view.host->hwnd()));
            }
            break;
        case NPPN_SHUTDOWN:
            shutdown();
            break;
        case NPPN_GLOBALMODIFIED:  // Replace All, which sends no SCN_MODIFIED
            if (g_ready) scheduleRefresh(g_views[activeView()].scintilla);
            break;
        case SCN_MODIFIED:
            if (g_ready && (notification->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) {
                if (viewOf(static_cast<HWND>(header.hwndFrom))) scheduleRefresh(static_cast<HWND>(header.hwndFrom));
            }
            break;
        case SCN_UPDATEUI:
            if (g_ready && g_panel && g_panelVisible && g_options.followCaret && (notification->updated & SC_UPDATE_SELECTION)) {
                EditorView* view = viewOf(static_cast<HWND>(header.hwndFrom));
                Document* document = g_panel->document();
                if (view && document && !document->tableMode && bufferInView(activeView()) == document->buffer())
                    selectCaretCell(*document, view->scintilla);
            }
            break;
        default:
            break;
    }
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }

extern "C" __declspec(dllexport) BOOL isUnicode() { return TRUE; }
