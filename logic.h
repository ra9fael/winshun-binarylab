// logic.h - pure, bounded computation for the Binary Lab WinShun plugin.
//
// This header has no OS dependencies, performs no I/O, holds no global mutable
// state, and never throws across its public API. Every entry point is a
// deterministic function of its arguments. main.cpp is the only file that
// talks to the host SDK. Keeping the logic separate makes it possible to test
// the parsers without starting a plugin process.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace binlab {

// Bounds. The host caps a query at timeoutMs and a response line at 16 MB;
// these caps keep every parse well inside both.
constexpr std::size_t kMaxInputChars = 16 * 1024;
constexpr std::size_t kMaxBytes = 4096;
constexpr std::size_t kMaxWordRows = 24;
constexpr std::size_t kMaxMembers = 256;
constexpr std::size_t kMaxDefinitions = 16;
constexpr int kMaxNesting = 8;
constexpr std::uint64_t kMaxStructBytes = 4096;
constexpr std::uint64_t kMaxArrayElements = 4096;

struct Failure {
    std::string code;
    std::string message;
    std::string hint;
};

inline Failure fail(std::string code, std::string message, std::string hint)
{
    return Failure {std::move(code), std::move(message), std::move(hint)};
}

template <class T>
struct Outcome {
    bool success = false;
    T value {};
    Failure failure {};

    static Outcome good(T v)
    {
        Outcome o;
        o.success = true;
        o.value = std::move(v);
        return o;
    }
    static Outcome bad(Failure f)
    {
        Outcome o;
        o.failure = std::move(f);
        return o;
    }
};

template <class T>
inline Outcome<T> failWith(Failure f)
{
    return Outcome<T>::bad(std::move(f));
}

// ---- small text and number helpers ------------------------------------------

inline std::string lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string trim(const std::string& s)
{
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

inline std::vector<std::string> splitWhitespace(const std::string& s)
{
    std::vector<std::string> out;
    std::string current;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty())
        out.push_back(current);
    return out;
}

inline bool startsWith(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

inline bool isDecimal(const std::string& s)
{
    if (s.empty())
        return false;
    for (char c : s)
        if (c < '0' || c > '9')
            return false;
    return true;
}

inline bool parseDecimal(const std::string& s, std::uint64_t limit, std::uint64_t& out)
{
    if (!isDecimal(s))
        return false;
    std::uint64_t v = 0;
    for (char c : s) {
        const std::uint64_t d = static_cast<std::uint64_t>(c - '0');
        if (v > (limit - d) / 10)
            return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}

inline bool isIdentifier(const std::string& s)
{
    if (s.empty())
        return false;
    const auto first = static_cast<unsigned char>(s[0]);
    if (!(std::isalpha(first) || s[0] == '_'))
        return false;
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '_'))
            return false;
    }
    return true;
}

inline std::uint64_t maskFor(int width)
{
    if (width <= 0)
        return 0;
    if (width >= 64)
        return ~std::uint64_t(0);
    return (std::uint64_t(1) << width) - 1;
}

inline std::int64_t asSigned(std::uint64_t bits, int width)
{
    const std::uint64_t m = maskFor(width);
    std::uint64_t full = bits & m;
    if (width > 0 && width < 64 && (full & (std::uint64_t(1) << (width - 1))))
        full |= ~m;
    std::int64_t result = 0;
    std::memcpy(&result, &full, sizeof result);
    return result;
}

inline std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment, bool& ok)
{
    if (alignment == 0) {
        ok = false;
        return 0;
    }
    const std::uint64_t remainder = value % alignment;
    if (remainder == 0) {
        ok = true;
        return value;
    }
    const std::uint64_t add = alignment - remainder;
    if (value > std::numeric_limits<std::uint64_t>::max() - add) {
        ok = false;
        return 0;
    }
    ok = true;
    return value + add;
}

inline std::string hexUpper(std::uint64_t bits, int width)
{
    static const char digits[] = "0123456789ABCDEF";
    const int nibbles = std::max(1, (width + 3) / 4);
    std::string s;
    s.reserve(static_cast<std::size_t>(nibbles) + 2);
    for (int i = nibbles - 1; i >= 0; --i)
        s += digits[(bits >> (4 * i)) & 0xF];
    return "0x" + s;
}

inline std::string binPlain(std::uint64_t bits, int width)
{
    std::string s;
    s.reserve(static_cast<std::size_t>(width) + 2);
    for (int i = width - 1; i >= 0; --i)
        s += ((bits >> i) & 1) ? '1' : '0';
    return "0b" + s;
}

inline std::string binGrouped(std::uint64_t bits, int width)
{
    std::string s;
    int emitted = 0;
    for (int i = width - 1; i >= 0; --i) {
        if (emitted && emitted % 4 == 0)
            s += '_';
        s += ((bits >> i) & 1) ? '1' : '0';
        ++emitted;
    }
    return s;
}

inline std::string octal(std::uint64_t bits, int width)
{
    const int digits = std::max(1, (width + 2) / 3);
    std::string s;
    for (int i = digits - 1; i >= 0; --i)
        s += static_cast<char>('0' + ((bits >> (3 * i)) & 7));
    return "0o" + s;
}

struct Repr {
    std::string hex;
    std::string bin;
    std::string oct;
    std::string dec;
    std::string unsignedDec;
    std::string signedDec;
};

inline Repr represent(std::uint64_t bits, int width, bool signedType)
{
    const std::uint64_t m = maskFor(width);
    const std::uint64_t v = bits & m;
    Repr r;
    r.hex = hexUpper(v, width);
    r.bin = binPlain(v, width);
    r.oct = octal(v, width);
    r.unsignedDec = std::to_string(v);
    r.signedDec = std::to_string(asSigned(v, width));
    r.dec = signedType ? r.signedDec : r.unsignedDec;
    return r;
}

inline int digitValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

inline std::string typeNameFor(int width, bool signedType)
{
    return std::string(signedType ? "i" : "u") + std::to_string(width);
}

inline std::string bitGrid(int width, std::uint64_t lhs, std::uint64_t rhs,
                           const std::string& op, std::uint64_t result, bool hasRhs)
{
    const std::uint64_t m = maskFor(width);
    lhs &= m;
    rhs &= m;
    result &= m;
    std::ostringstream out;
    for (int hi = width - 1; hi >= 0; hi -= 8) {
        const int lo = std::max(0, hi - 7);
        out << "bits [" << hi << ":" << lo << "]\n";
        const auto row = [&](const char* label, std::uint64_t v) {
            out << "  " << label << ":";
            for (int b = hi; b >= lo; --b)
                out << ' ' << (((v >> b) & 1) ? '1' : '0');
            out << "\n";
        };
        row("A", lhs);
        if (hasRhs)
            row("B", rhs);
        row("=", result);
    }
    out << "op " << op << "  A=" << hexUpper(lhs, width) << "  result=" << hexUpper(result, width) << "\n";
    return out.str();
}

