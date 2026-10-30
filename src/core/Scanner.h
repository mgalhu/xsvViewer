// The one place that knows how a record is laid out. Detection and the table both use it, so
// they can never disagree about where a record ends.
#pragma once
#include <cstring>
#include <string_view>

#include "Dialect.h"

namespace xsv {

inline const char* skipLine(const char* p, const char* end) {
    while (p < end && *p != '\n' && *p != '\r') ++p;
    if (p < end && *p == '\r' && p + 1 < end && p[1] == '\n') return p + 2;
    return p < end ? p + 1 : p;
}

inline bool startsWith(const char* p, const char* end, std::string_view prefix) {
    return !prefix.empty() && static_cast<size_t>(end - p) >= prefix.size() &&
           std::memcmp(p, prefix.data(), prefix.size()) == 0;
}

// Reads the record starting at p and calls field(begin, end, quoted, escaped) for each cell.
// Returns the start of the next record.
template <typename OnField>
const char* scanRecord(const char* p, const char* end, const Dialect& d, size_t& unbalancedQuotes, OnField&& field) {
    const char delimiter = d.delimiter[0];
    const size_t delimiterLength = d.delimiter.size();
    const char quote = d.quote;
    const char escape = d.escape;

    for (;;) {
        const char* start = p;
        const char* contentBegin = p;
        const char* contentEnd = nullptr;
        bool quoted = false;
        bool escaped = false;

        if (quote && p < end && *p == quote) {
            const char* q = p + 1;
            bool closed = false;
            while (q < end) {
                if (!escape) {
                    q = static_cast<const char*>(std::memchr(q, quote, static_cast<size_t>(end - q)));
                    if (!q) {
                        q = end;
                        break;
                    }
                } else if (*q == escape && q + 1 < end) {
                    q += 2;
                    escaped = true;
                    continue;
                } else if (*q != quote) {
                    ++q;
                    continue;
                }
                if (d.doubleQuote && q + 1 < end && q[1] == quote) {
                    q += 2;
                    escaped = true;
                    continue;
                }
                closed = true;
                break;
            }
            if (closed) {
                quoted = true;
                contentBegin = start + 1;
                contentEnd = q;
                p = q + 1;
            } else {
                // No closing quote before the end of the text. Treating it as literal keeps one
                // stray quote from swallowing the rest of the file.
                ++unbalancedQuotes;
                escaped = false;
            }
        }

        const char* s = p;
        while (s < end) {
            const char c = *s;
            if (c == '\n' || c == '\r') break;
            if (c == delimiter &&
                (delimiterLength == 1 || (static_cast<size_t>(end - s) >= delimiterLength &&
                                          std::memcmp(s, d.delimiter.data(), delimiterLength) == 0)))
                break;
            if (escape && c == escape && s + 1 < end) {
                s += 2;
                escaped = true;
                continue;
            }
            ++s;
        }

        if (quoted && s != p) {
            // Text after the closing quote ("a"b): not valid quoting, so show what the file has.
            contentBegin = start;
            contentEnd = s;
            quoted = false;
            escaped = false;
        } else if (!quoted) {
            contentEnd = s;
        }
        field(contentBegin, contentEnd, quoted, escaped);

        if (s >= end) return end;
        if (*s == '\n' || *s == '\r') return skipLine(s, end);
        p = s + delimiterLength;
    }
}

}  // namespace xsv
