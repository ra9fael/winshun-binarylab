// Record formats: fixed-length packets (Vivado ILA captures, BRAM dumps)
// decoded into named fields, the hextract model rebuilt on top of logic.h.
//
// A format lists fields in packet order, LSB end first, tightly packed with no
// ABI padding (this is a wire layout, not a C struct, so there is no sizeof /
// alignof here). Every field is read little-endian; "msb-first" reverses each
// whole record before that, which is what an ILA MSB-first display needs. That
// is deliberately separate from the bytes page's le/be word switch.
//
// Same contract as logic.h: no I/O, no OS calls, no exceptions across the API.
// TOML loading lives in formats_toml.h so this header stays dependency-free.
#pragma once

#include "logic.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace binlab {

constexpr int kMaxRecordBits = 4096;     // 512-byte records at most
constexpr std::size_t kMaxFields = 64;
constexpr int kMaxFieldCount = 256;
constexpr std::size_t kMaxColumns = 512;
constexpr std::size_t kMaxRules = 32;
constexpr std::size_t kMaxRecordRows = 24; // query preview rows

// ---- rule AST (compiled in formats.h, evaluated per record) ----------------
struct RuleNode;
using RuleNodePtr = std::shared_ptr<const RuleNode>;

// Source description of a format: what a TOML file or the built-in table says,
// before validation. compileFormat() turns it into a RecordFormat.
struct RawField {
    std::string name;
    std::string type;
    int count = 1;
    bool hasBits = false;
    int bitHi = -1;
    int bitLo = -1;
    std::string radix;   // "", "hex" or "dec"
    bool hasScale = false;
    double scale = 1.0;
};

struct RawRule {
    std::string name;
    std::string when;
    std::string bg;
    std::string fg;
};

struct RawFormat {
    std::string name;
    std::string description;
    std::string source;      // "builtin" or a file name
    int wordBits = 0;
    std::string byteOrder = "msb-first";
    std::vector<RawField> fields;
    std::vector<RawRule> rules;
};

struct FieldSpec {
    std::string name;
    TypeSpec type;           // always little-endian; see the header comment
    int count = 1;
    int bitHi = -1;          // -1: whole value
    int bitLo = -1;
    bool hexRadix = false;
    bool hasScale = false;
    double scale = 1.0;
    int offset = 0;          // byte offset of element 0 inside the record
    int firstColumn = 0;
};

struct RuleSpec {
    std::string name;
    std::string when;
    std::string bg;          // "#rrggbb" or empty
    std::string fg;
    RuleNodePtr code;
};

struct RecordFormat {
    std::string name;
    std::string description;
    std::string source;
    int wordBits = 0;
    bool msbFirst = true;
    std::vector<FieldSpec> fields;
    std::vector<RuleSpec> rules;
    std::vector<std::string> columns;
    std::vector<std::string> warnings;
    int recordBytes() const { return wordBits / 8; }
};

// A format file that failed to load still shows up in the list with its error,
// so a typo in one file never silently hides it.
struct FormatEntry {
    std::string name;
    std::string source;
    bool ok = false;
    std::string error;
    RecordFormat format;
};

struct FormatSet {
    std::vector<FormatEntry> entries;
    std::vector<std::string> notes;   // directory-level messages (overrides, caps)

    const RecordFormat* find(const std::string& name) const
    {
        const std::string n = lower(name);
        for (const FormatEntry& e : entries)
            if (e.ok && lower(e.name) == n)
                return &e.format;
        return nullptr;
    }
    const FormatEntry* entry(const std::string& name) const
    {
        const std::string n = lower(name);
        for (const FormatEntry& e : entries)
            if (lower(e.name) == n)
                return &e;
        return nullptr;
    }
};

// One decoded field element. Integers keep their raw (post-bits) value; the
// display text applies radix and scale. Rules see the raw value.
struct Cell {
    enum Kind { Unsigned, Signed, Float } kind = Unsigned;
    std::uint64_t u = 0;
    std::int64_t i = 0;
    double f = 0;
    std::uint64_t bits = 0;  // the stored bit pattern before any bit slice
    std::string text;
};

struct Record {
    std::size_t offset = 0;  // byte offset of the record in the input
    std::vector<Cell> cells; // one per column
    int rule = -1;           // first matching rule, -1 none
    std::string ruleError;   // first evaluation error, if any
};