// A literal is parsed into a raw bit pattern of `width` bits.
//   @0xFF  raw bit pattern (range checks against the width still apply)
//   -2     allowed only for signed types
inline Outcome<std::uint64_t> parseLiteral(const std::string& tokenIn, int width, bool signedType)
{
    const std::string hint = "bits u8 0xA5 & 0x0F";
    std::string token = tokenIn;
    bool raw = false;
    if (!token.empty() && token[0] == '@') {
        raw = true;
        token.erase(0, 1);
    }
    bool negative = false;
    if (!token.empty() && (token[0] == '-' || token[0] == '+')) {
        negative = token[0] == '-';
        token.erase(0, 1);
    }
    if (raw && negative)
        return failWith<std::uint64_t>(fail("E_BAD_LITERAL", "a raw bit pattern cannot have a sign", hint));
    if (width < 1 || width > 64)
        return failWith<std::uint64_t>(fail("E_BAD_WIDTH", "width must be 8, 16, 32 or 64", hint));

    int base = 10;
    std::string digits = token;
    if (token.size() >= 2 && token[0] == '0') {
        const char p = static_cast<char>(std::tolower(static_cast<unsigned char>(token[1])));
        if (p == 'x') {
            base = 16;
            digits = token.substr(2);
        } else if (p == 'b') {
            base = 2;
            digits = token.substr(2);
        } else if (p == 'o') {
            base = 8;
            digits = token.substr(2);
        }
    }
    std::string clean;
    for (char c : digits)
        if (c != '_')
            clean += c;
    if (clean.empty())
        return failWith<std::uint64_t>(fail("E_BAD_LITERAL", "a number has no digits", hint));

    std::uint64_t value = 0;
    for (char c : clean) {
        const int d = digitValue(c);
        if (d < 0 || d >= base)
            return failWith<std::uint64_t>(fail("E_BAD_LITERAL", "invalid digit for the base", hint));
        const std::uint64_t maximum = maskFor(64);
        if (value > (maximum - static_cast<std::uint64_t>(d)) / static_cast<std::uint64_t>(base))
            return failWith<std::uint64_t>(fail("E_OVERFLOW", "literal exceeds 64 bits", hint));
        value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
    }

    const std::uint64_t m = maskFor(width);
    if (value > m)
        return failWith<std::uint64_t>(fail("E_RANGE", "literal does not fit in the selected width", hint));

    if (signedType && !raw) {
        const std::uint64_t sign = std::uint64_t(1) << (width - 1);
        if (negative) {
            if (value > sign)
                return failWith<std::uint64_t>(fail("E_RANGE", "negative literal is out of range", "bits i8 -2 >> 1"));
            return Outcome<std::uint64_t>::good((std::uint64_t(0) - value) & m);
        }
        if (value >= sign)
            return failWith<std::uint64_t>(fail("E_RANGE", "positive literal is out of signed range", "bits i8 127 & 0x0F"));
        return Outcome<std::uint64_t>::good(value & m);
    }
    if (negative)
        return failWith<std::uint64_t>(fail("E_NEGATIVE_UNSIGNED", "unsigned types cannot take a negative literal", hint));
    return Outcome<std::uint64_t>::good(value & m);
}

// ---- bits: numeric literals and bit operations --------------------------------

inline std::vector<std::string> lexBits(const std::string& s)
{
    std::vector<std::string> tokens;
    std::string current;
    const auto flush = [&]() {
        if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            flush();
        } else if (c == '&' || c == '|' || c == '^' || c == '~') {
            flush();
            tokens.push_back(std::string(1, c));
        } else if (c == '<' || c == '>') {
            flush();
            if (i + 1 < s.size() && s[i + 1] == c) {
                tokens.push_back(std::string(2, c));
                ++i;
            } else {
                tokens.push_back(std::string(1, c));
            }
        } else {
            current += c;
        }
    }
    flush();
    return tokens;
}

inline bool isBitsType(const std::string& t, int& width, bool& signedType)
{
    const std::string s = lower(t);
    if (s.size() < 2 || (s[0] != 'u' && s[0] != 'i'))
        return false;
    const std::string digits = s.substr(1);
    if (digits != "8" && digits != "16" && digits != "32" && digits != "64")
        return false;
    width = std::stoi(digits);
    signedType = s[0] == 'i';
    return true;
}

struct BitsEval {
    int width = 32;
    bool signedType = false;
    std::string typeName = "u32";
    std::uint64_t result = 0;
    std::uint64_t lhs = 0;
    std::uint64_t rhs = 0;
    bool hasRhs = false;
    std::string op = "value";
    std::string expr;
    std::string detail;
    std::string preview;
    std::string chain;
};

inline Outcome<std::pair<int, int>> parseBitRange(const std::string& token, int width)
{
    const std::string hint = "bits u8 get 0xA5 7:4";
    const std::size_t colon = token.find(':');
    if (colon == std::string::npos || token.find(':', colon + 1) != std::string::npos)
        return failWith<std::pair<int, int>>(fail("E_BAD_RANGE", "a bit range must look like 7:4", hint));
    auto hi = parseLiteral(token.substr(0, colon), 64, false);
    if (!hi.success)
        return failWith<std::pair<int, int>>(hi.failure);
    auto lo = parseLiteral(token.substr(colon + 1), 64, false);
    if (!lo.success)
        return failWith<std::pair<int, int>>(lo.failure);
    if (hi.value > 63 || lo.value > 63 || lo.value > hi.value || hi.value >= static_cast<std::uint64_t>(width))
        return failWith<std::pair<int, int>>(fail("E_BAD_RANGE", "bit range is outside the selected width", hint));
    return Outcome<std::pair<int, int>>::good({static_cast<int>(hi.value), static_cast<int>(lo.value)});
}

