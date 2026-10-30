// The plugin as the rest of the UI sees it.
#pragma once
#include <windows.h>

#include <string>

#include "Export.h"
#include "Options.h"

class Document;

namespace plugin {

HINSTANCE instance();
HWND notepad();

xsv::Options& options();
const xsv::Palette& palette();
bool darkMode();

void optionsChanged();   // options() was modified: repaint everything, keep it for next time
void saveOptions();

void toggleTable();
void showCellInText(Document& document, size_t row, size_t column);
void exportView(Document& document, xsv::ExportFormat format);
void resetLayout();
void panelClosed();
void activateViewOf(HWND grid);  // the view holding this grid becomes Notepad++'s active view

void showOptionsDialog(HWND owner);
void showColumnsDialog(Document& document, HWND owner);
bool promptNumber(HWND owner, const wchar_t* label, int& value);

// Marks a child edit control whose keys must reach it instead of Notepad++'s accelerators.
void registerEdit(HWND edit);

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

}  // namespace plugin
