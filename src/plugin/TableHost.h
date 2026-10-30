// A table with its toolbar and status bar. One sits over each editor view, one in the dock panel.
#pragma once
#include <windows.h>

#include <memory>
#include <string>

#include "Document.h"
#include "Grid.h"

class TableHost : public DocumentObserver {
public:
    enum class Mode { InTab, Panel };

    static void registerClass();

    explicit TableHost(Mode mode) : mode_(mode) {}
    ~TableHost() override;

    HWND create(HWND parent);
    HWND hwnd() const { return hwnd_; }
    Grid& grid() { return grid_; }
    Document* document() const { return doc_; }

    void attach(Document* document);
    void documentChanged(bool structure) override;
    void optionsChanged();
    void focusFilter();

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK editProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);

    void createControls();
    void updateControlFont();
    void layoutChildren();
    void syncToolbar(bool replaceTypedText = false);
    void applyRowFilter();
    void paint(HDC dc);
    void onCommand(int id, int code);
    void commitDelimiterText();
    void changeParse(void (*change)(xsv::ParseOptions&, int), int value);
    void updateSelectionStats();
    std::wstring leftStatus() const;
    std::wstring rightStatus() const;
    int toolbarHeight() const;
    int statusHeight() const;
    int scale(int value) const;

    Mode mode_;
    HWND hwnd_ = nullptr;
    Grid grid_;
    Document* doc_ = nullptr;

    HWND delimiter_ = nullptr, quote_ = nullptr, header_ = nullptr, filter_ = nullptr;
    HWND filterRow_ = nullptr, fit_ = nullptr, columns_ = nullptr, options_ = nullptr, text_ = nullptr;
    HWND delimiterEdit_ = nullptr;
    HFONT controlFont_ = nullptr;
    HBRUSH toolbarBrush_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    bool syncing_ = false;

    xsv::SelectionStats stats_;
    bool haveStats_ = false;
    bool statsStale_ = false;
    Selection statsSelection_;
    size_t statsView_ = 0;
};