inline Outcome<BitsEval> evalBits(const std::string& input, int defaultWidth, bool defaultSigned)
{
    const std::string hint = "bits u8 0xA5 & 0x0F";
    if (input.size() > kMaxInputChars)
        return failWith<BitsEval>(fail("E_INPUT_TOO_LARGE", "input exceeds 16 KiB", hint));

    const std::vector<std::string> tokens = lexBits(input);
    if (tokens.empty())
        return failWith<BitsEval>(fail("E_EMPTY", "no expression", hint));

    BitsEval e;
    e.width = defaultWidth;
    e.signedType = defaultSigned;
    e.typeName = typeNameFor(defaultWidth, defaultSigned);
    std::size_t at = 0;
    if (isBitsType(tokens[0], e.width, e.signedType)) {
        e.typeName = typeNameFor(e.width, e.signedType);
        ++at;
    }
    if (at >= tokens.size())
        return failWith<BitsEval>(fail("E_EMPTY", "no expression after the type", hint));

    const std::uint64_t m = maskFor(e.width);
    const std::vector<std::string> rest(tokens.begin() + static_cast<std::ptrdiff_t>(at), tokens.end());
    const std::string& first = rest[0];
    const auto finish = [&](std::uint64_t result, std::uint64_t lhs, std::uint64_t rhs,
                            bool hasRhs, const std::string& op, const std::string& expression,
                            const std::string& note) -> Outcome<BitsEval> {
        e.result = result & m;
        e.lhs = lhs & m;
        e.rhs = rhs & m;
        e.hasRhs = hasRhs;
        e.op = op;
        e.expr = expression;
        const Repr r = represent(e.result, e.width, e.signedType);
        std::ostringstream d;
        d << e.typeName << "  " << expression << "\n"
          << "HEX " << r.hex << "   BIN " << r.bin << "   OCT " << r.oct << "\n"
          << "DEC " << r.dec << "   unsigned " << r.unsignedDec << "   signed " << r.signedDec << "\n"
          << "grouped " << binGrouped(e.result, e.width) << "\n" << note << "\n"
          << bitGrid(e.width, e.lhs, e.rhs, op, e.result, hasRhs);
        e.detail = d.str();
        std::ostringstream p;
        p << e.typeName << "  " << expression << "  =  " << r.hex << "\n"
          << bitGrid(e.width, e.lhs, e.rhs, op, e.result, hasRhs)
          << "HEX " << r.hex << "  BIN " << r.bin << "  OCT " << r.oct << "  DEC " << r.dec << "\n";
        e.preview = p.str();
        std::ostringstream c;
        c << "bytes hex";
        for (int i = 0; i < e.width / 8; ++i)
            c << ' ' << hexUpper((e.result >> (8 * i)) & 0xFF, 8).substr(2);
        e.chain = c.str();
        return Outcome<BitsEval>::good(e);
    };

    // Field operations: get / set / clear / toggle VALUE HIGH:LOW [VALUE]
    if (first == "get" || first == "set" || first == "clear" || first == "toggle") {
        const std::size_t expected = first == "set" ? 4u : 3u;
        if (rest.size() != expected)
            return failWith<BitsEval>(fail("E_SYNTAX", "wrong number of arguments for a field operation",
                                          first == "set" ? "bits u8 set 0xA5 7:4 0x3" : "bits u8 get 0xA5 7:4"));
        auto base = parseLiteral(rest[1], e.width, e.signedType);
        if (!base.success)
            return failWith<BitsEval>(base.failure);
        auto range = parseBitRange(rest[2], e.width);
        if (!range.success)
            return failWith<BitsEval>(range.failure);
        const int hi = range.value.first;
        const int lo = range.value.second;
        const int bitsCount = hi - lo + 1;
        const std::uint64_t fieldMask = maskFor(bitsCount) << lo;
        std::uint64_t result = base.value;
        std::string note = "field [" + std::to_string(hi) + ":" + std::to_string(lo) + "]  mask "
                           + hexUpper(fieldMask, e.width);
        if (first == "get") {
            result = (base.value & fieldMask) >> lo;
        } else if (first == "clear") {
            result = base.value & ~fieldMask;
        } else if (first == "toggle") {
            result = base.value ^ fieldMask;
        } else {
            auto v = parseLiteral(rest[3], bitsCount, false);
            if (!v.success)
                return failWith<BitsEval>(v.failure);
            result = (base.value & ~fieldMask) | ((v.value << lo) & fieldMask);
            note += "  write " + hexUpper(v.value, bitsCount);
        }
        const std::string expression = first + " " + hexUpper(base.value, e.width) + " [" + std::to_string(hi)
                                       + ":" + std::to_string(lo) + "]";
        return finish(result, base.value, fieldMask, true, first, expression, note);
    }

    // Unary complement.
    if (first == "~") {
        if (rest.size() != 2)
            return failWith<BitsEval>(fail("E_SYNTAX", "~ takes exactly one operand", "bits u8 ~0xA5"));
        auto a = parseLiteral(rest[1], e.width, e.signedType);
        if (!a.success)
            return failWith<BitsEval>(a.failure);
        return finish(~a.value & m, a.value, a.value, false, "~",
                      "~" + hexUpper(a.value, e.width), "complement is masked to the selected width");
    }

    // A single value: just show every representation.
    if (rest.size() == 1) {
        auto a = parseLiteral(rest[0], e.width, e.signedType);
        if (!a.success)
            return failWith<BitsEval>(a.failure);
        return finish(a.value, a.value, a.value, false, "value", hexUpper(a.value, e.width),
                      "no operation; the value is shown in four bases");
    }

    if (rest.size() != 3)
        return failWith<BitsEval>(fail("E_SYNTAX", "expected VALUE OP VALUE", hint));
    const std::string op = lower(rest[1]);
    static const std::set<std::string> binaryOps = {"&", "|", "^", "<<", ">>", "rol", "ror"};
    if (!binaryOps.count(op))
        return failWith<BitsEval>(fail("E_UNKNOWN_OP", "unsupported operator", "bits u8 0xA5 & 0x0F"));

    auto a = parseLiteral(rest[0], e.width, e.signedType);
    if (!a.success)
        return failWith<BitsEval>(a.failure);

    const bool isShift = op == "<<" || op == ">>" || op == "rol" || op == "ror";
    auto b = isShift ? parseLiteral(rest[2], 64, false)
                     : parseLiteral(rest[2], e.width, e.signedType);
    if (!b.success)
        return failWith<BitsEval>(b.failure);

    std::uint64_t result = 0;
    std::string note;
    if (op == "&")
        result = a.value & b.value;
    else if (op == "|")
        result = a.value | b.value;
    else if (op == "^")
        result = a.value ^ b.value;
    else if (op == "<<") {
        if (b.value >= static_cast<std::uint64_t>(e.width))
            return failWith<BitsEval>(fail("E_SHIFT", "shift count must be smaller than the width", hint));
        result = a.value << b.value;
        note = "logical left shift; bits shifted out are discarded";
    } else if (op == ">>") {
        if (b.value >= static_cast<std::uint64_t>(e.width))
            return failWith<BitsEval>(fail("E_SHIFT", "shift count must be smaller than the width", hint));
        if (e.signedType && (a.value & (std::uint64_t(1) << (e.width - 1)))) {
            const std::uint64_t full = asSigned(a.value, e.width) >= 0
                                           ? a.value
                                           : (a.value | ~m);
            std::uint64_t shifted = full >> b.value;
            if (b.value > 0)
                shifted |= ~std::uint64_t(0) << (64 - b.value);
            result = shifted;
            note = "arithmetic right shift (sign bit is replicated)";
        } else {
            result = a.value >> b.value;
            note = "logical right shift";
        }
    } else if (op == "rol") {
        const unsigned n = static_cast<unsigned>(b.value % static_cast<std::uint64_t>(e.width));
        result = n == 0 ? a.value : ((a.value << n) | (a.value >> (e.width - n)));
        note = "rotate left by " + std::to_string(n) + " (count mod width, no bits lost)";
    } else {
        const unsigned n = static_cast<unsigned>(b.value % static_cast<std::uint64_t>(e.width));
        result = n == 0 ? a.value : ((a.value >> n) | (a.value << (e.width - n)));
        note = "rotate right by " + std::to_string(n) + " (count mod width, no bits lost)";
    }
    // Bitwise operands read better in the same base; shift and rotate counts
    // are plain decimal amounts.
    const std::string rhsText = isShift ? std::to_string(b.value) : hexUpper(b.value, e.width);
    const std::string expression = hexUpper(a.value, e.width) + " " + op + " " + rhsText;
    return finish(result, a.value, b.value, true, op, expression, note);
}