struct RecordsEval {
    std::string formatName;
    bool msbFirst = true;
    int recordBytes = 0;
    std::vector<std::uint8_t> data;
    std::vector<Record> records;
    std::size_t trailing = 0;
    int ruleHits = 0;
    int ruleErrors = 0;
    std::string normalized;
    std::string table;        // fixed-width text table, the hextract CLI layout
    std::string preview;
    std::string detail;
    std::vector<std::string> warnings;
};

Outcome<RuleNodePtr> compileRule(const std::string& src, const std::vector<std::string>& fieldNames,
                                 const std::vector<int>& fieldCounts);

// ---- format compilation -----------------------------------------------------

inline bool validColor(const std::string& c)
{
    if (c.size() != 7 || c[0] != '#')
        return false;
    for (std::size_t i = 1; i < 7; ++i)
        if (digitValue(c[i]) < 0)
            return false;
    return true;
}

inline bool validFieldName(const std::string& n)
{
    if (n.empty() || n.size() > 64 || n[0] == '_' || std::isdigit(static_cast<unsigned char>(n[0])))
        return false;
    for (char c : n)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return false;
    static const std::set<std::string> kReserved = {
        "and", "or", "not", "for", "in", "if", "else", "true", "false", "True", "False",
        "any", "all", "min", "max", "abs", "len" };
    return kReserved.count(n) == 0;
}

inline Outcome<RecordFormat> compileFormat(const RawFormat& raw)
{
    const std::string hint = "see README: record formats";
    const auto bad = [&](const std::string& what) {
        return failWith<RecordFormat>(fail("E_FORMAT", (raw.name.empty() ? std::string("format") : raw.name)
                                                         + ": " + what, hint));
    };
    RecordFormat f;
    f.name = raw.name;
    f.description = raw.description;
    f.source = raw.source;
    if (raw.name.empty())
        return bad("format.name missing");
    if (raw.name.size() > 64)
        return bad("format.name longer than 64 characters");
    if (raw.wordBits <= 0 || raw.wordBits % 8 != 0)
        return bad("format.word_bits must be a positive multiple of 8");
    if (raw.wordBits > kMaxRecordBits)
        return bad("format.word_bits exceeds " + std::to_string(kMaxRecordBits));
    f.wordBits = raw.wordBits;
    if (raw.byteOrder == "msb-first")
        f.msbFirst = true;
    else if (raw.byteOrder == "lsb-first")
        f.msbFirst = false;
    else
        return bad("format.byte_order must be 'msb-first' or 'lsb-first'");
    if (raw.fields.empty())
        return bad("at least one [[field]] is required");
    if (raw.fields.size() > kMaxFields)
        return bad("more than " + std::to_string(kMaxFields) + " fields");

    std::set<std::string> seen;
    int offset = 0;
    std::vector<std::string> names;
    std::vector<int> counts;
    for (const RawField& rf : raw.fields) {
        if (!validFieldName(rf.name))
            return bad("field '" + rf.name + "': name must be an identifier (letters, digits, _; not reserved)");
        if (!seen.insert(rf.name).second)
            return bad("field '" + rf.name + "' defined twice");
        FieldSpec fs;
        fs.name = rf.name;
        // Fields are wire data: always little-endian within the (possibly
        // reversed) record, so an le/be suffix is not accepted here.
        const std::string t = lower(rf.type);
        int tw = 0;
        bool ts = false;
        if ((!isBitsType(t, tw, ts) && floatByName(t) == nullptr) || !resolveType(t, "le", fs.type))
            return bad("field '" + rf.name + "': unknown type '" + rf.type + "' (u8..u64 i8..i64 " + floatNameList() + ")");
        if (rf.count < 1 || rf.count > kMaxFieldCount)
            return bad("field '" + rf.name + "': count must be 1.." + std::to_string(kMaxFieldCount));
        fs.count = rf.count;
        const int typeBits = fs.type.size * 8;
        if (rf.hasBits) {
            if (fs.type.flt != nullptr)
                return bad("field '" + rf.name + "': bits needs an integer type");
            if (rf.bitHi < rf.bitLo || rf.bitLo < 0 || rf.bitHi >= typeBits)
                return bad("field '" + rf.name + "': bits " + std::to_string(rf.bitHi) + ":" + std::to_string(rf.bitLo)
                           + " out of range for a " + std::to_string(typeBits) + "-bit type");
            fs.bitHi = rf.bitHi;
            fs.bitLo = rf.bitLo;
        }
        if (!rf.radix.empty() && rf.radix != "hex" && rf.radix != "dec")
            return bad("field '" + rf.name + "': radix must be 'hex' or 'dec'");
        fs.hexRadix = rf.radix == "hex";
        if (rf.hasScale && !std::isfinite(rf.scale))
            return bad("field '" + rf.name + "': scale must be a finite number");
        fs.hasScale = rf.hasScale;
        fs.scale = rf.scale;
        fs.offset = offset;
        fs.firstColumn = static_cast<int>(f.columns.size());
        offset += fs.type.size * fs.count;
        if (offset * 8 > f.wordBits)
            return bad("fields use " + std::to_string(offset * 8) + " bits but word is "
                       + std::to_string(f.wordBits) + " bits");
        if (fs.count == 1)
            f.columns.push_back(fs.name);
        else
            for (int i = 0; i < fs.count; ++i)
                f.columns.push_back(fs.name + std::to_string(i));
        if (f.columns.size() > kMaxColumns)
            return bad("more than " + std::to_string(kMaxColumns) + " columns");
        names.push_back(fs.name);
        counts.push_back(fs.count);
        f.fields.push_back(fs);
    }

    if (raw.rules.size() > kMaxRules)
        return bad("more than " + std::to_string(kMaxRules) + " rules");
    std::map<std::string, std::size_t> seenRules;
    for (std::size_t i = 0; i < raw.rules.size(); ++i) {
        const RawRule& rr = raw.rules[i];
        const std::string label = "rule " + std::to_string(i + 1);
        if (rr.when.empty())
            return bad(label + ": when missing");
        if (rr.bg.empty() && rr.fg.empty())
            return bad(label + ": needs bg and/or fg color");
        if ((!rr.bg.empty() && !validColor(rr.bg)) || (!rr.fg.empty() && !validColor(rr.fg)))
            return bad(label + ": colors must look like #rrggbb");
        auto code = compileRule(rr.when, names, counts);
        if (!code.success)
            return bad(label + ": " + code.failure.message);
        RuleSpec rs;
        rs.name = rr.name.empty() ? "Rule " + std::to_string(i + 1) : rr.name;
        rs.when = rr.when;
        rs.bg = rr.bg;
        rs.fg = rr.fg;
        rs.code = code.value;
        const auto dup = seenRules.find(rr.when);
        if (dup != seenRules.end())
            f.warnings.push_back("rule " + std::to_string(i + 1) + " repeats rule " + std::to_string(dup->second + 1)
                                 + " and can never match first");
        else
            seenRules.emplace(rr.when, i);
        f.rules.push_back(rs);
    }
    return Outcome<RecordFormat>::good(f);
}

