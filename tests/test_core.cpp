// Unit tests for the core library. Run: xsvtests.exe [--perf]
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "Dialect.h"
#include "Export.h"
#include "Filter.h"
#include "Ini.h"
#include "Options.h"
#include "Table.h"
#include "Text.h"
#include "View.h"

using namespace xsv;

namespace {

int g_failures = 0;
int g_checks = 0;
const char* g_current = "";

struct TestCase {
    const char* name;
    std::function<void()> body;
};
std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}
struct Register {
    Register(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

#define TEST(name)                                     \
    static void test_##name();                         \
    static Register register_##name(#name, test_##name); \
    static void test_##name()

void fail(const char* file, int line, const std::string& message) {
    ++g_failures;
    std::printf("  FAIL %s (%s:%d): %s\n", g_current, file, line, message.c_str());
}

std::string show(const std::string& v) { return "\"" + v + "\""; }
std::string show(std::string_view v) { return show(std::string(v)); }
std::string show(const char* v) { return show(std::string(v)); }
std::string show(bool v) { return v ? "true" : "false"; }
std::string show(char v) { return "'" + std::string(1, v) + "'"; }
template <typename T>
std::string show(const T& v) {
    return std::to_string(v);
}
std::string show(const std::vector<size_t>& v) {
    std::string out = "[";
    for (size_t i = 0; i < v.size(); ++i) out += (i ? "," : "") + std::to_string(v[i]);
    return out + "]";
}

#define EXPECT_EQ(actual, expected)                                                                     \
    do {                                                                                                \
        ++g_checks;                                                                                     \
        auto&& a_ = (actual);                                                                           \
        auto&& e_ = (expected);                                                                         \
        if (!(a_ == e_)) fail(__FILE__, __LINE__, #actual " = " + show(a_) + ", expected " + show(e_)); \
    } while (0)
#define EXPECT_TRUE(cond)                                               \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) fail(__FILE__, __LINE__, #cond " is false");       \
    } while (0)
#define EXPECT_FALSE(cond)                                              \
    do {                                                                \
        ++g_checks;                                                     \
        if (cond) fail(__FILE__, __LINE__, #cond " is true");           \
    } while (0)
#define EXPECT_NEAR(actual, expected)                                                               \
    do {                                                                                            \
        ++g_checks;                                                                                 \
        double a_ = (actual), e_ = (expected);                                                      \
        if (std::fabs(a_ - e_) > 1e-9) fail(__FILE__, __LINE__, #actual " = " + std::to_string(a_)); \
    } while (0)

Dialect plain(std::string delimiter = ",", bool header = false) {
    Dialect d;
    d.delimiter = std::move(delimiter);
    d.hasHeader = header;
    return d;
}

Table table(std::string text, Dialect d = plain()) {
    Table t;
    t.load(std::move(text), std::move(d));
    return t;
}

std::string cell(const Table& t, size_t row, size_t column) {
    std::vector<Cell> cells;
    t.readRow(row, cells);
    if (column >= cells.size()) return "<none>";
    std::string scratch;
    return std::string(t.cellText(cells[column], scratch));
}

Dialect detect(std::string_view text, std::string_view extension = "csv") {
    return detectDialect(text, ParseOptions{}, extension);
}

}  // namespace

// ---- Table parsing ---------------------------------------------------------------------------

TEST(reads_rows_and_columns) {
    Table t = table("a,b,c\n1,2,3\n");
    EXPECT_EQ(t.rowCount(), 2u);
    EXPECT_EQ(t.columnCount(), 3u);
    EXPECT_EQ(cell(t, 1, 2), "3");
}

TEST(quoted_cell_keeps_delimiter) {
    Table t = table("\"a,b\",c\n");
    EXPECT_EQ(t.columnCount(), 2u);
    EXPECT_EQ(cell(t, 0, 0), "a,b");
}

TEST(doubled_quote_collapses_to_one) {
    Table t = table("\"say \"\"hi\"\"\",x\n");
    EXPECT_EQ(cell(t, 0, 0), "say \"hi\"");
    EXPECT_EQ(cell(t, 0, 1), "x");
}

TEST(newline_inside_quotes_stays_in_the_record) {
    Table t = table("\"line1\nline2\",b\nc,d\n");
    EXPECT_EQ(t.rowCount(), 2u);
    EXPECT_EQ(cell(t, 0, 0), "line1\nline2");
    EXPECT_EQ(cell(t, 1, 0), "c");
}

TEST(handles_all_line_endings) {
    EXPECT_EQ(table("a\r\nb\r\nc\r\n").rowCount(), 3u);
    EXPECT_EQ(table("a\rb\rc").rowCount(), 3u);
    EXPECT_EQ(table("a\nb\r\nc\rd").rowCount(), 4u);
}

TEST(no_trailing_newline_keeps_last_row) {
    Table t = table("a,b\nc,d");
    EXPECT_EQ(t.rowCount(), 2u);
    EXPECT_EQ(cell(t, 1, 1), "d");
}

TEST(trailing_delimiter_is_an_empty_cell) {
    Table t = table("a,b,\n");
    EXPECT_EQ(t.columnCount(), 3u);
    EXPECT_EQ(cell(t, 0, 2), "");
}

TEST(blank_lines_are_skipped_by_default) {
    EXPECT_EQ(table("a\n\n\nb\n").rowCount(), 2u);
}

TEST(blank_lines_are_rows_when_asked) {
    Dialect d = plain();
    d.skipBlankLines = false;
    EXPECT_EQ(table("a\n\nb\n", d).rowCount(), 3u);
}

TEST(comment_lines_are_skipped) {
    Dialect d = plain();
    d.comment = "#";
    Table t = table("# note, with comma\na,b\n#x\nc,d\n", d);
    EXPECT_EQ(t.rowCount(), 2u);
    EXPECT_EQ(cell(t, 1, 0), "c");
}

TEST(skip_lines_drops_physical_lines_first) {
    Dialect d = plain();
    d.skipLines = 2;
    Table t = table("junk\n\"unterminated\na,b\n", d);
    EXPECT_EQ(t.rowCount(), 1u);
    EXPECT_EQ(cell(t, 0, 0), "a");
}

TEST(ragged_rows_widen_the_table_and_are_counted) {
    Table t = table("a,b\nc\nd,e,f\ng,h\n");
    EXPECT_EQ(t.columnCount(), 3u);
    EXPECT_EQ(t.rowCount(), 4u);
    EXPECT_EQ(t.raggedRows(), 2u);
    EXPECT_EQ(cell(t, 1, 1), "<none>");
}

TEST(header_row_names_columns) {
    Table t = table("x,y\n1,2\n", plain(",", true));
    EXPECT_EQ(t.rowCount(), 1u);
    EXPECT_EQ(t.columnNames()[1], "y");
    EXPECT_EQ(cell(t, 0, 0), "1");
}

TEST(columns_get_generated_names_without_a_header) {
    Table t = table("1,2\n");
    EXPECT_EQ(t.columnNames()[0], "Column 1");
    EXPECT_EQ(t.columnNames()[1], "Column 2");
}

TEST(data_wider_than_header_gets_generated_names_for_the_rest) {
    Table t = table("x\n1,2\n", plain(",", true));
    EXPECT_EQ(t.columnNames()[1], "Column 2");
}

TEST(r_style_header_is_shifted_past_the_row_names) {
    Dialect d = plain("\t", true);
    d.rowNamesHeader = true;
    Table t = table("A\tB\nr1\t1\t2\nr2\t3\t4\n", d);
    EXPECT_EQ(t.columnCount(), 3u);
    EXPECT_EQ(t.columnNames()[0], "row.names");
    EXPECT_EQ(t.columnNames()[1], "A");
    EXPECT_EQ(t.columnNames()[2], "B");
    EXPECT_EQ(t.raggedRows(), 0u);
}

TEST(multi_byte_delimiter) {
    Table t = table("a||b||c\n", plain("||"));
    EXPECT_EQ(t.columnCount(), 3u);
    EXPECT_EQ(cell(t, 0, 2), "c");
}

TEST(non_ascii_delimiter) {
    Table t = table("a\xC2\xA7"
                    "b\n",
                    plain("\xC2\xA7"));
    EXPECT_EQ(t.columnCount(), 2u);
    EXPECT_EQ(cell(t, 0, 1), "b");
}

TEST(unterminated_quote_is_literal_and_does_not_eat_the_file) {
    Table t = table("a,b\n\"oops,c\nd,e\n");
    EXPECT_EQ(t.rowCount(), 3u);
    EXPECT_EQ(cell(t, 1, 0), "\"oops");
    EXPECT_EQ(cell(t, 2, 1), "e");
    EXPECT_EQ(t.unbalancedQuotes(), 1u);
}

TEST(quote_inside_unquoted_cell_is_literal) {
    Table t = table("5\" disk,b\n");
    EXPECT_EQ(cell(t, 0, 0), "5\" disk");
    EXPECT_EQ(cell(t, 0, 1), "b");
}

TEST(backslash_escape) {
    Dialect d = plain();
    d.escape = '\\';
    d.doubleQuote = false;
    Table t = table("\"a\\\"b\\\\c\",d\n", d);
    EXPECT_EQ(t.columnCount(), 2u);
    EXPECT_EQ(cell(t, 0, 0), "a\"b\\c");
}

TEST(quote_none_treats_quotes_as_text) {
    Dialect d = plain();
    d.quote = 0;
    Table t = table("\"a,b\"\n", d);
    EXPECT_EQ(t.columnCount(), 2u);
    EXPECT_EQ(cell(t, 0, 0), "\"a");
}

TEST(trim_removes_spaces_around_cells) {
    Dialect d = plain();
    d.trimCells = true;
    Table t = table("  a  ,\tb\n", d);
    EXPECT_EQ(cell(t, 0, 0), "a");
    EXPECT_EQ(cell(t, 0, 1), "b");
}

TEST(empty_document) {
    Table t = table("");
    EXPECT_EQ(t.rowCount(), 0u);
    EXPECT_EQ(t.columnCount(), 0u);
}

TEST(header_only_document) {
    Table t = table("x,y\n", plain(",", true));
    EXPECT_EQ(t.rowCount(), 0u);
    EXPECT_EQ(t.columnCount(), 2u);
}

TEST(cell_offsets_point_into_the_source) {
    Table t = table("ab,\"cd\"\nef,gh\n");
    std::vector<Cell> cells;
    t.readRow(1, cells);
    EXPECT_EQ(t.text().substr(cells[1].offset, cells[1].length), "gh");
    t.readRow(0, cells);
    EXPECT_EQ(t.text().substr(cells[1].offset, cells[1].length), "cd");
}

TEST(locate_finds_the_cell_for_a_caret_offset) {
    Table t = table("ab,cd\nef,gh\n");
    size_t row = 99, column = 99;
    EXPECT_TRUE(t.locate(10, row, column));  // the h of gh
    EXPECT_EQ(row, 1u);
    EXPECT_EQ(column, 1u);
    EXPECT_TRUE(t.locate(0, row, column));
    EXPECT_EQ(row, 0u);
    EXPECT_EQ(column, 0u);
}

TEST(locate_rejects_offsets_in_the_header) {
    Table t = table("x,y\n1,2\n", plain(",", true));
    size_t row = 0, column = 0;
    EXPECT_FALSE(t.locate(1, row, column));
    EXPECT_TRUE(t.locate(5, row, column));
    EXPECT_EQ(row, 0u);
}

// ---- Dialect detection -----------------------------------------------------------------------

TEST(detects_tab) { EXPECT_EQ(detect("a\tb\tc\n1\t2\t3\n").delimiter, "\t"); }
TEST(detects_comma) { EXPECT_EQ(detect("a,b,c\n1,2,3\n").delimiter, ","); }
TEST(detects_semicolon) { EXPECT_EQ(detect("a;b;c\n1;2;3\n").delimiter, ";"); }
TEST(detects_pipe) { EXPECT_EQ(detect("a|b|c\n1|2|3\n").delimiter, "|"); }

TEST(detection_ignores_delimiters_inside_quotes) {
    EXPECT_EQ(detect("\"a;b;c;d\",x\n\"e;f;g;h\",y\n\"i;j;k;l\",z\n").delimiter, ",");
}

TEST(detection_prefers_the_consistent_delimiter) {
    EXPECT_EQ(detect("id\tnote\n1\ta, b, c\n2\tplain\n3\tx, y\n4\tz\n").delimiter, "\t");
}

TEST(extension_breaks_ties) {
    EXPECT_EQ(detect("a,b\tc\n1,2\t3\n", "tsv").delimiter, "\t");
    EXPECT_EQ(detect("a,b\tc\n1,2\t3\n", "csv").delimiter, ",");
}

TEST(single_column_files_fall_back_to_the_extension) {
    EXPECT_EQ(detect("one\ntwo\nthree\n", "tsv").delimiter, "\t");
    EXPECT_EQ(detect("one\ntwo\nthree\n", "csv").delimiter, ",");
}

TEST(explicit_delimiter_wins) {
    ParseOptions o;
    o.delimiter = ";";
    EXPECT_EQ(detectDialect("a,b\n", o, "csv").delimiter, ";");
}

TEST(header_detected_over_numeric_body) { EXPECT_TRUE(detect("name,value\na,1\nb,2\n").hasHeader); }
TEST(numeric_first_row_is_not_a_header) { EXPECT_FALSE(detect("1,2\n3,4\n5,6\n").hasHeader); }
TEST(text_table_defaults_to_a_header) { EXPECT_TRUE(detect("city,country\nGraz,AT\nWien,AT\n").hasHeader); }
TEST(single_row_is_data_not_a_header) { EXPECT_FALSE(detect("a,b,c\n").hasHeader); }

TEST(explicit_header_choice_wins) {
    ParseOptions o;
    o.header = Tri::No;
    EXPECT_FALSE(detectDialect("name,value\na,1\n", o, "csv").hasHeader);
}

TEST(decimal_comma_detected_with_semicolons) {
    Dialect d = detect("a;b\n1,5;2,25\n3,5;4,75\n");
    EXPECT_EQ(d.delimiter, ";");
    EXPECT_TRUE(d.decimalComma);
}

TEST(no_decimal_comma_in_comma_files) { EXPECT_FALSE(detect("a,b\n1,2\n3,4\n").decimalComma); }

TEST(us_thousands_are_not_decimal_commas) {
    EXPECT_FALSE(detect("name\tn\na\t709,037\nb\t289,330\nc\t999\n", "tsv").decimalComma);
}

TEST(decimal_comma_detected_in_tabs_from_clear_cases) {
    EXPECT_TRUE(detect("name\tn\na\t1,5\nb\t2,25\n", "tsv").decimalComma);
}

TEST(three_decimals_count_with_semicolons) {
    EXPECT_TRUE(detect("a;b\n1,250;2,500\n3,750;4,125\n").decimalComma);
}

TEST(r_row_names_header_is_detected) {
    Dialect d = detect("A\tB\nr1\t1\t2\nr2\t3\t4\nr3\t5\t6\n", "tsv");
    EXPECT_TRUE(d.hasHeader);
    EXPECT_TRUE(d.rowNamesHeader);
}

TEST(trailing_delimiter_is_not_r_row_names) {
    const char* text = "id,name,score\n1,Ann,90,\n2,Bob,85,\n3,Cid,70,\n";
    Dialect d = detect(text);
    EXPECT_FALSE(d.rowNamesHeader);
    Table t;
    t.load(text, d);
    EXPECT_EQ(t.columnNames()[0], "id");
    EXPECT_EQ(cell(t, 0, 1), "Ann");
    EXPECT_EQ(t.columnNames()[2], "score");
}

TEST(aligned_header_is_not_treated_as_row_names) {
    EXPECT_FALSE(detect("id\tA\tB\nr1\t1\t2\nr2\t3\t4\n", "tsv").rowNamesHeader);
}

TEST(hash_preamble_is_a_comment) {
    Dialect d = detect("# written by tool 1.2\n# date 2026\nid,value\na,1\nb,2\n");
    EXPECT_EQ(d.comment, "#");
    EXPECT_TRUE(d.hasHeader);
}

TEST(vcf_style_header_line_survives_comment_detection) {
    const char* vcf = "##fileformat=VCFv4.2\n##source=x\n#CHROM\tPOS\tID\n1\t100\trs1\n2\t200\trs2\n";
    Dialect d = detect(vcf, "tsv");
    EXPECT_EQ(d.comment, "##");
    Table t;
    t.load(vcf, d);
    EXPECT_EQ(t.columnNames()[0], "#CHROM");
    EXPECT_EQ(t.rowCount(), 2u);
}

TEST(hash_header_without_preamble_is_a_header) {
    Dialect d = detect("#id\tvalue\na\t1\nb\t2\n", "tsv");
    Table t;
    t.load("#id\tvalue\na\t1\nb\t2\n", d);
    EXPECT_EQ(t.columnNames()[0], "#id");
}

TEST(skip_lines_are_ignored_by_detection) {
    ParseOptions o;
    o.skipLines = 2;
    Dialect d = detectDialect("title; of; report\nmore; junk\na,b\n1,2\n3,4\n", o, "csv");
    EXPECT_EQ(d.delimiter, ",");
    EXPECT_EQ(d.skipLines, 2u);
}

TEST(detection_survives_empty_text) {
    EXPECT_EQ(detect("", "tsv").delimiter, "\t");
    EXPECT_FALSE(detect("").hasHeader);
}

// ---- Numbers ---------------------------------------------------------------------------------

TEST(parses_numbers) {
    double v = 0;
    EXPECT_TRUE(parseNumber("42", false, v));
    EXPECT_NEAR(v, 42);
    EXPECT_TRUE(parseNumber("-1.5e3", false, v));
    EXPECT_NEAR(v, -1500);
    EXPECT_TRUE(parseNumber(" +2.5 ", false, v));
    EXPECT_NEAR(v, 2.5);
    EXPECT_TRUE(parseNumber(".5", false, v));
    EXPECT_NEAR(v, 0.5);
    EXPECT_TRUE(parseNumber("1,5", true, v));
    EXPECT_NEAR(v, 1.5);
}

TEST(parses_infinity_from_r) {
    double v = 0;
    EXPECT_TRUE(parseNumber("Inf", false, v));
    EXPECT_TRUE(std::isinf(v));
    EXPECT_TRUE(parseNumber("-Inf", false, v));
    EXPECT_TRUE(v < 0);
}

TEST(rejects_non_numbers) {
    double v = 0;
    EXPECT_FALSE(parseNumber("", false, v));
    EXPECT_FALSE(parseNumber("abc", false, v));
    EXPECT_FALSE(parseNumber("12abc", false, v));
    EXPECT_FALSE(parseNumber("1.2.3", false, v));
    EXPECT_FALSE(parseNumber("1,5", false, v));
    EXPECT_FALSE(parseNumber("NaN", false, v));
    EXPECT_FALSE(parseNumber("2026-10-30", false, v));
}

TEST(parses_grouped_numbers) {
    double v = 0;
    EXPECT_TRUE(parseNumber("709,037", false, v));
    EXPECT_NEAR(v, 709037);
    EXPECT_TRUE(parseNumber("-1,234,567.5", false, v));
    EXPECT_NEAR(v, -1234567.5);
    EXPECT_TRUE(parseNumber("1.234,5", true, v));
    EXPECT_NEAR(v, 1234.5);
    EXPECT_TRUE(parseNumber("1.234", true, v));
    EXPECT_NEAR(v, 1234);
    EXPECT_TRUE(parseNumber("1.234.567", true, v));
    EXPECT_NEAR(v, 1234567);
}

TEST(rejects_badly_grouped_numbers) {
    double v = 0;
    EXPECT_FALSE(parseNumber("12,34", false, v));
    EXPECT_FALSE(parseNumber("1,2345", false, v));
    EXPECT_FALSE(parseNumber("1,234,56", false, v));
    EXPECT_FALSE(parseNumber("1234,567", false, v));
    EXPECT_FALSE(parseNumber("1.2.3,5", true, v));
}

TEST(decimal_comma_mode_keeps_plain_dotted_decimals) {
    double v = 0;
    EXPECT_TRUE(parseNumber("1.5", true, v));
    EXPECT_NEAR(v, 1.5);
}

TEST(groups_digits_of_whole_numbers) {
    EXPECT_EQ(groupDigits("12345678901234567890", false), "12,345,678,901,234,567,890");
    EXPECT_EQ(groupDigits("-1234", false), "-1,234");
    EXPECT_EQ(groupDigits("1234", true), "1.234");
    EXPECT_EQ(groupDigits("999", false), "999");
}

TEST(groups_digits_leaves_ids_and_other_text_alone) {
    EXPECT_EQ(groupDigits("01234", false), "01234");
    EXPECT_EQ(groupDigits("12a34", false), "12a34");
    EXPECT_EQ(groupDigits("709,037", false), "709,037");
}

TEST(formats_decimals) {
    NumberFormat f;
    f.decimals = 2;
    EXPECT_EQ(formatNumber(3.14159, f), "3.14");
    EXPECT_EQ(formatNumber(-2.0, f), "-2.00");
    f.decimals = 0;
    EXPECT_EQ(formatNumber(2.6, f), "3");
}

TEST(formats_thousands_and_decimal_comma) {
    NumberFormat f;
    f.decimals = 1;
    f.thousands = true;
    EXPECT_EQ(formatNumber(1234567.25, f), "1,234,567.2");
    f.decimalComma = true;
    EXPECT_EQ(formatNumber(1234567.25, f), "1.234.567,2");
}

TEST(tiny_values_never_round_to_zero) {
    NumberFormat f;
    f.decimals = 3;
    EXPECT_EQ(formatNumber(1.2e-08, f), "1.200e-08");
    EXPECT_EQ(formatNumber(0.0, f), "0.000");
}

TEST(infinity_formats_readably) {
    NumberFormat f;
    f.decimals = 2;
    EXPECT_EQ(formatNumber(std::numeric_limits<double>::infinity(), f), "Inf");
    EXPECT_EQ(formatNumber(-std::numeric_limits<double>::infinity(), f), "-Inf");
}

TEST(missing_tokens) {
    MissingSet m("NA;null");
    EXPECT_TRUE(m.contains(""));
    EXPECT_TRUE(m.contains("NA"));
    EXPECT_TRUE(m.contains("null"));
    EXPECT_FALSE(m.contains("na"));
    EXPECT_FALSE(m.contains("0"));
}

TEST(fold_case_handles_umlauts) {
    EXPECT_EQ(foldCase("ABC"), "abc");
    EXPECT_EQ(foldCase("\xC3\x84pfel"), "\xC3\xA4pfel");  // Äpfel -> äpfel
    EXPECT_EQ(foldCase("\xC3\x97"), "\xC3\x97");          // × has no lower case
}

TEST(natural_compare_orders_digit_runs_by_value) {
    EXPECT_TRUE(compareText("s2", "s10", true) < 0);
    EXPECT_TRUE(compareText("s2", "s10", false) > 0);
    EXPECT_TRUE(compareText("a", "B", true) < 0);
    EXPECT_EQ(compareText("x01", "x1", true) == 0, false);
    EXPECT_EQ(compareText("same", "same", true), 0);
}

// ---- Filters ---------------------------------------------------------------------------------

Filter filter(std::string_view expression, bool caseSensitive = false) {
    return Filter::parse(expression, caseSensitive, false);
}

TEST(empty_filter_matches_everything) {
    Filter f = filter("");
    EXPECT_TRUE(f.empty());
    EXPECT_TRUE(f.matches("anything"));
}

TEST(plain_filter_is_case_insensitive_contains) {
    Filter f = filter("graz");
    EXPECT_TRUE(f.matches("City of GRAZ"));
    EXPECT_FALSE(f.matches("Wien"));
}

TEST(filter_folds_umlauts) { EXPECT_TRUE(filter("\xC3\xA4").matches("\xC3\x84pfel")); }

TEST(case_sensitive_filter) {
    EXPECT_FALSE(filter("graz", true).matches("GRAZ"));
    EXPECT_TRUE(filter("GRAZ", true).matches("GRAZ"));
}

TEST(numeric_comparisons) {
    EXPECT_TRUE(filter(">0.5").matches("0.75"));
    EXPECT_FALSE(filter(">0.5").matches("0.5"));
    EXPECT_TRUE(filter(">=0.5").matches("0.5"));
    EXPECT_TRUE(filter("<1e-3").matches("1e-5"));
    EXPECT_TRUE(filter("<=2").matches("2"));
}

TEST(numeric_comparisons_skip_text_and_missing_cells) {
    EXPECT_FALSE(filter(">0").matches("abc"));
    EXPECT_FALSE(filter("<5").matches(""));
    EXPECT_FALSE(filter("<5").matches("NA"));
}

TEST(numeric_comparison_with_decimal_comma) {
    EXPECT_TRUE(Filter::parse(">1,5", false, true).matches("2,5"));
}

TEST(exact_match) {
    EXPECT_TRUE(filter("=abc").matches("ABC"));
    EXPECT_FALSE(filter("=abc").matches("abcd"));
    EXPECT_TRUE(filter("=1").matches("1.0"));
}

TEST(not_equal) {
    EXPECT_FALSE(filter("!=abc").matches("abc"));
    EXPECT_TRUE(filter("!=abc").matches("abd"));
    EXPECT_FALSE(filter("!=1").matches("1.0"));
}

TEST(not_contains) {
    EXPECT_FALSE(filter("!abc").matches("xabcx"));
    EXPECT_TRUE(filter("!abc").matches("xyz"));
}

TEST(and_or_combine_with_and_binding_tighter) {
    Filter range = filter(">1 & <3");
    EXPECT_TRUE(range.matches("2"));
    EXPECT_FALSE(range.matches("4"));

    Filter either = filter("cat | dog");
    EXPECT_TRUE(either.matches("hotdog"));
    EXPECT_FALSE(either.matches("bird"));

    Filter mixed = filter("a & b | c");  // (a & b) | c
    EXPECT_TRUE(mixed.matches("c"));
    EXPECT_TRUE(mixed.matches("ab"));
    EXPECT_FALSE(mixed.matches("a"));
}

TEST(regex_filter) {
    EXPECT_TRUE(filter("/^a.*z$/").matches("abcz"));
    EXPECT_FALSE(filter("/^a.*z$/").matches("abc"));
    EXPECT_FALSE(filter("/ABC/").matches("abc"));
    EXPECT_TRUE(filter("/ABC/i").matches("abc"));
}

TEST(invalid_regex_is_reported_and_matches_everything) {
    Filter f = filter("/(/");
    EXPECT_FALSE(f.valid());
    EXPECT_FALSE(f.error().empty());
    EXPECT_TRUE(f.matches("anything"));
}

TEST(exact_filter_takes_the_value_literally) {
    Filter f = filter("==Food & Drink");
    EXPECT_TRUE(f.matches("Food & Drink"));
    EXPECT_FALSE(f.matches("Food"));
    EXPECT_FALSE(f.matches("Drink"));
    EXPECT_FALSE(f.matches("food & drink"));
    EXPECT_TRUE(filter("==a | b").matches("a | b"));
    EXPECT_TRUE(filter("==/x/").matches("/x/"));
}

TEST(not_exact_filter_is_the_opposite) {
    Filter f = filter("!==Food & Drink");
    EXPECT_FALSE(f.matches("Food & Drink"));
    EXPECT_TRUE(f.matches("Food"));
    EXPECT_TRUE(f.matches("Drink"));
}

TEST(exact_filter_can_match_empty_cells) {
    EXPECT_FALSE(filter("==").empty());
    EXPECT_TRUE(filter("==").matches(""));
    EXPECT_FALSE(filter("==").matches("x"));
    EXPECT_FALSE(filter("!==").matches(""));
    EXPECT_TRUE(filter("!==").matches("x"));
}

TEST(exact_filter_keeps_spaces) {
    EXPECT_TRUE(filter("== a ").matches(" a "));
    EXPECT_FALSE(filter("== a ").matches("a"));
}

TEST(a_lone_operator_is_text) {
    EXPECT_TRUE(filter(">").matches("a>b"));
    EXPECT_FALSE(filter(">").matches("ab"));
}

// ---- Sorting and view ------------------------------------------------------------------------

Table sample() {
    return table("name,score\nb,2\na,10\nc,NA\nd,1\n", plain(",", true));
}

ViewSpec sortBy(size_t column, bool descending, const MissingSet& missing) {
    ViewSpec spec;
    spec.sort.push_back({column, descending});
    spec.missing = &missing;
    return spec;
}

TEST(no_filters_or_sort_shows_all_rows_in_order) {
    EXPECT_EQ(buildView(sample(), ViewSpec{}), (std::vector<size_t>{0, 1, 2, 3}));
}

TEST(sorts_numbers_by_value_with_missing_last) {
    MissingSet missing("NA");
    EXPECT_EQ(buildView(sample(), sortBy(1, false, missing)), (std::vector<size_t>{3, 0, 1, 2}));
}

TEST(descending_sort_still_keeps_missing_last) {
    MissingSet missing("NA");
    EXPECT_EQ(buildView(sample(), sortBy(1, true, missing)), (std::vector<size_t>{1, 0, 3, 2}));
}

TEST(sorts_text) {
    MissingSet missing;
    EXPECT_EQ(buildView(sample(), sortBy(0, false, missing)), (std::vector<size_t>{1, 0, 2, 3}));
}

TEST(natural_sort_is_used_for_sample_names) {
    Table t = table("s10\ns2\ns1\n");
    MissingSet missing;
    EXPECT_EQ(buildView(t, sortBy(0, false, missing)), (std::vector<size_t>{2, 1, 0}));
}

TEST(numbers_sort_before_text) {
    Table t = table("abc\n5\n1\n");
    MissingSet missing;
    EXPECT_EQ(buildView(t, sortBy(0, false, missing)), (std::vector<size_t>{2, 1, 0}));
}

TEST(sort_is_stable_and_multi_key) {
    Table t = table("a,2\nb,1\na,1\nb,2\n");
    MissingSet missing;
    ViewSpec spec;
    spec.sort = {{0, false}, {1, true}};
    spec.missing = &missing;
    EXPECT_EQ(buildView(t, spec), (std::vector<size_t>{0, 2, 3, 1}));
}

TEST(short_rows_sort_as_missing) {
    Table t = table("1,x\n2\n0,y\n");
    MissingSet missing;
    EXPECT_EQ(buildView(t, sortBy(1, false, missing)), (std::vector<size_t>{0, 2, 1}));
}

TEST(sorts_cells_with_escaped_quotes_by_content) {
    Table t = table("\"b\"\"x\"\n\"a\"\"y\"\n");
    MissingSet missing;
    EXPECT_EQ(buildView(t, sortBy(0, false, missing)), (std::vector<size_t>{1, 0}));
}

TEST(column_filter_hides_rows) {
    ViewSpec spec;
    spec.columnFilters.emplace_back(1, filter(">1"));
    EXPECT_EQ(buildView(sample(), spec), (std::vector<size_t>{0, 1}));
}

TEST(global_filter_searches_every_column) {
    ViewSpec spec;
    spec.globalFilter = filter("10");
    EXPECT_EQ(buildView(sample(), spec), (std::vector<size_t>{1}));
}

TEST(filters_stack) {
    ViewSpec spec;
    spec.columnFilters.emplace_back(1, filter(">1"));
    spec.globalFilter = filter("b");
    EXPECT_EQ(buildView(sample(), spec), (std::vector<size_t>{0}));
}

TEST(filters_and_sort_combine) {
    MissingSet missing("NA");
    ViewSpec spec = sortBy(1, true, missing);
    spec.columnFilters.emplace_back(1, filter(">=1"));
    EXPECT_EQ(buildView(sample(), spec), (std::vector<size_t>{1, 0, 3}));
}

TEST(column_filter_on_a_short_row_sees_an_empty_cell) {
    Table t = table("1,x\n2\n");
    ViewSpec spec;
    spec.columnFilters.emplace_back(1, filter("x"));
    EXPECT_EQ(buildView(t, spec), (std::vector<size_t>{0}));
}

TEST(column_stats_ignore_missing_and_infinite_values) {
    Table t = table("v\n1\nNA\n5\nInf\n\n3\n", plain(",", true));
    MissingSet missing("NA");
    auto stats = computeStats(t, missing, 0);
    EXPECT_EQ(stats[0].numbers, 4u);
    EXPECT_EQ(stats[0].missing, 1u);
    EXPECT_TRUE(stats[0].numeric());
    EXPECT_NEAR(stats[0].min, 1);
    EXPECT_NEAR(stats[0].max, 5);
}

TEST(column_with_mostly_text_is_not_numeric) {
    Table t = table("v\na\nb\n1\n", plain(",", true));
    EXPECT_FALSE(computeStats(t, MissingSet{}, 0)[0].numeric());
}

TEST(column_stats_respect_the_row_limit) {
    Table t = table("1\n2\n3\n4\n");
    EXPECT_EQ(computeStats(t, MissingSet{}, 2)[0].numbers, 2u);
}

TEST(selection_stats_summarise_numbers_only) {
    Table t = table("1,a\nNA,b\n3,c\n");
    MissingSet missing("NA");
    SelectionStats s = computeSelectionStats(t, nullptr, 0, 2, {0, 1}, missing);
    EXPECT_EQ(s.cells, 6u);
    EXPECT_EQ(s.numbers, 2u);
    EXPECT_NEAR(s.sum, 4);
    EXPECT_NEAR(s.mean(), 2);
    EXPECT_NEAR(s.min, 1);
    EXPECT_NEAR(s.max, 3);
}

TEST(selection_stats_follow_the_view) {
    Table t = table("1\n2\n3\n");
    std::vector<size_t> view{2, 0};
    SelectionStats s = computeSelectionStats(t, &view, 0, 0, {0}, MissingSet{});
    EXPECT_NEAR(s.sum, 3);
}

// ---- Export ----------------------------------------------------------------------------------

TEST(csv_export_quotes_what_needs_it) {
    Table t = table("x,y\n\"a,b\",\"c\"\"d\"\n", plain(",", true));
    EXPECT_EQ(exportTable(t, nullptr, 0, 0, {0, 1}, ExportFormat::Csv, true), "x,y\r\n\"a,b\",\"c\"\"d\"\r\n");
}

TEST(tsv_export_uses_the_view_and_column_order) {
    Table t = table("x,y\n1,2\n3,4\n", plain(",", true));
    std::vector<size_t> view{1, 0};
    EXPECT_EQ(exportTable(t, &view, 0, 1, {1, 0}, ExportFormat::Tsv, true), "y\tx\r\n4\t3\r\n2\t1\r\n");
}

TEST(tsv_export_quotes_tabs_and_newlines) {
    Table t = table("\"a\tb\"\n");
    EXPECT_EQ(exportTable(t, nullptr, 0, 0, {0}, ExportFormat::Tsv, false), "\"a\tb\"\r\n");
}

TEST(markdown_export_escapes_pipes) {
    Table t = table("x,y\na|b,c\n", plain(",", true));
    EXPECT_EQ(exportTable(t, nullptr, 0, 0, {0, 1}, ExportFormat::Markdown, true),
              "| x | y |\r\n| --- | --- |\r\n| a\\|b | c |\r\n");
}

TEST(markdown_export_flattens_newlines) {
    Table t = table("\"a\nb\"\n");
    EXPECT_EQ(exportTable(t, nullptr, 0, 0, {0}, ExportFormat::Markdown, false), "| a<br>b |\r\n");
}

// ---- Settings --------------------------------------------------------------------------------

TEST(ini_round_trip) {
    Ini ini;
    ini.set("General", "font", "Segoe UI");
    ini.set("D:\\data\\file[1].tsv", "value", "a=b;c");
    Ini back;
    back.parse(ini.serialize());
    EXPECT_EQ(back.get("General", "font"), "Segoe UI");
    EXPECT_EQ(back.get("D:\\data\\file[1].tsv", "value"), "a=b;c");
}

TEST(ini_set_replaces_values) {
    Ini ini;
    ini.set("s", "k", "1");
    ini.set("s", "k", "2");
    EXPECT_EQ(ini.get("s", "k"), "2");
    EXPECT_EQ(ini.sections().size(), 1u);
}

TEST(ini_fallback_and_remove) {
    Ini ini;
    EXPECT_EQ(ini.get("s", "k", "d"), "d");
    ini.set("s", "k", "1");
    EXPECT_TRUE(ini.hasSection("s"));
    ini.removeSection("s");
    EXPECT_FALSE(ini.hasSection("s"));
}

TEST(ini_keeps_tabs_and_leading_spaces_in_values) {
    Ini ini;
    ini.set("s", "tab", "\t");
    ini.set("s", "space", " x");
    Ini back;
    back.parse(ini.serialize());
    EXPECT_EQ(back.get("s", "tab"), "\t");
    EXPECT_EQ(back.get("s", "space"), " x");
}

TEST(ini_survives_garbage) {
    Ini ini;
    ini.parse("no section\n=empty\n[unterminated\n[ok]\nk=v\n");
    EXPECT_EQ(ini.get("ok", "k"), "v");
}

TEST(options_round_trip) {
    Options o;
    o.fontFace = "Consolas";
    o.fontSize = 11;
    o.zebra = false;
    o.decimals = 3;
    o.colorScale = ColorScale::WholeTable;
    o.theme = Theme::Dark;
    o.light.zebra = rgb(1, 2, 3);
    o.parse.delimiter = "\t";
    o.parse.header = Tri::No;
    o.parse.quote = 0;
    o.scaleHigh = rgb(9, 8, 7);

    Ini ini;
    saveOptions(o, ini);
    Ini back;
    back.parse(ini.serialize());
    Options r;
    loadOptions(back, r);

    EXPECT_EQ(r.fontFace, "Consolas");
    EXPECT_EQ(r.fontSize, 11);
    EXPECT_FALSE(r.zebra);
    EXPECT_EQ(r.decimals, 3);
    EXPECT_TRUE(r.colorScale == ColorScale::WholeTable);
    EXPECT_TRUE(r.theme == Theme::Dark);
    EXPECT_EQ(r.light.zebra, rgb(1, 2, 3));
    EXPECT_EQ(r.parse.delimiter, "\t");
    EXPECT_TRUE(r.parse.header == Tri::No);
    EXPECT_EQ(static_cast<int>(r.parse.quote), 0);
    EXPECT_EQ(r.scaleHigh, rgb(9, 8, 7));
}

TEST(missing_option_keys_keep_their_defaults) {
    Ini ini;
    ini.set("Appearance", "fontSize", "14");
    Options r;
    loadOptions(ini, r);
    EXPECT_EQ(r.fontSize, 14);
    EXPECT_EQ(r.fontFace, Options{}.fontFace);
    EXPECT_EQ(r.light.background, kLightPalette.background);
}

TEST(out_of_range_options_are_clamped) {
    Ini ini;
    ini.set("Appearance", "fontSize", "0");
    ini.set("Appearance", "linesPerRow", "-5");
    ini.set("Appearance", "rowPadding", "abc");
    Options r;
    loadOptions(ini, r);
    EXPECT_TRUE(r.fontSize >= 4);
    EXPECT_TRUE(r.linesPerRow >= 1);
    EXPECT_EQ(r.rowPadding, Options{}.rowPadding);
}

TEST(layout_round_trip) {
    DocLayout l;
    l.hasParse = true;
    l.parse.delimiter = ";";
    l.parse.skipLines = 3;
    l.frozenColumns = 2;
    l.order = {2, 0, 1};
    l.columns.resize(3);
    l.columns[0].width = 120;
    l.columns[1].hidden = true;
    l.columns[2].align = Align::Right;
    l.columns[2].decimals = 4;

    Ini ini;
    saveLayout(l, ini, "file.csv");
    DocLayout r;
    EXPECT_TRUE(loadLayout(ini, "file.csv", r));
    EXPECT_TRUE(r.hasParse);
    EXPECT_EQ(r.parse.delimiter, ";");
    EXPECT_EQ(r.parse.skipLines, 3u);
    EXPECT_EQ(r.frozenColumns, 2);
    EXPECT_TRUE(r.order == (std::vector<int>{2, 0, 1}));
    EXPECT_EQ(r.columns[0].width, 120);
    EXPECT_TRUE(r.columns[1].hidden);
    EXPECT_TRUE(r.columns[2].align == Align::Right);
    EXPECT_EQ(r.columns[2].decimals, 4);
}

TEST(layout_for_unknown_file_is_not_found) {
    Ini ini;
    DocLayout r;
    EXPECT_FALSE(loadLayout(ini, "nope", r));
}

TEST(corrupt_layout_order_is_dropped) {
    Ini ini;
    ini.set("f", "columns", "3");
    ini.set("f", "order", "9,x,1");
    DocLayout r;
    EXPECT_TRUE(loadLayout(ini, "f", r));
    EXPECT_TRUE(r.order.empty());
}

// ---- Driver ----------------------------------------------------------------------------------

static void perf() {
    // 2 million rows x 12 columns, about 180 MB: well beyond what a reader normally opens.
    std::string text = "id\tgroup\tv1\tv2\tv3\tv4\tv5\tv6\tv7\tv8\tv9\tv10\n";
    text.reserve(200u * 1024 * 1024);
    char line[256];
    for (int i = 0; i < 2000000; ++i) {
        int n = std::snprintf(line, sizeof line, "row%d\tg%d\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\n", i,
                              i % 7, i * 0.001, i * 0.002, i * 0.003, i * 0.004, i * 0.005, i * 0.006, i * 0.007,
                              i * 0.008, i * 0.009, i * 0.010);
        text.append(line, static_cast<size_t>(n));
    }
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point from) {
        return std::chrono::duration<double, std::milli>(clock::now() - from).count();
    };
    std::printf("perf: %.1f MB\n", text.size() / (1024.0 * 1024.0));
    auto t0 = clock::now();
    Dialect d = detectDialect(text, ParseOptions{}, "tsv");
    std::printf("  detect          %8.1f ms\n", ms(t0));
    Table t;
    t0 = clock::now();
    t.load(text, d);
    std::printf("  load+index      %8.1f ms (%zu rows, %zu cols)\n", ms(t0), t.rowCount(), t.columnCount());
    MissingSet missing("NA");
    ViewSpec spec;
    spec.sort.push_back({3, true});
    spec.missing = &missing;
    t0 = clock::now();
    auto sorted = buildView(t, spec);
    std::printf("  sort numeric    %8.1f ms (%zu)\n", ms(t0), sorted.size());
    ViewSpec textSort;
    textSort.sort.push_back({0, true});
    textSort.missing = &missing;
    t0 = clock::now();
    auto sortedText = buildView(t, textSort);
    std::printf("  sort natural    %8.1f ms (%zu)\n", ms(t0), sortedText.size());
    ViewSpec filtered;
    filtered.columnFilters.emplace_back(2, Filter::parse(">1000 & <1500", false, false));
    t0 = clock::now();
    auto hits = buildView(t, filtered);
    std::printf("  column filter   %8.1f ms (%zu hits)\n", ms(t0), hits.size());
    ViewSpec global;
    global.globalFilter = Filter::parse("row1999", false, false);
    t0 = clock::now();
    auto hits2 = buildView(t, global);
    std::printf("  global filter   %8.1f ms (%zu hits)\n", ms(t0), hits2.size());
    t0 = clock::now();
    auto stats = computeStats(t, missing, 0);
    std::printf("  full stats      %8.1f ms\n", ms(t0));
}

// A crash inside one test is reported as that test's failure instead of ending the run.
static bool runGuarded(const TestCase& test) {
    __try {
        test.body();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::strcmp(argv[1], "--perf") == 0) {
        perf();
        return 0;
    }
    for (const TestCase& test : registry()) {
        g_current = test.name;
        if (!runGuarded(test)) fail(__FILE__, __LINE__, "crashed");
    }
    std::printf("%zu tests, %d checks, %d failures\n", registry().size(), g_checks, g_failures);
    return g_failures ? 1 : 0;
}