// ---- bytes: byte streams, word decoding, hexdump ------------------------------

struct TypeSpec {
    std::string name;
    int size = 1;
    bool isFloat = false;
    bool signedType = false;
    bool big = false;
};

inline bool resolveType(const std::string& token, const std::string& defaultEndian, TypeSpec& out)
{
    const std::string t = lower(token);
    const bool defaultBig = defaultEndian == "be";
    std::string base = t;
    out.big = defaultBig;
    if (t.size() > 2 && (t.compare(t.size() - 2, 2, "le") == 0 || t.compare(t.size() - 2, 2, "be") == 0)) {
        out.big = t.compare(t.size() - 2, 2, "be") == 0;
        base = t.substr(0, t.size() - 2);
    }
    out.signedType = !base.empty() && base[0] == 'i';
    out.isFloat = !base.empty() && base[0] == 'f';
    int width = 0;
    if (base == "u8" || base == "i8")
        width = 8;
    else if (base == "u16" || base == "i16" || base == "f32")
        width = base == "f32" ? 32 : 16;
    else if (base == "u32" || base == "i32" || base == "f64")
        width = base == "f64" ? 64 : 32;
    else if (base == "u64" || base == "i64")
        width = 64;
    else if (base == "f16")
        return false; // half precision is deliberately not supported
    else
        return false;
    out.size = width / 8;
    if (out.size == 1)
        out.big = false;
    out.name = base + (out.size == 1 ? std::string() : std::string(out.big ? "be" : "le"));
    return true;
}

struct ByteWord {
    std::size_t offset = 0;
    std::string typeName;
    std::string primary;
    std::string detail;
    std::string bitsHex;
    std::string chain;
    int width = 8;
    bool isFloat = false;
};

struct BytesEval {
    std::string format;
    std::string typeName;
    std::string normalized;
    std::string dump;
    std::string preview;
    std::string detail;
    std::string chain;
    std::vector<std::uint8_t> data;
    std::vector<ByteWord> words;
    std::size_t trailing = 0;
};

inline std::vector<std::string> lexByteTokens(const std::string& s)
{
    std::string spaced;
    for (char c : s) {
        if (c == '_' || c == ',' || c == ':' || c == ';' || c == '-' || std::isspace(static_cast<unsigned char>(c)))
            spaced += ' ';
        else
            spaced += c;
    }
    return splitWhitespace(spaced);
}

inline std::string stripRadixPrefix(const std::string& token)
{
    if (token.size() >= 2 && token[0] == '0') {
        const char p = static_cast<char>(std::tolower(static_cast<unsigned char>(token[1])));
        if (p == 'x' || p == 'b' || p == 'o')
            return token.substr(2);
    }
    return token;
}

inline std::string hexDump(const std::vector<std::uint8_t>& data, std::size_t maxLines)
{
    std::ostringstream out;
    const std::size_t lines = (data.size() + 15) / 16;
    const std::size_t shown = std::min(lines, maxLines);
    for (std::size_t line = 0; line < shown; ++line) {
        const std::size_t base = line * 16;
        char offset[16];
        std::snprintf(offset, sizeof offset, "%04X", static_cast<unsigned>(base));
        out << offset << "  ";
        for (std::size_t i = 0; i < 16; ++i) {
            if (base + i < data.size()) {
                char cell[8];
                std::snprintf(cell, sizeof cell, "%02X ", data[base + i]);
                out << cell;
            } else {
                out << "   ";
            }
            if (i == 7)
                out << ' ';
        }
        out << " |";
        for (std::size_t i = 0; i < 16 && base + i < data.size(); ++i) {
            const std::uint8_t b = data[base + i];
            out << (b >= 0x20 && b <= 0x7E ? static_cast<char>(b) : '.');
        }
        out << "|\n";
    }
    if (lines > shown)
        out << "... " << (lines - shown) << " more lines (" << data.size() << " bytes total)\n";
    return out.str();
}

inline std::string formatFloat(std::uint64_t raw, bool isDouble)
{
    std::ostringstream out;
    if (isDouble) {
        double v = 0;
        std::memcpy(&v, &raw, sizeof v);
        if (std::isnan(v))
            return "NaN";
        if (std::isinf(v))
            return v > 0 ? "+Inf" : "-Inf";
        out << std::setprecision(17) << v;
    } else {
        float v = 0;
        const std::uint32_t low = static_cast<std::uint32_t>(raw & 0xFFFFFFFFu);
        std::memcpy(&v, &low, sizeof v);
        if (std::isnan(v))
            return "NaN";
        if (std::isinf(v))
            return v > 0 ? "+Inf" : "-Inf";
        out << std::setprecision(9) << static_cast<double>(v);
    }
    return out.str();
}

inline std::string floatBreakdown(std::uint64_t raw, bool isDouble)
{
    std::ostringstream out;
    if (isDouble) {
        const std::uint64_t sign = raw >> 63;
        const std::uint64_t exponent = (raw >> 52) & 0x7FF;
        const std::uint64_t mantissa = raw & 0xFFFFFFFFFFFFFull;
        out << "IEEE-754 binary64  S=" << sign << "  E=0x" << std::uppercase << std::hex << exponent
            << "  M=0x" << mantissa << std::dec << "\n";
    } else {
        const std::uint32_t low = static_cast<std::uint32_t>(raw & 0xFFFFFFFFu);
        const std::uint32_t sign = low >> 31;
        const std::uint32_t exponent = (low >> 23) & 0xFF;
        const std::uint32_t mantissa = low & 0x7FFFFF;
        out << "IEEE-754 binary32  S=" << sign << "  E=0x" << std::uppercase << std::hex << exponent
            << "  M=0x" << mantissa << std::dec << "\n";
    }
    return out.str();
}

