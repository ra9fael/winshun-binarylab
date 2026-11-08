// test_expr_spec.cpp - table-driven specification tests for the bits
// expression language (evalBits + lexBits + parseLiteral + BitsParser).
//
// Every row pins exact behaviour observed in the implementation and
// hand-verified once: the golden float bit patterns were probed, checked
// against IEEE-754 by hand and then frozen, so the table is the standard.
// Error rows assert the code and a stable substring of the message, not the
// full wording.
//
// A1..J4 row ids match the expression-language test spec (docs discussion,
// 2026-11-08); keep the ids stable when adding rows.
#include "logic.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

int g_total = 0;
int g_failed = 0;

#define CHECK(condition, label)                                                                    \
    do {                                                                                           \
        ++g_total;                                                                                 \
        if (!(condition)) {                                                                        \
            ++g_failed;                                                                            \
            std::cerr << "FAIL [" << (label) << "] at line " << __LINE__ << "\n";                  \
        }                                                                                          \
    } while (false)

// One spec row. Value rows: hex/type/float/note describe the expected BitsEval.
// Error rows: err plus errSub (message substring).
struct Spec {
    const char* id;
    const char* in;
    int width = 32;
    bool sg = false;
    binlab::EvalMode mode = binlab::EvalMode::Fixed;
    const char* err = nullptr;   // expected error code; nullptr = value row
    const char* errSub = nullptr;
    const char* hex = nullptr;   // expected hexUpper(result, width), no prefix
    const char* type = nullptr;  // expected typeName
    const char* floatT = nullptr; // expected floatText; nullptr = integer row
    const char* noteSub = nullptr; // substring of unitNote|floatNote|detail
};

constexpr binlab::EvalMode Fixed = binlab::EvalMode::Fixed;
constexpr binlab::EvalMode C = binlab::EvalMode::C;

