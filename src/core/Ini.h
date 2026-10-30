// Small order-preserving INI document; values are UTF-8 and may contain anything but newlines.
#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xsv {

class Ini {
public:
    void parse(std::string_view content);
    std::string serialize() const;

    std::string get(std::string_view section, std::string_view key, std::string_view fallback = {}) const;
    void set(std::string_view section, std::string_view key, std::string_view value);

    bool hasSection(std::string_view section) const;
    void removeSection(std::string_view section);
    std::vector<std::string> sections() const;

private:
    struct Section {
        std::string name;
        std::vector<std::pair<std::string, std::string>> values;
    };
    std::vector<Section> sections_;
};

}  // namespace xsv