inline Outcome<BytesEval> evalBytes(const std::string& input, const std::string& defaultEndian)
{
    const std::string hint = "bytes hex u16le 34 12 78 56";
    if (input.size() > kMaxInputChars)
        return failWith<BytesEval>(fail("E_INPUT_TOO_LARGE", "input exceeds 16 KiB", hint));
    const std::vector<std::string> tokens = splitWhitespace(input);
    if (tokens.empty())
        return failWith<BytesEval>(fail("E_EMPTY", "no byte stream", hint));

    BytesEval e;
    e.format = lower(tokens[0]);
    if (e.format != "hex" && e.format != "bin" && e.format != "oct")
        return failWith<BytesEval>(fail("E_UNKNOWN_FORMAT", "format must be hex, bin or oct", hint));

    std::size_t at = 1;
    TypeSpec spec;
    if (at < tokens.size() && resolveType(tokens[at], defaultEndian, spec)) {
        ++at;
    } else {
        if (!resolveType("u8", defaultEndian, spec))
            return failWith<BytesEval>(fail("E_INTERNAL", "default type failed to resolve", hint));
    }
    e.typeName = spec.name;

    std::string dataText;
    for (std::size_t i = at; i < tokens.size(); ++i) {
        if (!dataText.empty())
            dataText += ' ';
        dataText += tokens[i];
    }
    const std::vector<std::string> byteTokens = lexByteTokens(dataText);
    if (byteTokens.empty())
        return failWith<BytesEval>(fail("E_EMPTY", "no bytes after the format and type", hint));

    for (const std::string& raw : byteTokens) {
        const std::string token = stripRadixPrefix(raw);
        if (token.empty())
            return failWith<BytesEval>(fail("E_BAD_TOKEN", "empty byte token", hint));
        if (e.format == "hex") {
            if (token.size() % 2 != 0)
                return failWith<BytesEval>(fail("E_HEX_ODD", "a compact hex token needs an even number of digits", hint));
            for (std::size_t i = 0; i < token.size(); i += 2) {
                const int hi = digitValue(token[i]);
                const int lo = digitValue(token[i + 1]);
                if (hi < 0 || lo < 0 || hi > 15 || lo > 15)
                    return failWith<BytesEval>(fail("E_BAD_TOKEN", "not a hex digit", hint));
                e.data.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
            }
        } else if (e.format == "bin") {
            if (token.size() != 8)
                return failWith<BytesEval>(fail("E_BIN_WIDTH", "a bin token must be exactly 8 bits", "bytes bin 10101010 00001111"));
            std::uint8_t value = 0;
            for (char c : token) {
                if (c != '0' && c != '1')
                    return failWith<BytesEval>(fail("E_BAD_TOKEN", "not a binary digit", "bytes bin 10101010 00001111"));
                value = static_cast<std::uint8_t>((value << 1) | (c == '1' ? 1 : 0));
            }
            e.data.push_back(value);
        } else {
            if (token.size() > 3)
                return failWith<BytesEval>(fail("E_OCT_RANGE", "an oct token must be 000..377", "bytes oct 377 040 000"));
            std::uint64_t value = 0;
            for (char c : token) {
                if (c < '0' || c > '7')
                    return failWith<BytesEval>(fail("E_BAD_TOKEN", "not an octal digit", "bytes oct 377 040 000"));
                value = value * 8 + static_cast<std::uint64_t>(c - '0');
            }
            if (value > 0xFF)
                return failWith<BytesEval>(fail("E_OCT_RANGE", "an oct token must be 000..377", "bytes oct 377 040 000"));
            e.data.push_back(static_cast<std::uint8_t>(value));
        }
        if (e.data.size() > kMaxBytes)
            return failWith<BytesEval>(fail("E_BYTES_TOO_LARGE", "more than 4096 decoded bytes", hint));
    }

    // Normalized text and hexdump.
    std::ostringstream normalized;
    for (std::size_t i = 0; i < e.data.size(); ++i) {
        if (i)
            normalized << ' ';
        char cell[8];
        std::snprintf(cell, sizeof cell, "%02X", e.data[i]);
        normalized << cell;
    }
    e.normalized = normalized.str();
    e.dump = hexDump(e.data, 256);
    const std::string shortDump = hexDump(e.data, 32);

    // Decode complete words; never invent a value for a partial trailing group.
    const std::size_t size = static_cast<std::size_t>(spec.size);
    for (std::size_t offset = 0; offset + size <= e.data.size(); offset += size) {
        std::uint64_t raw = 0;
        for (std::size_t i = 0; i < size; ++i) {
            const std::size_t index = spec.big ? (size - 1 - i) : i;
            raw |= static_cast<std::uint64_t>(e.data[offset + index]) << (8 * i);
        }
        ByteWord w;
        w.offset = offset;
        w.typeName = spec.name;
        w.width = spec.size * 8;
        w.isFloat = spec.isFloat;
        w.bitsHex = hexUpper(raw, w.width);
        w.primary = spec.isFloat ? formatFloat(raw, spec.size == 8) : w.bitsHex;

        std::ostringstream bytes;
        for (std::size_t i = 0; i < size; ++i) {
            if (i)
                bytes << ' ';
            char cell[8];
            std::snprintf(cell, sizeof cell, "%02X", e.data[offset + i]);
            bytes << cell;
        }
        std::ostringstream d;
        d << spec.name << " @" << offset << "  bytes " << bytes.str() << "\n";
        if (spec.isFloat) {
            d << "value " << w.primary << "  bits " << w.bitsHex << "\n" << floatBreakdown(raw, spec.size == 8);
        } else {
            const Repr r = represent(raw, w.width, spec.signedType);
            d << "HEX " << r.hex << "  BIN " << r.bin << "  OCT " << r.oct << "\n"
              << "DEC " << r.dec << "  unsigned " << r.unsignedDec << "  signed " << r.signedDec << "\n"
              << "bits " << binGrouped(raw, w.width) << "\n";
            if (spec.signedType)
                w.primary = r.dec;
        }
        w.detail = d.str();
        w.chain = "bits " + typeNameFor(w.width, false) + " " + w.bitsHex;
        e.words.push_back(std::move(w));
    }
    e.trailing = e.data.size() % size;

    std::ostringstream p;
    p << e.format << " -> " << e.typeName << "  " << e.data.size() << " bytes  " << e.words.size()
      << " words  trailing " << e.trailing << "\n" << shortDump;
    for (std::size_t i = 0; i < e.words.size() && i < kMaxWordRows; ++i)
        p << "@" << e.words[i].offset << "  " << e.words[i].primary << "\n";
    if (e.words.size() > kMaxWordRows)
        p << "... " << (e.words.size() - kMaxWordRows) << " more words (see full details)\n";
    e.preview = p.str();

    // Copyable detail text is bounded. All bytes are still present in
    // `normalized` and the hexdump; per-word breakdowns are capped so a
    // 4096-byte input does not inflate every result row with megabytes.
    constexpr std::size_t kMaxDetailWords = 64;
    std::ostringstream d;
    d << e.format << " -> " << e.typeName << "  " << e.data.size() << " bytes\n"
      << "normalized " << e.normalized << "\n" << e.dump;
    const std::size_t detailWords = std::min(e.words.size(), kMaxDetailWords);
    for (std::size_t i = 0; i < detailWords; ++i) {
        const ByteWord& w = e.words[i];
        d << "@" << w.offset << " " << w.primary << "\n" << w.detail;
    }
    if (e.words.size() > detailWords)
        d << "detail text truncated: " << (e.words.size() - detailWords)
          << " more words decoded; the complete byte stream is in 'normalized' and the hexdump above\n";
    if (e.trailing)
        d << "trailing " << e.trailing << " byte(s) left undecoded\n";
    e.detail = d.str();

    if (!e.words.empty())
        e.chain = e.words.front().chain;
    else
        e.chain = "bytes " + e.format + " " + e.normalized;
    return Outcome<BytesEval>::good(e);
}

