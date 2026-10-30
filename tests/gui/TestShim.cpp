// Test-only plugin, deployed to the portable test copy and never installed.
//
// Popup menus need real input, which a hidden desktop does not get, and the clipboard is shared
// with whatever the user is doing. So this patches xsvViewer.dll's imports: TrackPopupMenu picks
// the item named in shim_pick.txt, and clipboard writes land in shim_clipboard.txt. Everything
// up to those calls is the plugin's real code.
#include <windows.h>

#include <cstdio>
#include <string>

#include "PluginInterface.h"

namespace {

std::wstring g_directory;
FuncItem g_item;

std::wstring pathFor(const wchar_t* name) { return g_directory + L"\\" + name; }

std::wstring readPick() {
    std::wstring text;
    FILE* file = nullptr;
    if (_wfopen_s(&file, pathFor(L"shim_pick.txt").c_str(), L"rt, ccs=UTF-8") == 0 && file) {
        wchar_t buffer[256];
        if (fgetws(buffer, 256, file)) text = buffer;
        fclose(file);
    }
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) text.pop_back();
    return text;
}

UINT findItem(HMENU menu, const std::wstring& label, FILE* dump) {
    UINT found = 0;
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        wchar_t text[128] = {};
        MENUITEMINFOW info{sizeof info};
        info.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE | MIIM_SUBMENU | MIIM_FTYPE;
        info.dwTypeData = text;
        info.cch = 128;
        GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info);
        if (info.fType & MFT_SEPARATOR) continue;
        std::wstring name = text;
        name = name.substr(0, name.find(L'\t'));
        if (dump) fwprintf(dump, L"%u\t%s\t%s\n", info.wID, (info.fState & MFS_DISABLED) ? L"disabled" : L"enabled", name.c_str());
        if (info.hSubMenu) {
            const UINT nested = findItem(info.hSubMenu, label, dump);
            if (nested) found = nested;
        } else if (name == label && !(info.fState & MFS_DISABLED)) {
            found = info.wID;
        }
    }
    return found;
}

BOOL WINAPI fakeTrackPopupMenu(HMENU menu, UINT, int, int, int, HWND, const RECT*) {
    FILE* dump = nullptr;
    _wfopen_s(&dump, pathFor(L"shim_menu.txt").c_str(), L"wt, ccs=UTF-8");
    const UINT command = findItem(menu, readPick(), dump);
    if (dump) fclose(dump);
    DeleteFileW(pathFor(L"shim_pick.txt").c_str());
    return static_cast<BOOL>(command);
}

BOOL WINAPI fakeOpenClipboard(HWND) { return TRUE; }
BOOL WINAPI fakeEmptyClipboard() { return TRUE; }
BOOL WINAPI fakeCloseClipboard() { return TRUE; }

HANDLE WINAPI fakeSetClipboardData(UINT, HANDLE data) {
    const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data));
    FILE* file = nullptr;
    if (text && _wfopen_s(&file, pathFor(L"shim_clipboard.txt").c_str(), L"wb, ccs=UTF-8") == 0 && file) {
        fputws(text, file);
        fclose(file);
    }
    GlobalUnlock(data);
    return data;
}

void patchImport(HMODULE module, const char* function, void* replacement) {
    BYTE* base = reinterpret_cast<BYTE*>(module);
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto* headers = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* import = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; import->Name; ++import) {
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + import->OriginalFirstThunk);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + import->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(name->Name, function) != 0) continue;
            DWORD old;
            VirtualProtect(&addresses->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            addresses->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&addresses->u1.Function, sizeof(void*), old, &old);
        }
    }
}

void noCommand() {}

}  // namespace

BOOL APIENTRY DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

extern "C" __declspec(dllexport) void setInfo(NppData data) {
    wchar_t directory[MAX_PATH] = {};
    SendMessageW(data._nppHandle, NPPM_GETPLUGINSCONFIGDIR, MAX_PATH, reinterpret_cast<LPARAM>(directory));
    g_directory = directory;
    wcscpy_s(g_item._itemName, L"Test shim");
    g_item._pFunc = noCommand;
}

extern "C" __declspec(dllexport) const wchar_t* getName() { return L"xsvTestShim"; }

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* count) {
    *count = 1;
    return &g_item;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* notification) {
    if (notification->nmhdr.code != NPPN_READY) return;
    HMODULE viewer = GetModuleHandleW(L"xsvViewer.dll");
    if (!viewer) return;
    if (GetFileAttributesW(pathFor(L"shim_real_menus.txt").c_str()) == INVALID_FILE_ATTRIBUTES)
        patchImport(viewer, "TrackPopupMenu", reinterpret_cast<void*>(fakeTrackPopupMenu));
    patchImport(viewer, "OpenClipboard", reinterpret_cast<void*>(fakeOpenClipboard));
    patchImport(viewer, "EmptyClipboard", reinterpret_cast<void*>(fakeEmptyClipboard));
    patchImport(viewer, "CloseClipboard", reinterpret_cast<void*>(fakeCloseClipboard));
    patchImport(viewer, "SetClipboardData", reinterpret_cast<void*>(fakeSetClipboardData));
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }

extern "C" __declspec(dllexport) BOOL isUnicode() { return TRUE; }
