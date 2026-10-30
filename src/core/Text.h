// Number parsing/formatting and text comparison shared by the parser, filters and sorting.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace xsv {

// Locale-independent. Accepts surrounding spaces, a leading '+', scientific notation and
// Inf/-Inf. Returns false for anything else, including NaN (treat that as missing).
bool parseNumber(std::string_view text, bool decimalComma, double& value);

struct NumberFormat {
    int decimals = -1;          // -1 keeps the text as written in the file
    bool thousands = false;
    bool decimalComma = false;
};

// Values too small to show at the requested precision switch to scientific notation,
// so a p-value of 1.2e-08 never reads as 0.000.
std::string formatNumber(double value, const NumberFormat& format);

// Adds thousands separators to a whole number written as plain digits, without going through a
// double, so long IDs keep every digit. Anything else, leading zeros included, comes back unchanged.
std::string groupDigits(std::string_view text, bool decimalComma);

// The tokens that mean "no value". An empty cell always counts as missing.
class MissingSet {
public:
    MissingSet() = default;
    explicit MissingSet(std::string_view semicolonSeparated);
    bool contains(std::string_view cell) const;

private:
    std::vector<std::string> tokens_;
};

// Case folds ASCII and the Latin-1 range of UTF-8 (so Ä matches ä).
std::string foldCase(std::string_view text);

// strcmp-style result. With natural == true, digit runs compare by value: s2 < s10.
int compareText(std::string_view a, std::string_view b, bool natural);

}  // namespace xsv