// The two packets hextract ships, kept here so the plugin works without any
// format folder. They mirror formats/blm_*.toml field for field; the TOML
// fixture test checks that the two stay equal.
inline std::vector<RawFormat> builtinRawFormats()
{
    const auto field = [](const char* name, const char* type, int count = 1, const char* radix = "") {
        RawField f;
        f.name = name;
        f.type = type;
        f.count = count;
        f.radix = radix;
        return f;
    };
    const auto rule = [](const char* when, const char* bg, const char* fg) {
        RawRule r;
        r.when = when;
        r.bg = bg;
        r.fg = fg;
        return r;
    };
    const std::vector<RawRule> blmRules = {
        rule("any(v < 0 for v in integral)", "#f6caca", ""),
        rule("any(v != 0 for v in count)", "#d3ecd3", ""),
        rule("all(v == 0 for v in integral) and all(v == 0 for v in count)", "", "#9a9a9a"),
    };
    RawFormat raw256;
    raw256.name = "blm_rawdata_256";
    raw256.description = "bram_rawdata_packet (32 B)";
    raw256.source = "builtin";
    raw256.wordBits = 256;
    raw256.byteOrder = "lsb-first";
    raw256.fields = { field("wr_time_s", "u32"), field("wr_time_ns", "u32"),
                      field("integral", "i16", 6), field("count", "u16", 6) };
    raw256.rules = blmRules;

    RawFormat int512;
    int512.name = "blm_interlock_512";
    int512.description = "bram_interlock_packet (64 B)";
    int512.source = "builtin";
    int512.wordBits = 512;
    int512.byteOrder = "msb-first";
    int512.fields = { field("wr_time_s", "u32"), field("wr_time_ns", "u32"),
                      field("data_id", "u32", 1, "hex"), field("lock_addr", "u32", 1, "hex"),
                      field("integral", "i32", 6), field("count", "u32", 6) };
    int512.rules = blmRules;
    return { raw256, int512 };
}