const Spec kSpec[] = {
    // ---- A. lexer ----------------------------------------------------------
    {"A1", "1e-6", 32, false, Fixed, nullptr, nullptr, "0x3EB0C6F7A0B5ED8D", "f64", "1e-06",
     "IEEE-754 binary64 bit pattern"}, // the exponent sign stays with its number
    {"A2", "0x1E - 6", 32, false, Fixed, nullptr, nullptr, "0x00000018", "u32", nullptr, nullptr},
    {"A3", "1e+3", 32, false, Fixed, nullptr, nullptr, "0x408F400000000000", "f64", "1000", nullptr},
    {"A4", "1 < 2", 32, false, Fixed, "E_SYNTAX", "near '<'", nullptr, nullptr, nullptr, nullptr},
    {"A5", "1 > > 2", 32, false, Fixed, "E_SYNTAX", "near '>'", nullptr, nullptr, nullptr, nullptr},
    {"A6", "2**3**2", 32, false, Fixed, nullptr, nullptr, "0x00000200", "u32", nullptr,
     nullptr}, // ** is one token and right associative
    {"A7", "0b1e1", 32, false, Fixed, "E_BAD_LITERAL", "invalid digit for the base", nullptr,
     nullptr, nullptr, nullptr},

    // ---- B. integer literals ----------------------------------------------
    {"B1", "u8 0xA5", 32, false, Fixed, nullptr, nullptr, "0xA5", "u8", nullptr, nullptr},
    {"B2", "0b1_0000", 32, false, Fixed, nullptr, nullptr, "0x00000010", "u32", nullptr, nullptr},
    {"B3", "0o17", 32, false, Fixed, nullptr, nullptr, "0x0000000F", "u32", nullptr, nullptr},
    {"B4", "0xFF_FF", 32, false, Fixed, nullptr, nullptr, "0x0000FFFF", "u32", nullptr, nullptr},
    {"B5", "+5", 32, false, Fixed, nullptr, nullptr, "0x00000005", "u32", nullptr, nullptr},
    {"B6a", "-5", 32, false, Fixed, nullptr, nullptr, "0xFFFFFFFB", "u32", nullptr, "wrapped"},
    {"B6b", "--5", 32, false, Fixed, nullptr, nullptr, "0x00000005", "u32", nullptr,
     "1 of 2 steps exceeded the type and wrapped"},
    {"B7", "i8 -128", 32, false, Fixed, nullptr, nullptr, "0x80", "i8", nullptr, nullptr},
    {"B8", "i8 -129", 32, false, Fixed, nullptr, nullptr, "0x7F", "i8", nullptr, "wrapped"},
    {"B9", "i8 128", 32, false, Fixed, "E_RANGE", "positive literal is out of signed range",
     nullptr, nullptr, nullptr, nullptr},
    {"B10", "u8 -1", 32, false, Fixed, nullptr, nullptr, "0xFF", "u8", nullptr, "wrapped"},
    {"B11", "i8 @0xFF", 32, false, Fixed, nullptr, nullptr, "0xFF", "i8", nullptr, nullptr},
    {"B12", "i8 @-1", 32, false, Fixed, "E_BAD_LITERAL", "a number has no digits", nullptr,
     nullptr, nullptr, nullptr},
    {"B13", "u64 18446744073709551615", 32, false, Fixed, nullptr, nullptr, "0xFFFFFFFFFFFFFFFF",
     "u64", nullptr, nullptr},
    {"B14", "u64 18446744073709551616", 32, false, Fixed, "E_OVERFLOW", "literal exceeds 64 bits",
     nullptr, nullptr, nullptr, nullptr},
    {"B15", "u32 4294967296", 32, false, Fixed, "E_RANGE", "literal does not fit in the selected width",
     nullptr, nullptr, nullptr, nullptr},
    {"B16", "0x", 32, false, Fixed, "E_BAD_LITERAL", "a number has no digits", nullptr, nullptr,
     nullptr, nullptr},

    // ---- C. unit suffixes ---------------------------------------------------
    {"C1", "4M", 32, false, Fixed, nullptr, nullptr, "0x003D0900", "u32", nullptr,
     "4M = 4 x 10^6 = 4000000"},
    {"C2", "4Mi", 32, false, Fixed, nullptr, nullptr, "0x00400000", "u32", nullptr,
     "4Mi = 4 x 2^20 = 4194304"},
    {"C3", "4KB", 32, false, Fixed, nullptr, nullptr, "0x00000FA0", "u32", nullptr,
     "4KB = 4 x 10^3 = 4000"},
    {"C4", "u8 4M", 32, false, Fixed, "E_RANGE", "literal does not fit in the selected width",
     nullptr, nullptr, nullptr, nullptr},
    {"C5", "u64 18446744074G", 32, false, Fixed, "E_OVERFLOW", "unit multiplier exceeds 64 bits",
     nullptr, nullptr, nullptr, nullptr},
    {"C6", "0xAB", 32, false, Fixed, nullptr, nullptr, "0x000000AB", "u32", nullptr,
     nullptr}, // the B is never a byte unit
    {"C7", "0xDEADBEEF", 32, false, Fixed, nullptr, nullptr, "0xDEADBEEF", "u32", nullptr, nullptr},
    {"C8", "4 MB", 32, false, Fixed, "E_UNIT_ATTACH", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"C9", "MB", 32, false, Fixed, "E_UNIT_ATTACH", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"C10a", "1m", 32, false, Fixed, nullptr, nullptr, "0x3F50624DD2F1A9FC", "f64", "0.001",
     "1m = 1 x 10^-3 = 0.001"},
    {"C10b", "u8 1m", 32, false, Fixed, "E_FLOAT_TYPE",
     "an integer type prefix cannot take a float literal", nullptr, nullptr, nullptr, nullptr},
    {"C11a", "1n", 32, false, Fixed, nullptr, nullptr, "0x3E112E0BE826D695", "f64", "1e-09", nullptr},
    {"C11b", "1p", 32, false, Fixed, nullptr, nullptr, "0x3D719799812DEA11", "f64", "1e-12", nullptr},
    {"C11c", "1u", 32, false, Fixed, nullptr, nullptr, "0x3EB0C6F7A0B5ED8D", "f64", "1e-06", nullptr},
    {"C12a", "1\xC2\xB5", 32, false, Fixed, "E_BAD_UNIT", "micro sign", nullptr, nullptr, nullptr,
     nullptr}, // U+00B5 MICRO SIGN
    {"C12b", "1\xCE\xBC", 32, false, Fixed, "E_BAD_UNIT", "micro sign", nullptr, nullptr, nullptr,
     nullptr}, // U+03BC GREEK SMALL LETTER MU
    {"C13a", "micro", 32, false, Fixed, "E_UNIT_ATTACH", nullptr, nullptr, nullptr, nullptr,
     nullptr},
    {"C13b", "4micro", 32, false, Fixed, nullptr, nullptr, "0x3ED0C6F7A0B5ED8D", "f64", "4e-06",
     "4micro = 4 x 10^-6 = 4e-06"},
    {"C14a", "u8 get 0xA5 4M", 32, false, Fixed, "E_BAD_RANGE", "a bit range must look like 7:4",
     nullptr, nullptr, nullptr, nullptr},
    {"C14b", "u8 get 0xA5 4M:0", 32, false, Fixed, "E_UNIT_ON_INDEX", nullptr, nullptr, nullptr,
     nullptr, nullptr},

    // ---- D. float literals and containers ----------------------------------
    {"D1", "f16 1.5", 32, false, Fixed, nullptr, nullptr, "0x3E00", "f16", "1.5", nullptr},
    {"D2", "f32 88", 32, false, Fixed, nullptr, nullptr, "0x42B00000", "f32", "88",
     nullptr}, // under a float prefix a decimal is the value, not a pattern
    {"D3", "f32 0x3FC00000", 32, false, Fixed, nullptr, nullptr, "0x3FC00000", "f32", "1.5", nullptr},
    {"D4", "1.5", 32, false, Fixed, nullptr, nullptr, "0x3FF8000000000000", "f64", "1.5", nullptr},
    {"D5", "-0.125", 32, false, Fixed, nullptr, nullptr, "0xBFC0000000000000", "f64", "-0.125",
     nullptr},
    {"D6", "1_000.25", 32, false, Fixed, nullptr, nullptr, "0x408F420000000000", "f64", "1000.25",
     nullptr},
    {"D7", "f16 65504", 32, false, Fixed, nullptr, nullptr, "0x7BFF", "f16", "6.55e+04", nullptr},
    {"D8", "f16 65535", 32, false, Fixed, nullptr, nullptr, "0x7C00", "f16", "+Inf",
     "f16 range: 65535 is beyond binary16 and encodes as +Inf"},
    {"D9", "f16 1e-9", 32, false, Fixed, nullptr, nullptr, "0x0000", "f16", "0",
     "f16 range: 1e-9 is below the smallest binary16 subnormal and encodes as 0"},
    {"D10", "f16 6e-8", 32, false, Fixed, nullptr, nullptr, "0x0001", "f16", "5.96e-08", nullptr},
    {"D11a", "f32 Inf", 32, false, Fixed, nullptr, nullptr, "0x7F800000", "f32", "+Inf", nullptr},
    {"D11b", "f32 -Inf", 32, false, Fixed, nullptr, nullptr, "0xFF800000", "f32", "-Inf", nullptr},
    {"D11c", "f32 NaN", 32, false, Fixed, nullptr, nullptr, "0x7FC00000", "f32", "NaN", nullptr},
    {"D12", "f16 1e30", 32, false, Fixed, nullptr, nullptr, "0x7C00", "f16", "+Inf", nullptr},
    {"D13a", "f128 1.5", 32, false, Fixed, "E_FLOAT_TYPE",
     "not a float container this tool implements", nullptr, nullptr, nullptr, nullptr},
    {"D13b", "bf16 1.5", 32, false, Fixed, "E_FLOAT_TYPE",
     "not a float container this tool implements", nullptr, nullptr, nullptr, nullptr},
    {"D14", "u8 1.5", 32, false, Fixed, "E_FLOAT_TYPE",
     "an integer type prefix cannot take a float literal", nullptr, nullptr, nullptr, nullptr},
    {"D15", "f32 1.5 + 1", 32, false, Fixed, "E_FLOAT_OP", "a float type takes a single value",
     nullptr, nullptr, nullptr, nullptr},
    {"D16", "f16 0.5 * 2", 32, false, Fixed, "E_FLOAT_OP", "a float type takes a single value",
     nullptr, nullptr, nullptr, nullptr},
    {"D17", "f16 1.0009765625", 32, false, Fixed, nullptr, nullptr, "0x3C01", "f16", "1.001",
     nullptr}, // exactly one ULP above 1.0, so no rounding happens

    // ---- E. type prefixes and mode selection --------------------------------
    {"E1", "u8 200 + 100", 32, false, Fixed, nullptr, nullptr, "0x2C", "u8", nullptr,
     "exceeded the type and wrapped"},
    {"E2", "200 + 100", 8, false, C, nullptr, nullptr, "0x0000012C", "i32", nullptr,
     "result type i32"}, // C mode grows past the default width
    {"E3", "u8 200 + 100", 8, false, C, nullptr, nullptr, "0x2C", "u8", nullptr,
     nullptr}, // an explicit prefix keeps Fixed semantics even in C mode
    {"E4", "u8 3 * 4", 8, false, C, nullptr, nullptr, "0x0C", "u8", nullptr, nullptr},
    {"E6a", "u8 ~0", 32, false, Fixed, nullptr, nullptr, "0xFF", "u8", nullptr, nullptr},
    {"E6b", "~0", 32, false, Fixed, nullptr, nullptr, "0xFFFFFFFF", "u32", nullptr, nullptr},
    {"E7", "u8 ~0xA5", 32, false, Fixed, nullptr, nullptr, "0x5A", "u8", nullptr,
     "complement is masked to the selected width"},

    // ---- F. operators and precedence ----------------------------------------
    {"F1", "1 | 2 ^ 3 & 5", 32, false, Fixed, nullptr, nullptr, "0x00000003", "u32", nullptr, nullptr},
    {"F2", "1 << 2 + 3", 32, false, Fixed, nullptr, nullptr, "0x00000020", "u32", nullptr, nullptr},
    {"F3", "2 + 3 << 1", 32, false, Fixed, nullptr, nullptr, "0x0000000A", "u32", nullptr, nullptr},
    {"F4", "(1 | 2) << 3", 32, false, Fixed, nullptr, nullptr, "0x00000018", "u32", nullptr, nullptr},
    {"F5", "-2 ** 2", 32, false, Fixed, nullptr, nullptr, "0x00000004", "u32", nullptr, nullptr},
    {"F7a", "2 ** 0", 32, false, Fixed, nullptr, nullptr, "0x00000001", "u32", nullptr, nullptr},
    {"F7b", "2 ** -1", 32, false, Fixed, "E_RANGE", "exponent must be non-negative", nullptr,
     nullptr, nullptr, nullptr},
    {"F8a", "7 / 2", 32, false, Fixed, nullptr, nullptr, "0x00000003", "u32", nullptr, nullptr},
    {"F8b", "7 % 2", 32, false, Fixed, nullptr, nullptr, "0x00000001", "u32", nullptr, nullptr},
    {"F9", "i8 -7 / 2", 32, false, Fixed, nullptr, nullptr, "0xFD", "i8", nullptr,
     nullptr}, // division truncates toward zero
    {"F10a", "1 / 0", 32, false, Fixed, "E_DIV_ZERO", "division by zero", nullptr, nullptr, nullptr,
     nullptr},
    {"F10b", "1 % 0", 32, false, Fixed, "E_DIV_ZERO", "modulo by zero", nullptr, nullptr, nullptr,
     nullptr},
    {"F11", "1 2", 32, false, Fixed, "E_SYNTAX", "unexpected token after the expression near '2'",
     nullptr, nullptr, nullptr, nullptr},
    {"F12", "1 +", 32, false, Fixed, "E_SYNTAX", "expected a value", nullptr, nullptr, nullptr,
     nullptr},
    {"F13", "0xA5 ~ 1", 32, false, Fixed, "E_SYNTAX", "near '~'", nullptr, nullptr, nullptr,
     nullptr},
    {"F14", "0 - 2147483647 - 2", 8, false, C, "E_RANGE", "signed subtraction overflows i32",
     nullptr, nullptr, nullptr, nullptr},
    {"F15", "4294967295 + 1", 8, false, C, nullptr, nullptr, "0x00000000", "u32", nullptr,
     "unsigned arithmetic wrapped at 1 step"},
    {"F16", "9223372036854775807 + 1", 8, false, C, "E_RANGE", "signed addition overflows i64",
     nullptr, nullptr, nullptr, nullptr},
    {"F17", "i64 (0 - 9223372036854775807 - 1) / (0 - 1)", 32, false, Fixed, nullptr, nullptr,
     "0x8000000000000000", "i64", nullptr,
     "wrapped"}, // INT64_MIN / -1 stays the same pattern in Fixed

    // ---- G. shifts and rotates ----------------------------------------------
    {"G1", "u8 1 << 7", 32, false, Fixed, nullptr, nullptr, "0x80", "u8", nullptr,
     "logical left shift; bits shifted out are discarded"},
    {"G2", "u8 0x80 >> 1", 32, false, Fixed, nullptr, nullptr, "0x40", "u8", nullptr,
     "logical right shift"},
    {"G3", "i8 @0x80 >> 1", 32, false, Fixed, nullptr, nullptr, "0xC0", "i8", nullptr,
     "arithmetic right shift (sign bit is replicated)"},
    {"G4", "u8 1 rol 9", 32, false, Fixed, nullptr, nullptr, "0x02", "u8", nullptr,
     "count mod width, no bits lost"},
    {"G5", "u8 0x81 ror 4", 32, false, Fixed, nullptr, nullptr, "0x18", "u8", nullptr, nullptr},
    {"G6a", "u8 1 << 8", 32, false, Fixed, "E_SHIFT", "shift count must be smaller than the width",
     nullptr, nullptr, nullptr, nullptr},
    {"G6b", "u8 1 >> 8", 32, false, Fixed, "E_SHIFT", "shift count must be smaller than the width",
     nullptr, nullptr, nullptr, nullptr},
    {"G7a", "u8 1 rol (0 - 1)", 32, false, Fixed, nullptr, nullptr, "0x80", "u8", nullptr,
     nullptr}, // rotate counts are taken mod the width: (0-1)=255, 255 mod 8 = 7
    {"G7b", "i8 1 rol -1", 32, false, Fixed, "E_SHIFT", "rotate count is negative", nullptr,
     nullptr, nullptr, nullptr},
    {"G8", "u8 1 << (0 - 1)", 32, false, Fixed, "E_SHIFT",
     "shift count must be smaller than the width", nullptr, nullptr, nullptr,
     nullptr}, // shift counts are raw u64: 0-1 underflows to a huge count
    {"G9", "u8 1 << (8 / 2)", 32, false, Fixed, nullptr, nullptr, "0x10", "u8", nullptr, nullptr},
    {"G10", "u8 1 << 4M", 32, false, Fixed, "E_UNIT_ON_INDEX", nullptr, nullptr, nullptr, nullptr,
     nullptr},
    {"G11", "u8 1 << 1.5", 32, false, Fixed, "E_FLOAT_NOT_ALLOWED", nullptr, nullptr, nullptr,
     nullptr, nullptr},
    {"G12", "(1) << 31", 8, false, C, nullptr, nullptr, "0x80000000", "i32", nullptr,
     nullptr}, // not the classic shape, so C promotions apply
    {"G13", "u8 0x80 << 1", 32, false, Fixed, nullptr, nullptr, "0x00", "u8", nullptr, nullptr},

    // ---- H. field operations -------------------------------------------------
    {"H1", "u8 get 0xA5 7:4", 32, false, Fixed, nullptr, nullptr, "0x0A", "u8", nullptr,
     "field [7:4]"},
    {"H2", "u8 set 0xA5 7:4 0x3", 32, false, Fixed, nullptr, nullptr, "0x35", "u8", nullptr, nullptr},
    {"H3", "u8 clear 0xA5 7:4", 32, false, Fixed, nullptr, nullptr, "0x05", "u8", nullptr, nullptr},
    {"H4", "u8 toggle 0xA5 7:4", 32, false, Fixed, nullptr, nullptr, "0x55", "u8", nullptr, nullptr},
    {"H5", "u8 set 0xA5 7:4 0x10", 32, false, Fixed, "E_RANGE", nullptr, nullptr, nullptr, nullptr,
     nullptr},
    {"H6a", "u8 get 0xA5 8:0", 32, false, Fixed, "E_BAD_RANGE", nullptr, nullptr, nullptr, nullptr,
     nullptr},
    {"H6b", "u8 get 0xA5 4:7", 32, false, Fixed, "E_BAD_RANGE", nullptr, nullptr, nullptr, nullptr,
     nullptr},
    {"H6c", "u8 get 0xA5 7", 32, false, Fixed, "E_BAD_RANGE", nullptr, nullptr, nullptr, nullptr,
     nullptr},
    {"H7a", "u16 get 0xA5A5 15:12", 32, false, Fixed, nullptr, nullptr, "0x000A", "u16", nullptr,
     nullptr},
    {"H7b", "u8 get 0xA5 15:12", 32, false, Fixed, "E_BAD_RANGE", nullptr, nullptr, nullptr,
     nullptr, nullptr},
    {"H8", "u8 set 0xA5 7:4 0x3 0x9", 32, false, Fixed, "E_SYNTAX", "wrong number of arguments",
     nullptr, nullptr, nullptr, nullptr},

    // ---- I. blanks, single values, input bounds -------------------------------
    {"I1a", "", 32, false, Fixed, "E_EMPTY", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"I1b", "   ", 32, false, Fixed, "E_EMPTY", nullptr, nullptr, nullptr, nullptr, nullptr},
    {"I1c", "u8", 32, false, Fixed, "E_EMPTY", "no expression after the type", nullptr, nullptr,
     nullptr, nullptr},
    {"I2", "0", 32, false, Fixed, nullptr, nullptr, "0x00000000", "u32", nullptr, "no operation"},

    // ---- J. status and detail fields ------------------------------------------
    {"J1", "u8 0xA5 & 0x0F", 32, false, Fixed, nullptr, nullptr, "0x05", "u8", nullptr, nullptr},
    {"J2", "f16 1.5", 32, false, Fixed, nullptr, nullptr, "0x3E00", "f16", "1.5", nullptr},
    {"J3", "f16 65535", 32, false, Fixed, nullptr, nullptr, "0x7C00", "f16", "+Inf", nullptr},
    {"J4", "4M + 1M", 32, false, Fixed, nullptr, nullptr, "0x004C4B40", "u32", nullptr,
     "1M = 1 x 10^6 = 1000000"}, // two unit operands: the last one names the note
};