// ---- struct: C-like layout calculation ----------------------------------------

struct MemberRow {
    std::string name;
    std::string typeName;
    std::string kind; // member | padding | tail
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t align = 1;
};

struct TypeInfo {
    std::string name;
    std::uint64_t size = 0;
    std::uint64_t align = 1;
    bool aggregate = false;
    bool isUnion = false;
    std::vector<MemberRow> layout;
};

struct RawMember {
    std::string name;
    std::string typeName;
    TypeInfo type;
};

struct Layout {
    std::string name;
    std::string definition;
    std::string text;
    std::string memberTable;
    std::string assertions;
    std::string byteMap;
    std::string chain;
    bool isUnion = false;
    int pack = 0;
    std::uint64_t size = 0;
    std::uint64_t align = 1;
    std::vector<MemberRow> members;
};

inline TypeInfo scalarType(const std::string& name, int pack)
{
    TypeInfo t;
    t.name = name;
    const std::string s = lower(name);
    const int width = s.substr(1) == "8" ? 8
                      : s.substr(1) == "16" ? 16
                      : s.substr(1) == "32" ? 32
                                           : 64;
    t.size = static_cast<std::uint64_t>(width / 8);
    const std::uint64_t natural = t.size;
    t.align = pack > 0 ? std::min(natural, static_cast<std::uint64_t>(pack)) : natural;
    return t;
}

inline bool isScalarName(const std::string& s)
{
    static const std::set<std::string> names = {"u8", "i8", "u16", "i16", "u32", "i32",
                                               "u64", "i64", "f32", "f64"};
    return names.count(lower(s)) > 0;
}

inline bool isUnsupportedCName(const std::string& s)
{
    static const std::set<std::string> names = {"int", "long", "short", "char", "bool", "void", "float",
                                               "double", "size_t", "uint8_t", "uint16_t", "uint32_t",
                                               "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t"};
    return names.count(lower(s)) > 0;
}

// `pack` is already folded into every TypeInfo.align by scalarType(); it is
// kept in the signature so call sites state the active packing explicitly.
inline TypeInfo computeAggregate(bool isUnion, const std::vector<RawMember>& raw, int pack,
                                const std::string& name, Failure& error)
{
    (void)pack;
    TypeInfo t;
    t.name = name;
    t.aggregate = true;
    t.isUnion = isUnion;
    const std::string hint = "struct pack=4 struct Frame { u8 sof; u32 seq; u16 len; u8 data[3]; };";
    if (raw.empty()) {
        error = fail("E_EMPTY_STRUCT", "empty struct or union", hint);
        return t;
    }
    if (raw.size() > kMaxMembers) {
        error = fail("E_TOO_MANY_MEMBERS", "more than 256 members", hint);
        return t;
    }
    std::set<std::string> seen;
    for (const RawMember& m : raw) {
        if (!seen.insert(m.name).second) {
            error = fail("E_DUPLICATE_MEMBER", "duplicate member name: " + m.name, hint);
            return t;
        }
        if (m.type.size == 0 || m.type.align == 0) {
            error = fail("E_INTERNAL", "zero-sized type", hint);
            return t;
        }
    }

    std::uint64_t offset = 0;
    std::uint64_t align = 1;
    std::uint64_t unionContent = 0;
    std::vector<MemberRow> rows;
    for (const RawMember& m : raw) {
        const std::uint64_t memberAlign = std::max<std::uint64_t>(1, m.type.align);
        align = std::max(align, memberAlign);
        if (isUnion) {
            rows.push_back({m.name, m.typeName, "member", 0, m.type.size, memberAlign});
            unionContent = std::max(unionContent, m.type.size);
        } else {
            bool ok = true;
            const std::uint64_t aligned = alignUp(offset, memberAlign, ok);
            if (!ok) {
                error = fail("E_STRUCT_TOO_LARGE", "offset overflow", hint);
                return t;
            }
            const std::uint64_t padding = aligned - offset;
            if (padding)
                rows.push_back({"(padding)", "-", "padding", offset, padding, 1});
            offset = aligned;
            if (offset > kMaxStructBytes - m.type.size) {
                error = fail("E_STRUCT_TOO_LARGE", "struct exceeds 4096 bytes", hint);
                return t;
            }
            rows.push_back({m.name, m.typeName, "member", offset, m.type.size, memberAlign});
            offset += m.type.size;
        }
    }
    const std::uint64_t content = isUnion ? unionContent : offset;
    bool ok = true;
    const std::uint64_t size = alignUp(content, align, ok);
    if (!ok || size > kMaxStructBytes) {
        error = fail("E_STRUCT_TOO_LARGE", "struct exceeds 4096 bytes", hint);
        return t;
    }
    if (size > content)
        rows.push_back({"(tail padding)", "-", "tail", content, size - content, 1});
    t.size = size;
    t.align = align;
    t.layout = std::move(rows);
    return t;
}

inline std::string rangeText(std::uint64_t offset, std::uint64_t size)
{
    if (size <= 1)
        return std::to_string(offset);
    return std::to_string(offset) + ".." + std::to_string(offset + size - 1);
}

