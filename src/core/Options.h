// Everything the user can adjust. Global options apply to every file; DocLayout is per file.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Dialect.h"
#include "Ini.h"

namespace xsv {

using Color = std::uint32_t;  // 0x00BBGGRR, same layout as COLORREF

constexpr Color rgb(int r, int g, int b) {
    return static_cast<Color>(r) | (static_cast<Color>(g) << 8) | (static_cast<Color>(b) << 16);
}

struct Palette {
    Color background, text, headerBackground, headerText, grid, zebra, selection, selectionText,
          cursor, gutterBackground, gutterText, frozenLine, missingText, accent;
};

constexpr Palette kLightPalette = {
    rgb(255, 255, 255), rgb(32, 33, 36),    rgb(240, 242, 245), rgb(32, 33, 36),    rgb(222, 225, 230),
    rgb(247, 248, 250), rgb(204, 228, 255), rgb(16, 24, 40),    rgb(26, 115, 232),  rgb(240, 242, 245),
    rgb(95, 99, 104),   rgb(150, 156, 165), rgb(160, 165, 172), rgb(26, 115, 232)};

constexpr Palette kDarkPalette = {
    rgb(32, 33, 36),    rgb(226, 227, 230), rgb(45, 47, 51),    rgb(232, 234, 237), rgb(60, 62, 67),
    rgb(38, 39, 43),    rgb(38, 73, 116),   rgb(240, 244, 250), rgb(102, 170, 255), rgb(45, 47, 51),
    rgb(154, 160, 166), rgb(110, 114, 120), rgb(112, 116, 122), rgb(102, 170, 255)};

enum class Theme : int { FollowNotepad = 0, Light = 1, Dark = 2 };
enum class ColorScale : int { Off = 0, PerColumn = 1, WholeTable = 2 };
enum class Align : int { Auto = 0, Left = 1, Center = 2, Right = 3 };

constexpr int kDecimalsInherit = -2;  // per-column value: use the global default
constexpr int kDecimalsRaw = -1;      // show the value exactly as the file has it

struct Options {
    // Opening
    bool autoOpen = true;
    std::string extensions = "csv;tsv;tab;psv";
    bool autoRefresh = true;
    bool rememberLayout = true;
    bool followCaret = true;

    // Parsing defaults for files without their own settings
    ParseOptions parse;
    std::string missingTokens = "NA;N/A;#N/A;NaN;nan;null;NULL;None;.";

    // Text and sizing
    std::string fontFace = "Segoe UI";
    int fontSize = 9;
    bool headerBold = true;
    int rowPadding = 3;
    int cellPadding = 6;
    int linesPerRow = 1;
    int minColumnWidth = 40;
    int maxAutoColumnWidth = 320;
    int autoFitRows = 1000;

    // Visible parts
    bool showToolbar = true;
    bool showStatusBar = true;
    bool showFilterRow = false;
    bool showRowNumbers = true;
    bool showGridH = true;
    bool showGridV = true;
    bool zebra = true;
    bool tooltips = true;

    // Values
    bool rightAlignNumbers = true;
    int decimals = kDecimalsRaw;
    bool thousands = false;
    bool dimMissing = true;
    ColorScale colorScale = ColorScale::Off;
    Color scaleLow = rgb(49, 104, 196);
    Color scaleMid = rgb(255, 255, 255);
    Color scaleHigh = rgb(214, 60, 50);

    // Sorting and filtering
    bool naturalSort = true;
    bool caseSensitiveFilter = false;

    // Colours
    Theme theme = Theme::FollowNotepad;
    Palette light = kLightPalette;
    Palette dark = kDarkPalette;
};

void saveOptions(const Options& options, Ini& ini);
void loadOptions(const Ini& ini, Options& options);

struct ColumnLayout {
    int width = 0;  // 0 = fit to content
    bool hidden = false;
    Align align = Align::Auto;
    int decimals = kDecimalsInherit;
};

// Per-file settings. Sort and filters are deliberately left out: reopening a file with rows
// silently hidden by a forgotten filter would make the data look wrong.
struct DocLayout {
    bool hasParse = false;  // the user changed parsing for this file
    ParseOptions parse;
    std::vector<ColumnLayout> columns;  // by file column
    std::vector<int> order;             // display order of file columns
    int frozenColumns = 0;
};

void saveLayout(const DocLayout& layout, Ini& ini, const std::string& section);
bool loadLayout(const Ini& ini, const std::string& section, DocLayout& layout);

}  // namespace xsv