inline FormatEntry entryFor(const RawFormat& raw)
{
    FormatEntry e;
    e.name = raw.name;
    e.source = raw.source;
    const auto c = compileFormat(raw);
    e.ok = c.success;
    if (c.success)
        e.format = c.value;
    else
        e.error = c.failure.message;
    return e;
}

inline FormatSet builtinFormatSet()
{
    FormatSet set;
    for (const RawFormat& raw : builtinRawFormats())
        set.entries.push_back(entryFor(raw));
    return set;
}

// Adds or replaces by name. A user file with a built-in name overrides it and
// says so, instead of two entries the selector cannot tell apart.
inline void addFormat(FormatSet& set, FormatEntry entry)
{
    for (FormatEntry& e : set.entries) {
        if (lower(e.name) == lower(entry.name) && !entry.name.empty()) {
            set.notes.push_back(entry.source + " overrides " + e.source + " format '" + e.name + "'");
            e = std::move(entry);
            return;
        }
    }
    set.entries.push_back(std::move(entry));
}

// ---- strict hex input ---------------------------------------------------------
// Unlike hextract's "delete every non-hex character", only real separators are
// accepted: whitespace , : ; _ and a 0x prefix. An address like "0010:" is fine
// (it is hex), but "ILA[3]" or a comment is an error with its position, so it
// can never be spliced silently into the data.
inline Outcome<std::vector<std::uint8_t>> parseStrictHex(const std::string& text)
{
    const std::string hint = "bytes fmt blm_rawdata_256 01 00 00 00 ...";
    std::string digits;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == ':' || c == ';' || c == '_')
            continue;
        if (c == '0' && i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X')
            && (i == 0 || !std::isxdigit(static_cast<unsigned char>(text[i - 1])))) {
            ++i;
            continue;
        }
        if (digitValue(c) < 0)
            return failWith<std::vector<std::uint8_t>>(fail("E_BAD_TOKEN",
                std::string("not a hex digit or separator at character ") + std::to_string(i + 1) + ": '" + c + "'",
                hint));
        digits += c;
    }
    if (digits.size() % 2 != 0)
        return failWith<std::vector<std::uint8_t>>(fail("E_HEX_ODD", "odd number of hex digits", hint));
    if (digits.size() / 2 > kMaxBytes)
        return failWith<std::vector<std::uint8_t>>(fail("E_BYTES_TOO_LARGE", "more than 4096 decoded bytes", hint));
    std::vector<std::uint8_t> out;
    out.reserve(digits.size() / 2);
    for (std::size_t i = 0; i < digits.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>((digitValue(digits[i]) << 4) | digitValue(digits[i + 1])));
    return Outcome<std::vector<std::uint8_t>>::good(out);
}

// ---- cell formatting ----------------------------------------------------------

