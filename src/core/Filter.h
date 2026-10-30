// Cell filters. A filter is one line of text:
//   abc          contains abc
//   !abc         does not contain abc
//   =abc  !=abc  equals / differs (numerically when both sides are numbers)
//   >1  >=1  <1  <=1   numeric comparison; text and missing cells never match
//   /regex/      regular expression, /regex/i for case-insensitive
//   a & b        both        a | b        either  (& binds tighter)
//   ==text !==text   the whole cell is exactly this; everything after the operator is taken
//                    literally, spaces and & | included, and it may be empty
#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xsv {

class Filter {
public:
    Filter();
    ~Filter();
    Filter(Filter&&) noexcept;
    Filter& operator=(Filter&&) noexcept;

    static Filter parse(std::string_view expression, bool caseSensitive, bool decimalComma);

    bool empty() const;   // nothing to test: matches every cell
    bool valid() const;   // false for e.g. a broken regex; an invalid filter matches every cell
    const std::string& error() const;
    bool matches(std::string_view cell) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xsv
