#include "Filter.h"

#include <regex>

#include "Text.h"

namespace xsv {

namespace {

constexpr size_t kMaxRegexInput = 8192;  // std::regex recursion gets expensive on long cells

enum class Op { Contains, NotContains, Equals, NotEquals, Greater, GreaterEqual, Less, LessEqual, Regex, Exact, NotExact };

struct Term {
    Op op = Op::Contains;
    std::string text;
    double number = 0;
    bool numeric = false;
    std::shared_ptr<std::regex> regex;
};

std::string_view trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

// A regex term ends at the first '/' that is followed by the end of the expression or an operator.
size_t findRegexEnd(std::string_view s, size_t from) {
    for (size_t i = s.find('/', from); i != std::string_view::npos; i = s.find('/', i + 1)) {
        size_t after = i + 1;
        if (after < s.size() && s[after] == 'i') ++after;
        const std::string_view rest = s.substr(after);
        if (rest.empty() || rest.starts_with(" & ") || rest.starts_with(" | ")) return after;
    }
    return std::string_view::npos;
}

}  // namespace

struct Filter::Impl {
    std::vector<std::vector<Term>> alternatives;  // OR of ANDs
    bool caseSensitive = false;
    bool decimalComma = false;
    bool needsFold = false;
    bool needsNumber = false;
    std::string error;
    mutable std::string folded;

    Term parseTerm(std::string_view source) {
        Term term;
        source = trim(source);

        if (source.size() >= 2 && source[0] == '/') {
            const bool insensitive = source.back() == 'i' && source[source.size() - 2] == '/';
            const size_t closing = insensitive ? source.size() - 2 : source.size() - 1;
            if (closing > 0 && source[closing] == '/') {
                term.op = Op::Regex;
                auto flags = std::regex::ECMAScript;
                if (insensitive) flags |= std::regex::icase;
                try {
                    term.regex = std::make_shared<std::regex>(std::string(source.substr(1, closing - 1)), flags);
                } catch (const std::regex_error& e) {
                    error = e.what();
                }
                return term;
            }
        }

        if (!source.empty() && source[0] == '\\') {  // \>5 searches for the text ">5"
            setText(term, Op::Contains, source.substr(1));
            return term;
        }

        static const struct {
            std::string_view prefix;
            Op op;
        } operators[] = {{">=", Op::GreaterEqual}, {"<=", Op::LessEqual}, {"!=", Op::NotEquals},
                         {">", Op::Greater},       {"<", Op::Less},       {"=", Op::Equals},
                         {"!", Op::NotContains}};
        for (const auto& candidate : operators) {
            if (!source.starts_with(candidate.prefix)) continue;
            const std::string_view rest = trim(source.substr(candidate.prefix.size()));
            if (rest.empty()) break;
            double number = 0;
            const bool isNumber = parseNumber(rest, decimalComma, number);
            const bool isComparison = candidate.op == Op::Greater || candidate.op == Op::GreaterEqual ||
                                      candidate.op == Op::Less || candidate.op == Op::LessEqual;
            if (isComparison && !isNumber) break;  // >abc is just text
            setText(term, candidate.op, rest);
            term.number = number;
            term.numeric = isNumber;
            needsNumber = needsNumber || isNumber;
            return term;
        }
        setText(term, Op::Contains, source);
        return term;
    }

    void setText(Term& term, Op op, std::string_view text) {
        term.op = op;
        if (op != Op::Greater && op != Op::GreaterEqual && op != Op::Less && op != Op::LessEqual) {
            term.text = caseSensitive ? std::string(text) : foldCase(text);
            needsFold = needsFold || !caseSensitive;
        }
    }

    void parseExpression(std::string_view source) {
        alternatives.emplace_back();
        size_t pos = 0;
        while (pos <= source.size()) {
            while (pos < source.size() && source[pos] == ' ') ++pos;
            size_t termEnd = std::string_view::npos;
            if (pos < source.size() && source[pos] == '/') termEnd = findRegexEnd(source, pos + 1);
            if (termEnd == std::string_view::npos) {
                const size_t nextAnd = source.find(" & ", pos);
                const size_t nextOr = source.find(" | ", pos);
                termEnd = std::min(nextAnd, nextOr);
                if (termEnd == std::string_view::npos) termEnd = source.size();
            }
            alternatives.back().push_back(parseTerm(source.substr(pos, termEnd - pos)));
            if (termEnd >= source.size()) break;
            if (source.compare(termEnd, 3, " | ") == 0) alternatives.emplace_back();
            pos = termEnd + 3;
        }
    }

    bool test(const Term& term, std::string_view cell, std::string_view searchable, bool isNumber, double number) const {
        switch (term.op) {
            case Op::Contains:
                return searchable.find(term.text) != std::string_view::npos;
            case Op::NotContains:
                return searchable.find(term.text) == std::string_view::npos;
            case Op::Equals:
                return (term.numeric && isNumber) ? number == term.number : searchable == term.text;
            case Op::NotEquals:
                return (term.numeric && isNumber) ? number != term.number : searchable != term.text;
            case Op::Greater:
                return isNumber && number > term.number;
            case Op::GreaterEqual:
                return isNumber && number >= term.number;
            case Op::Less:
                return isNumber && number < term.number;
            case Op::LessEqual:
                return isNumber && number <= term.number;
            case Op::Exact:
                return cell == term.text;
            case Op::NotExact:
                return cell != term.text;
            case Op::Regex:
                try {
                    const std::string_view input = cell.substr(0, kMaxRegexInput);
                    return std::regex_search(input.data(), input.data() + input.size(), *term.regex);
                } catch (const std::regex_error&) {
                    return false;  // pattern too complex for this cell
                }
        }
        return false;
    }
};

Filter::Filter() : impl_(std::make_unique<Impl>()) {}
Filter::~Filter() = default;
Filter::Filter(Filter&&) noexcept = default;
Filter& Filter::operator=(Filter&&) noexcept = default;

Filter Filter::parse(std::string_view expression, bool caseSensitive, bool decimalComma) {
    Filter filter;
    Impl& impl = *filter.impl_;
    impl.caseSensitive = caseSensitive;
    impl.decimalComma = decimalComma;

    const bool exact = expression.starts_with("==");
    if (exact || expression.starts_with("!==")) {
        Term term;
        term.op = exact ? Op::Exact : Op::NotExact;
        term.text = expression.substr(exact ? 2 : 3);
        impl.alternatives.push_back({std::move(term)});
        return filter;
    }

    expression = trim(expression);
    if (!expression.empty()) impl.parseExpression(expression);
    return filter;
}

bool Filter::empty() const { return impl_->alternatives.empty(); }
bool Filter::valid() const { return impl_->error.empty(); }
const std::string& Filter::error() const { return impl_->error; }

bool Filter::matches(std::string_view cell) const {
    const Impl& impl = *impl_;
    if (impl.alternatives.empty() || !impl.error.empty()) return true;

    std::string_view searchable = cell;
    if (impl.needsFold) {
        impl.folded = foldCase(cell);
        searchable = impl.folded;
    }
    double number = 0;
    const bool isNumber = impl.needsNumber && parseNumber(cell, impl.decimalComma, number);

    for (const auto& all : impl.alternatives) {
        bool ok = true;
        for (const Term& term : all) {
            if (!impl.test(term, cell, searchable, isNumber, number)) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

}  // namespace xsv
