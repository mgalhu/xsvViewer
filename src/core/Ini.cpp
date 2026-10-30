#include "Ini.h"

#include <algorithm>

namespace xsv {

void Ini::parse(std::string_view content) {
    sections_.clear();
    Section* current = nullptr;
    while (!content.empty()) {
        const size_t lineEnd = content.find('\n');
        std::string_view line = content.substr(0, lineEnd);
        content.remove_prefix(lineEnd == std::string_view::npos ? content.size() : lineEnd + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line[0] == ';') continue;

        if (line[0] == '[') {
            const size_t close = line.rfind(']');  // section names are file paths and may hold ']'
            if (close == std::string_view::npos) continue;
            sections_.push_back({std::string(line.substr(1, close - 1)), {}});
            current = &sections_.back();
            continue;
        }

        const size_t equals = line.find('=');
        if (!current || equals == std::string_view::npos || equals == 0) continue;
        current->values.emplace_back(line.substr(0, equals), line.substr(equals + 1));
    }
}

std::string Ini::serialize() const {
    std::string out;
    for (const Section& section : sections_) {
        out += '[';
        out += section.name;
        out += "]\n";
        for (const auto& [key, value] : section.values) {
            out += key;
            out += '=';
            out += value;
            out += '\n';
        }
        out += '\n';
    }
    return out;
}

std::string Ini::get(std::string_view section, std::string_view key, std::string_view fallback) const {
    for (const Section& s : sections_) {
        if (s.name != section) continue;
        for (const auto& [k, v] : s.values) {
            if (k == key) return v;
        }
    }
    return std::string(fallback);
}

void Ini::set(std::string_view section, std::string_view key, std::string_view value) {
    auto s = std::find_if(sections_.begin(), sections_.end(), [&](const Section& x) { return x.name == section; });
    if (s == sections_.end()) {
        sections_.push_back({std::string(section), {}});
        s = sections_.end() - 1;
    }
    for (auto& [k, v] : s->values) {
        if (k == key) {
            v = value;
            return;
        }
    }
    s->values.emplace_back(key, value);
}

bool Ini::hasSection(std::string_view section) const {
    return std::any_of(sections_.begin(), sections_.end(), [&](const Section& s) { return s.name == section; });
}

void Ini::removeSection(std::string_view section) {
    std::erase_if(sections_, [&](const Section& s) { return s.name == section; });
}

std::vector<std::string> Ini::sections() const {
    std::vector<std::string> names;
    names.reserve(sections_.size());
    for (const Section& s : sections_) names.push_back(s.name);
    return names;
}

}  // namespace xsv