std::string toHex(const binlab::BitsEval& e)
{
    return binlab::hexUpper(e.result, e.width);
}

void runSpec(const Spec& s)
{
    const std::string label = std::string(s.id) + " `" + s.in + "`";
    const auto r = binlab::evalBits(s.in, s.width, s.sg, s.mode);
    if (s.err) {
        CHECK(r.success == false, label + " fails");
        if (!r.success) {
            CHECK(r.failure.code == s.err, label + " error code");
            if (s.errSub)
                CHECK(r.failure.message.find(s.errSub) != std::string::npos,
                      label + " error message");
        }
        return;
    }
    CHECK(r.success == true, label + " evaluates");
    if (!r.success)
        return;
    const binlab::BitsEval& e = r.value;
    if (s.hex)
        CHECK(toHex(e) == s.hex, label + " result");
    if (s.type)
        CHECK(e.typeName == s.type, label + " type name");
    if (s.floatT) {
        CHECK(e.isFloat, label + " is float");
        if (e.isFloat)
            CHECK(e.floatText == s.floatT, label + " float text");
    } else {
        CHECK(!e.isFloat, label + " is integer");
    }
    if (s.noteSub) {
        const std::string all = e.unitNote + "\n" + e.floatNote + "\n" + e.detail;
        CHECK(all.find(s.noteSub) != std::string::npos, label + " note");
    }
}

