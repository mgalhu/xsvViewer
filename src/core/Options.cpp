#include "Options.h"

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace xsv {

namespace {

bool parseInt(std::string_view text, int& value) {
    const char* end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, value);
    return error == std::errc{} && ptr == end && !text.empty();
}

std::string colorToText(Color c) {
    char buffer[8];
    std::snprintf(buffer, sizeof buffer, "#%02X%02X%02X", c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
    return buffer;
}

bool colorFromText(std::string_view text, Color& color) {
    if (text.size() != 7 || text[0] != '#') return false;
    unsigned value = 0;
    const char* end = text.data() + 7;
    const auto [ptr, error] = std::from_chars(text.data() + 1, end, value, 16);
    if (error != std::errc{} || ptr != end) return false;
    color = rgb((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    return true;
}

// The same field list drives saving and loading, so the two cannot drift apart.
class Saver {
public:
    Saver(Ini& ini, std::string section) : ini_(ini), section_(std::move(section)) {}
    void section(const char* name) { section_ = name; }
    void flag(const char* key, bool& v) { ini_.set(section_, key, v ? "1" : "0"); }
    void text(const char* key, std::string& v) { ini_.set(section_, key, v); }
    void number(const char* key, int& v, int, int) { ini_.set(section_, key, std::to_string(v)); }
    void color(const char* key, Color& v) { ini_.set(section_, key, colorToText(v)); }
    template <typename E>
    void choice(const char* key, E& v, int, int) {
        ini_.set(section_, key, std::to_string(static_cast<int>(v)));
    }
    void character(const char* key, char& v) { ini_.set(section_, key, std::to_string(static_cast<int>(v))); }
    void size(const char* key, size_t& v) { ini_.set(section_, key, std::to_string(v)); }

private:
    Ini& ini_;
    std::string section_;
};

class Loader {
public:
    Loader(const Ini& ini, std::string section) : ini_(ini), section_(std::move(section)) {}
    void section(const char* name) { section_ = name; }
    void flag(const char* key, bool& v) {
        int n;
        if (read(key, n)) v = n != 0;
    }
    void text(const char* key, std::string& v) { v = ini_.get(section_, key, v); }
    void number(const char* key, int& v, int low, int high) {
        int n;
        if (read(key, n)) v = std::clamp(n, low, high);
    }
    void color(const char* key, Color& v) {
        Color c;
        if (colorFromText(ini_.get(section_, key), c)) v = c;
    }
    template <typename E>
    void choice(const char* key, E& v, int low, int high) {
        int n;
        if (read(key, n)) v = static_cast<E>(std::clamp(n, low, high));
    }
    void character(const char* key, char& v) {
        int n;
        if (read(key, n)) v = static_cast<char>(std::clamp(n, 0, 127));
    }
    void size(const char* key, size_t& v) {
        int n;
        if (read(key, n) && n >= 0) v = static_cast<size_t>(n);
    }

private:
    bool read(const char* key, int& n) const { return parseInt(ini_.get(section_, key), n); }

    const Ini& ini_;
    std::string section_;
};

template <typename Archive>
void visitParse(Archive& a, ParseOptions& p) {
    a.text("delimiter", p.delimiter);
    a.character("quote", p.quote);
    a.flag("doubleQuote", p.doubleQuote);
    a.character("escape", p.escape);
    a.choice("header", p.header, 0, 2);
    a.choice("rowNames", p.rowNames, 0, 2);
    a.choice("decimalComma", p.decimalComma, 0, 2);
    a.flag("commentAuto", p.commentAuto);
    a.text("comment", p.comment);
    a.size("skipLines", p.skipLines);
    a.flag("trimCells", p.trimCells);
    a.flag("skipBlankLines", p.skipBlankLines);
}

template <typename Archive>
void visitPalette(Archive& a, Palette& p) {
    a.color("background", p.background);
    a.color("text", p.text);
    a.color("headerBackground", p.headerBackground);
    a.color("headerText", p.headerText);
    a.color("grid", p.grid);
    a.color("zebra", p.zebra);
    a.color("selection", p.selection);
    a.color("selectionText", p.selectionText);
    a.color("cursor", p.cursor);
    a.color("gutterBackground", p.gutterBackground);
    a.color("gutterText", p.gutterText);
    a.color("frozenLine", p.frozenLine);
    a.color("missingText", p.missingText);
    a.color("accent", p.accent);
}

template <typename Archive>
void visitOptions(Archive& a, Options& o) {
    a.section("Opening");
    a.flag("autoOpen", o.autoOpen);
    a.text("extensions", o.extensions);
    a.flag("autoRefresh", o.autoRefresh);
    a.flag("rememberLayout", o.rememberLayout);
    a.flag("followCaret", o.followCaret);

    a.section("Parsing");
    visitParse(a, o.parse);
    a.text("missingTokens", o.missingTokens);

    a.section("Appearance");
    a.text("fontFace", o.fontFace);
    a.number("fontSize", o.fontSize, 4, 72);
    a.flag("headerBold", o.headerBold);
    a.number("rowPadding", o.rowPadding, 0, 40);
    a.number("cellPadding", o.cellPadding, 0, 40);
    a.number("linesPerRow", o.linesPerRow, 1, 20);
    a.number("minColumnWidth", o.minColumnWidth, 10, 400);
    a.number("maxAutoColumnWidth", o.maxAutoColumnWidth, 40, 4000);
    a.number("autoFitRows", o.autoFitRows, 1, 1000000);
    a.flag("showToolbar", o.showToolbar);
    a.flag("showStatusBar", o.showStatusBar);
    a.flag("showFilterRow", o.showFilterRow);
    a.flag("showRowNumbers", o.showRowNumbers);
    a.flag("showGridH", o.showGridH);
    a.flag("showGridV", o.showGridV);
    a.flag("zebra", o.zebra);
    a.flag("tooltips", o.tooltips);

    a.section("Values");
    a.flag("rightAlignNumbers", o.rightAlignNumbers);
    a.number("decimals", o.decimals, kDecimalsRaw, 15);
    a.flag("thousands", o.thousands);
    a.flag("dimMissing", o.dimMissing);
    a.choice("colorScale", o.colorScale, 0, 2);
    a.color("scaleLow", o.scaleLow);
    a.color("scaleMid", o.scaleMid);
    a.color("scaleHigh", o.scaleHigh);
    a.flag("naturalSort", o.naturalSort);
    a.flag("caseSensitiveFilter", o.caseSensitiveFilter);

    a.section("Theme");
    a.choice("theme", o.theme, 0, 2);
    a.section("Light");
    visitPalette(a, o.light);
    a.section("Dark");
    visitPalette(a, o.dark);
}

template <typename T, typename Format>
std::string joinList(const std::vector<T>& items, Format format) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ',';
        out += std::to_string(format(items[i]));
    }
    return out;
}

bool parseIntList(std::string_view text, std::vector<int>& out) {
    out.clear();
    while (!text.empty()) {
        const size_t comma = text.find(',');
        int value;
        if (!parseInt(text.substr(0, comma), value)) return false;
        out.push_back(value);
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    return true;
}

}  // namespace

void saveOptions(const Options& options, Ini& ini) {
    Options copy = options;
    Saver saver(ini, "");
    visitOptions(saver, copy);
}

void loadOptions(const Ini& ini, Options& options) {
    Loader loader(ini, "");
    visitOptions(loader, options);
}

void saveLayout(const DocLayout& layout, Ini& ini, const std::string& section) {
    ini.set(section, "columns", std::to_string(layout.columns.size()));
    ini.set(section, "frozen", std::to_string(layout.frozenColumns));
    ini.set(section, "order", joinList(layout.order, [](int v) { return v; }));
    ini.set(section, "width", joinList(layout.columns, [](const ColumnLayout& c) { return c.width; }));
    ini.set(section, "hidden", joinList(layout.columns, [](const ColumnLayout& c) { return c.hidden ? 1 : 0; }));
    ini.set(section, "align", joinList(layout.columns, [](const ColumnLayout& c) { return static_cast<int>(c.align); }));
    ini.set(section, "decimals", joinList(layout.columns, [](const ColumnLayout& c) { return c.decimals; }));
    ini.set(section, "hasParse", layout.hasParse ? "1" : "0");
    if (layout.hasParse) {
        ParseOptions copy = layout.parse;
        Saver saver(ini, section);
        visitParse(saver, copy);
    }
}

bool loadLayout(const Ini& ini, const std::string& section, DocLayout& layout) {
    if (!ini.hasSection(section)) return false;
    layout = DocLayout{};

    int count = 0;
    if (!parseInt(ini.get(section, "columns"), count) || count < 0) return true;
    layout.columns.resize(static_cast<size_t>(count));

    int frozen = 0;
    if (parseInt(ini.get(section, "frozen"), frozen)) layout.frozenColumns = std::clamp(frozen, 0, count);

    std::vector<int> values;
    if (parseIntList(ini.get(section, "order"), values) && values.size() == layout.columns.size()) {
        std::vector<int> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        bool isPermutation = true;
        for (int i = 0; i < count; ++i) isPermutation = isPermutation && sorted[static_cast<size_t>(i)] == i;
        if (isPermutation) layout.order = values;
    }

    const auto apply = [&](const char* key, auto assign) {
        std::vector<int> list;
        if (!parseIntList(ini.get(section, key), list) || list.size() != layout.columns.size()) return;
        for (size_t i = 0; i < list.size(); ++i) assign(layout.columns[i], list[i]);
    };
    apply("width", [](ColumnLayout& c, int v) { c.width = std::max(v, 0); });
    apply("hidden", [](ColumnLayout& c, int v) { c.hidden = v != 0; });
    apply("align", [](ColumnLayout& c, int v) { c.align = static_cast<Align>(std::clamp(v, 0, 3)); });
    apply("decimals", [](ColumnLayout& c, int v) { c.decimals = std::clamp(v, kDecimalsInherit, 15); });

    layout.hasParse = ini.get(section, "hasParse") == "1";
    if (layout.hasParse) {
        Loader loader(ini, section);
        visitParse(loader, layout.parse);
    }
    return true;
}

}  // namespace xsv
