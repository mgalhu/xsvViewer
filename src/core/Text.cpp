#include "Text.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace xsv {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// 1,234 and 1,234,567.5: one to three digits, then groups of exactly three.
bool isGrouped(std::string_view text, char group, char decimal) {
    size_t i = text[0] == '-' || text[0] == '+' ? 1 : 0;
    const size_t start = i;
    while (i < text.size() && isDigit(text[i])) ++i;
    if (i == start || i - start > 3 || i >= text.size() || text[i] != group) return false;
    while (i < text.size() && text[i] == group) {
        ++i;
        for (int digit = 0; digit < 3; ++digit, ++i) {
            if (i >= text.size() || !isDigit(text[i])) return false;
        }
    }
    if (i == text.size()) return true;
    if (text[i] != decimal) return false;
    const size_t fraction = ++i;
    while (i < text.size() && isDigit(text[i])) ++i;
    return i == text.size() && i > fraction;
}

bool outOfRangeValue(std::string_view text, double& value) {
    const bool negativeExponent = text.find("e-") != std::string_view::npos || text.find("E-") != std::string_view::npos;
    const bool negative = text.front() == '-';
    value = negativeExponent ? 0.0 : std::numeric_limits<double>::infinity();
    if (negative) value = -value;
    return true;
}

}  // namespace

bool parseNumber(std::string_view text, bool decimalComma, double& value) {
    while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
    if (text.empty()) return false;

    const char groupSeparator = decimalComma ? '.' : ',';
    const char decimalSeparator = decimalComma ? ',' : '.';
    const bool grouped = text.find(groupSeparator) != std::string_view::npos && isGrouped(text, groupSeparator, decimalSeparator);

    char buffer[64];
    if (grouped || (decimalComma && text.find(',') != std::string_view::npos)) {
        size_t length = 0;
        for (const char c : text) {
            if (grouped && c == groupSeparator) continue;
            if (length == sizeof buffer) return false;
            buffer[length++] = c == decimalSeparator ? '.' : c;
        }
        text = std::string_view(buffer, length);
    }

    if (text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty() || text.front() == '-' || text.front() == '+') return false;
    }

    double parsed = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, parsed, std::chars_format::general);
    if (ptr != end) return false;
    if (error == std::errc::result_out_of_range) return outOfRangeValue(text, value);
    if (error != std::errc{} || std::isnan(parsed)) return false;
    value = parsed;
    return true;
}

std::string formatNumber(double value, const NumberFormat& format) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Inf" : "Inf";

    char buffer[512];
    int length;
    if (format.decimals >= 0) {
        const double smallest = 0.5 * std::pow(10.0, -format.decimals);
        if (value != 0 && std::fabs(value) < smallest) {
            std::snprintf(buffer, sizeof buffer, "%.*e", format.decimals, value);
            std::string scientific(buffer);
            if (format.decimalComma) {
                const size_t dot = scientific.find('.');
                if (dot != std::string::npos) scientific[dot] = ',';
            }
            return scientific;
        }
        length = std::snprintf(buffer, sizeof buffer, "%.*f", format.decimals, value);
    } else {
        const auto result = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::fixed);
        length = static_cast<int>(result.ptr - buffer);
    }

    std::string_view digits(buffer, static_cast<size_t>(length));
    const char decimalSeparator = format.decimalComma ? ',' : '.';
    const char groupSeparator = format.decimalComma ? '.' : ',';

    std::string out;
    out.reserve(digits.size() + digits.size() / 3 + 1);
    size_t i = 0;
    if (digits[0] == '-') {
        out.push_back('-');
        i = 1;
    }
    const size_t dot = digits.find('.');
    const size_t integerEnd = dot == std::string_view::npos ? digits.size() : dot;
    for (size_t k = i; k < integerEnd; ++k) {
        out.push_back(digits[k]);
        const size_t remaining = integerEnd - k - 1;
        if (format.thousands && remaining && remaining % 3 == 0) out.push_back(groupSeparator);
    }
    if (dot != std::string_view::npos) {
        out.push_back(decimalSeparator);
        out.append(digits.substr(dot + 1));
    }
    return out;
}

std::string groupDigits(std::string_view text, bool decimalComma) {
    const size_t sign = !text.empty() && (text[0] == '-' || text[0] == '+') ? 1 : 0;
    const std::string_view digits = text.substr(sign);
    const bool plain = !digits.empty() && digits.find_first_not_of("0123456789") == std::string_view::npos;
    if (!plain || (digits.size() > 1 && digits[0] == '0')) return std::string(text);

    std::string out(text.substr(0, sign));
    for (size_t i = 0; i < digits.size(); ++i) {
        out.push_back(digits[i]);
        const size_t remaining = digits.size() - i - 1;
        if (remaining && remaining % 3 == 0) out.push_back(decimalComma ? '.' : ',');
    }
    return out;
}

MissingSet::MissingSet(std::string_view semicolonSeparated) {
    while (!semicolonSeparated.empty()) {
        const size_t split = semicolonSeparated.find(';');
        std::string_view token = semicolonSeparated.substr(0, split);
        while (!token.empty() && isSpace(token.front())) token.remove_prefix(1);
        while (!token.empty() && isSpace(token.back())) token.remove_suffix(1);
        if (!token.empty()) tokens_.emplace_back(token);
        if (split == std::string_view::npos) break;
        semicolonSeparated.remove_prefix(split + 1);
    }
}

bool MissingSet::contains(std::string_view cell) const {
    if (cell.empty()) return true;
    for (const std::string& token : tokens_) {
        if (cell == token) return true;
    }
    return false;
}

std::string foldCase(std::string_view text) {
    std::string out(text);
    for (size_t i = 0; i < out.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(out[i]);
        if (c < 0x80) {
            out[i] = lowerAscii(out[i]);
        } else if (c == 0xC3 && i + 1 < out.size()) {
            // U+00C0..U+00DE map to U+00E0..U+00FE, a change of one bit in the second byte;
            // U+00D7 (the multiplication sign) has no lower case.
            const unsigned char next = static_cast<unsigned char>(out[i + 1]);
            if (next >= 0x80 && next <= 0x9E && next != 0x97) out[i + 1] = static_cast<char>(next + 0x20);
            ++i;
        }
    }
    return out;
}

int compareText(std::string_view a, std::string_view b, bool natural) {
    const auto sign = [](int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); };
    if (!natural) {
        const int order = a.compare(b);
        return sign(order);
    }

    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            size_t startA = i, startB = j;
            while (startA < a.size() && a[startA] == '0') ++startA;
            while (startB < b.size() && b[startB] == '0') ++startB;
            size_t endA = startA, endB = startB;
            while (endA < a.size() && isDigit(a[endA])) ++endA;
            while (endB < b.size() && isDigit(b[endB])) ++endB;
            const size_t lengthA = endA - startA, lengthB = endB - startB;
            if (lengthA != lengthB) return lengthA < lengthB ? -1 : 1;
            const int order = std::memcmp(a.data() + startA, b.data() + startB, lengthA);
            if (order) return sign(order);
            i = endA;
            j = endB;
            continue;
        }
        const char ca = lowerAscii(a[i]), cb = lowerAscii(b[j]);
        if (ca != cb) return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb) ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return sign(a.compare(b));  // equal apart from case or leading zeros: keep the order total
}

}  // namespace xsv