inline void renderLayout(Layout& l)
{
    std::ostringstream table;
    table << "  #  range      name              type              size\n";
    std::size_t index = 0;
    for (const MemberRow& m : l.members) {
        table << std::right << std::setw(3) << index++ << "  " << std::left << std::setw(11) << rangeText(m.offset, m.size)
              << std::setw(18) << (m.name.empty() ? "-" : m.name)
              << std::setw(18) << (m.typeName.empty() ? "-" : m.typeName)
              << m.size << " B\n";
    }
    l.memberTable = table.str();

    const std::string name = l.name.empty() ? "Anonymous" : l.name;
    std::ostringstream assertions;
    assertions << "static_assert(sizeof(" << name << ") == " << l.size << ", \"sizeof(" << name << ")\");\n"
               << "static_assert(alignof(" << name << ") == " << l.align << ", \"alignof(" << name << ")\");\n";
    for (const MemberRow& m : l.members)
        if (m.kind == "member")
            assertions << "static_assert(offsetof(" << name << ", " << m.name << ") == " << m.offset
                       << ", \"offsetof(" << name << ", " << m.name << ")\");\n";
    l.assertions = assertions.str();

    std::ostringstream map;
    const std::size_t shown = std::min<std::uint64_t>(l.size, 256u);
    std::vector<std::string> letters;
    std::vector<char> memberLetters(l.members.size(), '.');
    std::size_t ordinal = 0;
    for (std::size_t i = 0; i < l.members.size(); ++i) {
        if (l.members[i].kind != "member")
            continue;
        const char c = ordinal < 26 ? static_cast<char>('A' + ordinal)
                                    : (ordinal < 52 ? static_cast<char>('a' + (ordinal - 26)) : '#');
        memberLetters[i] = c;
        letters.push_back(std::string(1, c) + "=" + l.members[i].name);
        ++ordinal;
    }
    map << "byte map (";
    for (std::size_t i = 0; i < letters.size(); ++i)
        map << (i ? " " : "") << letters[i];
    map << "  .=padding)\n";
    for (std::size_t base = 0; base < shown; base += 16) {
        char offset[16];
        std::snprintf(offset, sizeof offset, "%04X", static_cast<unsigned>(base));
        map << offset << "  ";
        for (std::size_t i = 0; i < 16 && base + i < shown; ++i) {
            const std::uint64_t byte = base + i;
            char cell = '.';
            for (std::size_t k = 0; k < l.members.size(); ++k) {
                const MemberRow& m = l.members[k];
                if (m.kind == "member" && byte >= m.offset && byte < m.offset + m.size) {
                    cell = memberLetters[k];
                    break;
                }
            }
            map << cell;
        }
        map << "\n";
    }
    if (l.size > shown)
        map << "... " << (l.size - shown) << " more bytes (see the member table)\n";
    l.byteMap = map.str();

    std::ostringstream text;
    text << name << "  sizeof=" << l.size << " B  align=" << l.align << " B  pack="
         << (l.pack == 0 ? "natural" : std::to_string(l.pack)) << "\n"
         << l.memberTable << l.byteMap << l.assertions;
    l.text = text.str();
}

inline std::vector<std::string> lexC(const std::string& s)
{
    std::string spaced;
    for (char c : s) {
        if (std::strchr("{}[];:*=,()", c) != nullptr) {
            spaced += ' ';
            spaced += c;
            spaced += ' ';
        } else {
            spaced += c;
        }
    }
    return splitWhitespace(spaced);
}

class StructParser {
public:
    StructParser(std::vector<std::string> tokens, int pack)
        : m_tokens(std::move(tokens)), m_pack(pack)
    {
    }

    Outcome<Layout> run()
    {
        const std::string hint = "struct pack=4 struct Frame { u8 sof; u32 seq; u16 len; u8 data[3]; };";
        Layout last;
        int definitions = 0;
        while (m_at < m_tokens.size()) {
            const std::string head = m_tokens[m_at++];
            if (head != "struct" && head != "union")
                return failWith<Layout>(fail("E_SYNTAX", "expected struct or union", hint));
            const bool isUnion = head == "union";
            std::string name;
            if (m_at < m_tokens.size() && isIdentifier(m_tokens[m_at]))
                name = m_tokens[m_at++];
            if (!expect("{"))
                return failWith<Layout>(fail("E_SYNTAX", "expected {", hint));
            ++m_at;
            std::vector<RawMember> raw;
            while (m_at < m_tokens.size() && m_tokens[m_at] != "}") {
                Outcome<RawMember> m = parseMember(0);
                if (!m.success)
                    return failWith<Layout>(m.failure);
                raw.push_back(std::move(m.value));
            }
            if (!expect("}"))
                return failWith<Layout>(fail("E_SYNTAX", "missing }", hint));
            ++m_at;
            if (!expect(";"))
                return failWith<Layout>(fail("E_SYNTAX", "expected ; after }", hint));
            ++m_at;
            if (definitions + 1 > static_cast<int>(kMaxDefinitions))
                return failWith<Layout>(fail("E_TOO_MANY_DEFINITIONS", "more than 16 definitions", hint));
            if (!name.empty() && m_types.count(name))
                return failWith<Layout>(fail("E_DUPLICATE_TYPE", "duplicate type name: " + name, hint));
            Failure error;
            TypeInfo t = computeAggregate(isUnion, raw, m_pack, name.empty() ? "Anonymous" : name, error);
            if (!error.code.empty())
                return failWith<Layout>(error);
            if (!name.empty())
                m_types[name] = t;
            last = makeLayout(t, name, isUnion);
            ++definitions;
        }
        if (definitions == 0)
            return failWith<Layout>(fail("E_EMPTY", "no struct definition", hint));
        return Outcome<Layout>::good(last);
    }

private:
    bool expect(const std::string& token) const
    {
        return m_at < m_tokens.size() && m_tokens[m_at] == token;
    }

    static Layout makeLayout(const TypeInfo& t, const std::string& name, bool isUnion)
    {
        Layout l;
        l.name = name;
        l.isUnion = isUnion;
        l.size = t.size;
        l.align = t.align;
        l.members = t.layout;
        return l;
    }

