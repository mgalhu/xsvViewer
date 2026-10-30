// The table itself: a virtual, owner-drawn grid. Only what is on screen is ever parsed or painted.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "Document.h"
#include "Export.h"

class TableHost;

class Grid {
public:
    static void registerClass();

    HWND create(HWND parent, TableHost* host);
    HWND hwnd() const { return hwnd_; }

    void setDocument(Document* document);
    void refresh(bool structure);   // data or layout changed
    void optionsChanged();          // fonts, sizes or colours changed
    void fitColumns();              // back to content width for every column

    // Called for every key press while the grid has focus, before Notepad++ sees it.
    bool handleKey(UINT key, bool ctrl, bool shift);

private:
    enum class Zone { None, Corner, Header, ResizeEdge, Filter, Gutter, Cell };
    enum class Drag { None, Resize, HeaderPress, Reorder, Cells, Rows };

    struct Hit {
        Zone zone = Zone::None;
        size_t position = 0;  // column position
        size_t row = 0;       // view row
    };

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK filterEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);

    // Geometry
    void rebuildFonts();
    void measure();
    void computeAutoWidths();
    void rebuildColumnEdges();
    void updateScrollBars();
    int scale(int value) const;
    int columnWidth(size_t fileColumn) const;
    int columnScreenX(size_t position) const;
    int bodyTop() const;
    int visibleRows() const;
    int viewportWidth() const;
    Hit hitTest(int x, int y) const;
    size_t rowAt(int y) const;
    size_t positionAt(int x) const;

    // Painting
    void paint(HDC target);
    void paintBody(HDC dc, const RECT& client);
    void paintRowCells(HDC dc, size_t rowIndex, size_t viewRow, int y, size_t first, size_t last, int clipLeft, int clipRight);
    void paintHeader(HDC dc, const RECT& client);
    void paintHeaderCell(HDC dc, size_t position, int clipLeft, int clipRight);
    void paintFilterRow(HDC dc, const RECT& client);
    void paintGutter(HDC dc, const RECT& client);
    void drawText(HDC dc, const wchar_t* text, int length, const RECT& cell, xsv::Align align, COLORREF color);
    void selectedRange(size_t& firstRow, size_t& lastRow, size_t& firstColumn, size_t& lastColumn) const;
    bool isSelected(size_t viewRow, size_t position) const;
    int toWide(std::string_view text, wchar_t* out, int capacity) const;
    int measureWidth(std::string_view text) const;

    // Interaction
    void scrollTo(size_t row, int x);
    void ensureVisible(size_t row, size_t position);
    void moveCursor(long long rows, long long columns, bool extend);
    void setCursor(size_t row, size_t position, bool extend);
    void selectionChanged();
    void onLeftDown(int x, int y, WPARAM keys);
    void onLeftUp();
    void onLeftDouble(int x, int y);
    void onMouseMove(int x, int y, WPARAM keys);
    void onContextMenu(int x, int y);
    void onHover(int x, int y);
    void extendDragSelection(int x, int y);
    void headerMenu(size_t position, POINT screen);
    void cellMenu(size_t viewRow, size_t position, POINT screen);
    void copySelection(xsv::ExportFormat format, bool header);
    void beginFilterEdit(size_t position);
    void commitFilterEdit(bool focusGrid);
    void placeFilterEdit();
    void hideTip();
    void zoom(int steps);

    HWND hwnd_ = nullptr;
    TableHost* host_ = nullptr;
    Document* doc_ = nullptr;

    int dpi_ = 96;
    HFONT font_ = nullptr;
    HFONT headerFont_ = nullptr;
    int textHeight_ = 16;
    int charWidths_[128] = {};
    int wideCharWidth_ = 16;
    int rowHeight_ = 22;
    int headerHeight_ = 26;
    int filterHeight_ = 24;
    int gutterWidth_ = 0;
    int cellPadding_ = 6;

    std::vector<int> autoWidths_;       // by file column, unscaled pixels
    std::vector<long long> edges_;      // left edge of each visible column; one extra at the end
    size_t autoWidthsFor_ = SIZE_MAX;

    HDC bufferDc_ = nullptr;
    HBITMAP buffer_ = nullptr;
    SIZE bufferSize_ = {};

    Drag drag_ = Drag::None;
    POINT dragOrigin_ = {};
    size_t dragPosition_ = 0;
    int dragStartWidth_ = 0;
    size_t dropPosition_ = 0;
    bool shiftAtPress_ = false;

    HWND filterEdit_ = nullptr;
    size_t filterPosition_ = SIZE_MAX;

    HWND tip_ = nullptr;
    std::wstring tipText_;
    bool tipVisible_ = false;
    Hit tipCell_;
    bool trackingMouse_ = false;

    std::vector<xsv::Cell> cells_;
    std::string scratch_, format_;
    std::vector<wchar_t> wide_;
};