// Python "%g": 6 significant digits, trailing zeros dropped.
inline std::string shortG(double v)
{
    if (std::isnan(v))
        return "nan";
    if (std::isinf(v))
        return v > 0 ? "inf" : "-inf";
    char b[48];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

inline std::string hexLower(std::uint64_t v)
{
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// Same display rules as hextract's format_cell: scale first, then radix=hex
// for integers ("-0x1" for negatives), %g for floats, plain decimal otherwise.
inline std::string cellText(const FieldSpec& f, const Cell& c)
{
    if (f.hasScale) {
        const double base = c.kind == Cell::Float ? c.f : c.kind == Cell::Signed ? static_cast<double>(c.i)
                                                                                  : static_cast<double>(c.u);
        const double scaled = base * f.scale;
        // An integer times an integral scale stays an integer in Python.
        if (c.kind != Cell::Float && std::floor(f.scale) == f.scale && std::fabs(scaled) < 9.0e15) {
            const long long n = static_cast<long long>(scaled);
            if (f.hexRadix)
                return n < 0 ? "-" + hexLower(static_cast<std::uint64_t>(-n)) : hexLower(static_cast<std::uint64_t>(n));
            return std::to_string(n);
        }
        return shortG(scaled);
    }
    if (c.kind == Cell::Float)
        return shortG(c.f);
    if (c.kind == Cell::Signed) {
        if (f.hexRadix)
            return c.i < 0 ? "-" + hexLower(static_cast<std::uint64_t>(0) - static_cast<std::uint64_t>(c.i))
                           : hexLower(static_cast<std::uint64_t>(c.i));
        return std::to_string(c.i);
    }
    return f.hexRadix ? hexLower(c.u) : std::to_string(c.u);
}

inline Cell decodeCell(const std::uint8_t* p, const FieldSpec& f)
{
    std::uint64_t raw = 0;
    for (int i = 0; i < f.type.size; ++i)
        raw |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    Cell c;
    c.bits = raw;
    const int width = f.type.size * 8;
    if (f.type.flt != nullptr) {
        c.kind = Cell::Float;
        c.f = floatToDouble(raw, *f.type.flt);
    } else if (f.bitHi >= 0) {
        // Python reads the signed value first and then masks, so a slice of a
        // signed field is always a non-negative number; mirror that exactly.
        const std::uint64_t v = f.type.signedType ? static_cast<std::uint64_t>(asSigned(raw, width)) : raw;
        c.kind = Cell::Unsigned;
        c.u = (v >> f.bitLo) & maskFor(f.bitHi - f.bitLo + 1);
    } else if (f.type.signedType) {
        c.kind = Cell::Signed;
        c.i = asSigned(raw, width);
    } else {
        c.kind = Cell::Unsigned;
        c.u = raw;
    }
    c.text = cellText(f, c);
    return c;
}

int evalRuleOn(const RecordFormat& f, const Record& r, std::string& error);

inline std::string recordsTable(const RecordFormat& f, const std::vector<Record>& rows, std::size_t maxRows)
{
    std::vector<std::string> header = { "#" };
    header.insert(header.end(), f.columns.begin(), f.columns.end());
    const std::size_t shown = std::min(rows.size(), maxRows);
    std::vector<std::size_t> widths(header.size());
    for (std::size_t c = 0; c < header.size(); ++c)
        widths[c] = header[c].size();
    for (std::size_t r = 0; r < shown; ++r) {
        widths[0] = std::max(widths[0], std::to_string(r).size());
        for (std::size_t c = 0; c < rows[r].cells.size(); ++c)
            widths[c + 1] = std::max(widths[c + 1], rows[r].cells[c].text.size());
    }
    const auto pad = [](const std::string& s, std::size_t w) { return std::string(w - std::min(w, s.size()), ' ') + s; };
    std::ostringstream out;
    if (shown > 0) {
        for (std::size_t c = 0; c < header.size(); ++c)
            out << (c ? "  " : "") << pad(header[c], widths[c]);
        out << "\n";
    }
    for (std::size_t r = 0; r < shown; ++r) {
        out << pad(std::to_string(r), widths[0]);
        for (std::size_t c = 0; c < rows[r].cells.size(); ++c)
            out << "  " << pad(rows[r].cells[c].text, widths[c + 1]);
        if (rows[r].rule >= 0)
            out << "  [" << f.rules[static_cast<std::size_t>(rows[r].rule)].name << "]";
        out << "\n";
    }
    if (rows.size() > shown)
        out << "... " << (rows.size() - shown) << " more records\n";
    return out.str();
}

// Decode `data` with `f`. Split out of evalRecords() so the GUI and tests can
// decode bytes they already hold.
inline RecordsEval decodeRecords(const RecordFormat& f, bool msbFirst, std::vector<std::uint8_t> data)
{
    RecordsEval e;
    e.formatName = f.name;
    e.msbFirst = msbFirst;
    e.recordBytes = f.recordBytes();
    e.data = std::move(data);
    e.warnings = f.warnings;
    const std::size_t rb = static_cast<std::size_t>(e.recordBytes);
    std::vector<std::uint8_t> chunk(rb);
    for (std::size_t off = 0; off + rb <= e.data.size(); off += rb) {
        for (std::size_t i = 0; i < rb; ++i)
            chunk[i] = msbFirst ? e.data[off + rb - 1 - i] : e.data[off + i];
        Record r;
        r.offset = off;
        for (const FieldSpec& fs : f.fields)
            for (int k = 0; k < fs.count; ++k)
                r.cells.push_back(decodeCell(chunk.data() + fs.offset + k * fs.type.size, fs));
        r.rule = evalRuleOn(f, r, r.ruleError);
        if (r.rule >= 0)
            ++e.ruleHits;
        if (!r.ruleError.empty())
            ++e.ruleErrors;
        e.records.push_back(std::move(r));
    }
    e.trailing = e.data.size() % rb;

    std::ostringstream n;
    for (std::size_t i = 0; i < e.data.size(); ++i) {
        char cell[4];
        std::snprintf(cell, sizeof cell, "%02X", e.data[i]);
        n << (i ? " " : "") << cell;
    }
    e.normalized = n.str();
    e.table = recordsTable(f, e.records, 256);
    std::ostringstream head;
    head << f.name << " · " << (msbFirst ? "msb-first" : "lsb-first") << " · " << e.recordBytes << " B/record · "
         << e.records.size() << " records · trailing " << e.trailing;
    if (!f.rules.empty())
        head << " · rules " << e.ruleHits << " hit / " << e.ruleErrors << " error";
    head << "\n";
    for (const std::string& w : e.warnings)
        head << "warning: " << w << "\n";
    e.preview = head.str() + recordsTable(f, e.records, kMaxRecordRows);
    e.detail = head.str() + e.table;
    if (e.trailing)
        e.detail += std::to_string(e.trailing) + " trailing bytes not decoded (a record needs "
                    + std::to_string(e.recordBytes) + ")\n";
    return e;
}

// `fmt <name> [lsb|msb] <hex...>`. The order word is optional and defaults to
// the format's byte_order.
inline Outcome<RecordsEval> evalRecords(const std::string& input, const FormatSet& formats)
{
    const std::string hint = "bytes fmt blm_rawdata_256 01 00 00 00 ...";
    if (input.size() > kMaxInputChars)
        return failWith<RecordsEval>(fail("E_INPUT_TOO_LARGE", "input exceeds 16 KiB", hint));
    std::vector<std::string> tokens = splitWhitespace(input);
    if (tokens.empty() || lower(tokens[0]) != "fmt")
        return failWith<RecordsEval>(fail("E_SYNTAX", "a record decode starts with 'fmt <name>'", hint));
    if (tokens.size() < 2)
        return failWith<RecordsEval>(fail("E_FORMAT_NAME", "no format name after fmt", hint));
    const FormatEntry* entry = formats.entry(tokens[1]);
    if (entry == nullptr)
        return failWith<RecordsEval>(fail("E_FORMAT_NAME", "unknown record format '" + tokens[1] + "'",
                                          "bytes fmt  (lists the formats)"));
    if (!entry->ok)
        return failWith<RecordsEval>(fail("E_FORMAT", entry->source + ": " + entry->error, hint));
    const RecordFormat& f = entry->format;
    bool msb = f.msbFirst;
    std::size_t at = 2;
    if (at < tokens.size()) {
        const std::string o = lower(tokens[at]);
        if (o == "msb" || o == "msb-first") {
            msb = true;
            ++at;
        } else if (o == "lsb" || o == "lsb-first") {
            msb = false;
            ++at;
        }
    }
    std::string hexText;
    for (std::size_t i = at; i < tokens.size(); ++i)
        hexText += (hexText.empty() ? "" : " ") + tokens[i];
    auto data = parseStrictHex(hexText);
    if (!data.success)
        return failWith<RecordsEval>(data.failure);
    if (data.value.empty())
        return failWith<RecordsEval>(fail("E_EMPTY", "no record bytes after the format name", hint));
    if (data.value.size() < static_cast<std::size_t>(f.recordBytes()))
        return failWith<RecordsEval>(fail("E_SHORT_RECORD",
            "need " + std::to_string(f.recordBytes()) + " B per record (" + std::to_string(f.wordBits) + "-bit), got "
                + std::to_string(data.value.size()) + " B", hint));
    return Outcome<RecordsEval>::good(decodeRecords(f, msb, std::move(data.value)));
}

}  // namespace binlab

#include "formats_rules.h"