    Outcome<RawMember> parseMember(int depth)
    {
        const std::string hint = "struct pack=4 struct Frame { u8 sof; u32 seq; u16 len; u8 data[3]; };";
        if (depth > kMaxNesting)
            return failWith<RawMember>(fail("E_RECURSION", "nesting is too deep", hint));
        if (m_at >= m_tokens.size())
            return failWith<RawMember>(fail("E_SYNTAX", "unexpected end of definition", hint));
        const std::string head = m_tokens[m_at];

        // struct Name member;  (reference to a type defined earlier)
        if ((head == "struct" || head == "union") && m_at + 2 < m_tokens.size()
            && isIdentifier(m_tokens[m_at + 1]) && isIdentifier(m_tokens[m_at + 2])) {
            const std::string typeName = m_tokens[m_at + 1];
            const std::string memberName = m_tokens[m_at + 2];
            if (!m_types.count(typeName))
                return failWith<RawMember>(fail("E_UNKNOWN_TYPE", "unknown or forward-referenced type: " + typeName, hint));
            m_at += 3;
            if (!expect(";"))
                return failWith<RawMember>(fail("E_SYNTAX", "expected ;", hint));
            ++m_at;
            RawMember r;
            r.name = memberName;
            r.typeName = typeName;
            r.type = m_types[typeName];
            return Outcome<RawMember>::good(r);
        }

        // Inline nested definition: struct [Name] { ... } member;
        if (head == "struct" || head == "union") {
            ++m_at;
            const bool isUnion = head == "union";
            std::string typeName;
            if (m_at < m_tokens.size() && isIdentifier(m_tokens[m_at]))
                typeName = m_tokens[m_at++];
            if (!expect("{"))
                return failWith<RawMember>(fail("E_SYNTAX", "expected { for a nested type", hint));
            ++m_at;
            std::vector<RawMember> nested;
            while (m_at < m_tokens.size() && m_tokens[m_at] != "}") {
                Outcome<RawMember> n = parseMember(depth + 1);
                if (!n.success)
                    return failWith<RawMember>(n.failure);
                nested.push_back(std::move(n.value));
            }
            if (!expect("}"))
                return failWith<RawMember>(fail("E_SYNTAX", "missing } for a nested type", hint));
            ++m_at;
            Failure error;
            TypeInfo t = computeAggregate(isUnion, nested, m_pack,
                                         typeName.empty() ? head : typeName, error);
            if (!error.code.empty())
                return failWith<RawMember>(error);
            if (!typeName.empty()) {
                if (m_types.count(typeName))
                    return failWith<RawMember>(fail("E_DUPLICATE_TYPE", "duplicate type name: " + typeName, hint));
                m_types[typeName] = t;
            }
            if (m_at >= m_tokens.size() || !isIdentifier(m_tokens[m_at]))
                return failWith<RawMember>(fail("E_SYNTAX", "a nested type needs a member name", hint));
            const std::string memberName = m_tokens[m_at++];
            if (!expect(";"))
                return failWith<RawMember>(fail("E_SYNTAX", "expected ;", hint));
            ++m_at;
            RawMember r;
            r.name = memberName;
            r.typeName = typeName.empty() ? head : typeName;
            r.type = t;
            return Outcome<RawMember>::good(r);
        }

        const std::string typeToken = head;
        ++m_at;
        if (expect("*"))
            return failWith<RawMember>(fail("E_POINTER", "pointers are not supported (ABI dependent)", hint));
        if (expect(":"))
            return failWith<RawMember>(fail("E_BITFIELD", "bitfields are not supported (ABI dependent)", hint));
        if (isUnsupportedCName(typeToken))
            return failWith<RawMember>(fail("E_UNSUPPORTED_TYPE",
                                           "use fixed-width types (u8..u64, i8..i64, f32, f64), not " + typeToken,
                                           hint));
        TypeInfo t;
        if (isScalarName(typeToken)) {
            t = scalarType(typeToken, m_pack);
        } else if (m_types.count(typeToken)) {
            t = m_types[typeToken];
        } else {
            return failWith<RawMember>(fail("E_UNKNOWN_TYPE", "unknown type: " + typeToken, hint));
        }
        if (m_at >= m_tokens.size() || !isIdentifier(m_tokens[m_at]))
            return failWith<RawMember>(fail("E_SYNTAX", "expected a member name after " + typeToken, hint));
        const std::string memberName = m_tokens[m_at++];
        if (expect(":"))
            return failWith<RawMember>(fail("E_BITFIELD", "bitfields are not supported (ABI dependent)", hint));
        if (expect("*"))
            return failWith<RawMember>(fail("E_POINTER", "pointers are not supported (ABI dependent)", hint));
        if (expect(","))
            return failWith<RawMember>(fail("E_SYNTAX", "declare one member per statement", hint));

        std::string displayType = typeToken;
        if (expect("[")) {
            ++m_at;
            if (m_at >= m_tokens.size())
                return failWith<RawMember>(fail("E_ARRAY_SIZE", "missing array length", hint));
            std::uint64_t count = 0;
            if (!parseDecimal(m_tokens[m_at], kMaxArrayElements, count) || count == 0)
                return failWith<RawMember>(fail("E_ARRAY_SIZE", "array length must be 1..4096", hint));
            ++m_at;
            if (!expect("]"))
                return failWith<RawMember>(fail("E_SYNTAX", "expected ]", hint));
            ++m_at;
            if (t.size > kMaxStructBytes / count)
                return failWith<RawMember>(fail("E_STRUCT_TOO_LARGE", "array exceeds 4096 bytes", hint));
            t.size *= count;
            t.aggregate = false;
            displayType += "[" + std::to_string(count) + "]";
        }
        if (!expect(";"))
            return failWith<RawMember>(fail("E_SYNTAX", "expected ; after a member", hint));
        ++m_at;
        RawMember r;
        r.name = memberName;
        r.typeName = displayType;
        r.type = t;
        return Outcome<RawMember>::good(r);
    }

    std::vector<std::string> m_tokens;
    std::map<std::string, TypeInfo> m_types;
    std::size_t m_at = 0;
    int m_pack = 0;
};

inline bool validPack(int pack)
{
    return pack == 0 || pack == 1 || pack == 2 || pack == 4 || pack == 8 || pack == 16;
}

inline Outcome<Layout> evalStruct(const std::string& input, int defaultPack)
{
    const std::string hint = "struct pack=4 struct Frame { u8 sof; u32 seq; u16 len; u8 data[3]; };";
    if (input.size() > kMaxInputChars)
        return failWith<Layout>(fail("E_INPUT_TOO_LARGE", "input exceeds 16 KiB", hint));
    const std::vector<std::string> words = splitWhitespace(input);
    std::size_t at = 0;
    if (at < words.size() && lower(words[at]) == "layout")
        ++at;
    int pack = defaultPack;
    if (at < words.size() && startsWith(lower(words[at]), "pack=")) {
        std::uint64_t value = 0;
        if (!parseDecimal(words[at].substr(5), 16, value) || !validPack(static_cast<int>(value)))
            return failWith<Layout>(fail("E_BAD_PACK", "pack must be 0, 1, 2, 4, 8 or 16", hint));
        pack = static_cast<int>(value);
        ++at;
    } else if (at + 1 < words.size() && lower(words[at]) == "pack" && isDecimal(words[at + 1])) {
        std::uint64_t value = 0;
        if (!parseDecimal(words[at + 1], 16, value) || !validPack(static_cast<int>(value)))
            return failWith<Layout>(fail("E_BAD_PACK", "pack must be 0, 1, 2, 4, 8 or 16", hint));
        pack = static_cast<int>(value);
        at += 2;
    }
    if (!validPack(pack))
        return failWith<Layout>(fail("E_BAD_PACK", "pack must be 0, 1, 2, 4, 8 or 16", hint));

    std::string body;
    for (std::size_t i = at; i < words.size(); ++i) {
        if (!body.empty())
            body += ' ';
        body += words[i];
    }
    if (body.empty())
        return failWith<Layout>(fail("E_EMPTY", "no struct definition", hint));
    StructParser parser(lexC(body), pack);
    Outcome<Layout> result = parser.run();
    if (!result.success)
        return result;
    Layout& l = result.value;
    l.pack = pack;
    l.definition = body;
    renderLayout(l);
    const int nextPack = pack == 1 ? 0 : 1;
    l.chain = "struct pack=" + std::to_string(nextPack) + " " + body;
    return result;
}

} // namespace binlab