void runFieldChecks()
{
    // J1: the detail's bit grid and the bytes chain.
    const auto j1 = binlab::evalBits("u8 0xA5 & 0x0F", 32, false);
    CHECK(j1.success, "J1 evaluates");
    if (j1.success) {
        CHECK(j1.value.detail.find("bits [7:0]") != std::string::npos, "J1 grid header");
        CHECK(j1.value.detail.find("A:") != std::string::npos, "J1 grid operand row");
        CHECK(j1.value.detail.find("B:") != std::string::npos, "J1 grid rhs row");
        CHECK(j1.value.detail.find("=") != std::string::npos, "J1 grid result row");
        CHECK(j1.value.chain == "bytes hex 05", "J1 bytes chain");
        CHECK(j1.value.detail.find("HEX 0x05") != std::string::npos, "J1 hex in detail");
    }
    // J2: the float breakdown line.
    const auto j2 = binlab::evalBits("f16 1.5", 32, false);
    CHECK(j2.success, "J2 evaluates");
    if (j2.success) {
        CHECK(j2.value.detail.find("FLOAT 1.5") != std::string::npos, "J2 float row");
        CHECK(j2.value.detail.find("S=0  E=0xF  M=0x200") != std::string::npos,
              "J2 S/E/M breakdown");
    }
    // J3: the range note rises into the preview.
    const auto j3 = binlab::evalBits("f16 65535", 32, false);
    CHECK(j3.success, "J3 evaluates");
    if (j3.success)
        CHECK(j3.value.preview.find("beyond binary16") != std::string::npos, "J3 note in preview");
}

void runSizeBoundChecks()
{
    // Over the gate: named, not silently evaluated.
    CHECK(binlab::evalBits(std::string(17000, '1'), 32, false).failure.code == "E_INPUT_TOO_LARGE",
          "input over 16 KiB is named");
    // At the gate: the size check passes and the failure is about the value.
    CHECK(binlab::evalBits(std::string(16384, '1'), 32, false).failure.code == "E_OVERFLOW",
          "input at the 16 KiB boundary parses");
    CHECK(binlab::evalBits(std::string(16385, '1'), 32, false).failure.code == "E_INPUT_TOO_LARGE",
          "input one past the boundary is named");
}

} // namespace

int main()
{
    for (const Spec& s : kSpec)
        runSpec(s);
    runFieldChecks();
    runSizeBoundChecks();
    std::cout << (g_failed == 0 ? "OK" : "FAILED") << ": " << g_total << " checks, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
