// test_logic.cpp - self-contained tests for logic.h (no plugin host needed).
//
// Build: cmake -B build && cmake --build build && ctest --test-dir build
// The layout tests compare against real compiler output produced by
// #pragma pack, sizeof, alignof and offsetof in this translation unit.
#include "logic.h"
#include "formats.h"
#include "formats_toml.h"

#include <filesystem>
#include <fstream>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_total = 0;
int g_failed = 0;

#define CHECK(condition, label)                                                                    \
    do {                                                                                       \
        ++g_total;                                                                             \
        if (!(condition)) {                                                                    \
            ++g_failed;                                                                        \
            std::cerr << "FAIL [" << (label) << "] at line " << __LINE__ << "\n";              \
        }                                                                                      \
    } while (false)

std::uint64_t bitsOf(const std::string& expression, bool& ok, int width = 32, bool isSigned = false)
{
    const auto r = binlab::evalBits(expression, width, isSigned);
    ok = r.success;
    return ok ? r.value.result : 0;
}

std::string errorCode(const binlab::Outcome<binlab::BitsEval>& r)
{
    return r.success ? std::string() : r.failure.code;
}

// ---- compiler oracles ---------------------------------------------------------

struct NaturalFrame {
    std::uint8_t sof;
    std::uint32_t seq;
    std::uint16_t len;
    std::uint8_t data[3];
};

#pragma pack(push, 1)
struct Pack1Frame {
    std::uint8_t sof;
    std::uint32_t seq;
    std::uint16_t len;
    std::uint8_t data[3];
};
#pragma pack(pop)

#pragma pack(push, 2)
struct Pack2Frame {
    std::uint8_t sof;
    std::uint32_t seq;
    std::uint16_t len;
    std::uint8_t data[3];
};
#pragma pack(pop)

#pragma pack(push, 4)
struct Pack4Frame {
    std::uint8_t sof;
    std::uint32_t seq;
    std::uint16_t len;
    std::uint8_t data[3];
};
#pragma pack(pop)

#pragma pack(push, 8)
struct Pack8Frame {
    std::uint8_t sof;
    std::uint32_t seq;
    std::uint16_t len;
    std::uint8_t data[3];
};
#pragma pack(pop)

#pragma pack(push, 16)
struct Pack16Frame {
    std::uint8_t sof;
    std::uint32_t seq;
    std::uint16_t len;
    std::uint8_t data[3];
};
#pragma pack(pop)

#pragma pack(push, 2)
struct NestedInner {
    std::uint8_t a;
    std::uint16_t b;
};
struct NestedOuter {
    NestedInner i;
    std::uint32_t c;
};
union PackedUnion {
    std::uint8_t a;
    std::uint32_t b;
    std::uint16_t c;
};
#pragma pack(pop)

std::uint64_t memberOffset(const binlab::Layout& l, const std::string& name, bool& found)
{
    for (const binlab::MemberRow& m : l.members) {
        if (m.kind == "member" && m.name == name) {
            found = true;
            return m.offset;
        }
    }
    found = false;
    return 0;
}

void checkFrameOracle(int pack, std::size_t expectedSize, std::size_t expectedAlign,
                      std::size_t expectedSeq, std::size_t expectedLen,
                      std::size_t expectedData, const std::string& label)
{
    const std::string query = "pack=" + std::to_string(pack)
                              + " struct Frame { u8 sof; u32 seq; u16 len; u8 data[3]; };";
    const auto r = binlab::evalStruct(query, 0);
    CHECK(r.success, label + " parses");
    if (!r.success) {
        std::cerr << "      error " << r.failure.code << ": " << r.failure.message << "\n";
        return;
    }
    const binlab::Layout& l = r.value;
    CHECK(l.size == expectedSize, label + " sizeof");
    CHECK(l.align == expectedAlign, label + " alignof");
    bool found = false;
    CHECK(memberOffset(l, "seq", found) == expectedSeq && found, label + " offsetof(seq)");
    CHECK(memberOffset(l, "len", found) == expectedLen && found, label + " offsetof(len)");
    CHECK(memberOffset(l, "data", found) == expectedData && found, label + " offsetof(data)");
    CHECK(l.assertions.find("== " + std::to_string(expectedSeq)) != std::string::npos,
          label + " static_assert text");
}

// ---- record format helpers ----------------------------------------------------
// Little-endian packers, the C++ twin of hextract's struct.pack('<...').
void putLE(std::vector<std::uint8_t>& out, std::uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i)
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::vector<std::uint8_t> interlockBlob()
{
    std::vector<std::uint8_t> b;
    const auto pkt = [&](std::uint32_t s, std::uint32_t ns, std::uint32_t id, std::uint32_t addr,
                         std::vector<std::int32_t> integral, std::vector<std::uint32_t> count) {
        putLE(b, s, 4); putLE(b, ns, 4); putLE(b, id, 4); putLE(b, addr, 4);
        for (std::int32_t v : integral) putLE(b, static_cast<std::uint32_t>(v), 4);
        for (std::uint32_t v : count) putLE(b, v, 4);
    };
    pkt(100, 200, 1, 0xabc, {1, 2, 3, 4, 5, 6}, {7, 0, 0, 0, 0, 0});
    pkt(100, 200, 2, 0xabc, {-5, 2, 3, 4, 5, 6}, {0, 0, 0, 0, 0, 0});
    pkt(0, 0, 0, 0, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0});
    return b;
}

std::vector<std::uint8_t> msbFirst(const std::vector<std::uint8_t>& blob, std::size_t wb)
{
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < blob.size(); i += wb)
        for (std::size_t k = 0; k < wb; ++k)
            out.push_back(blob[i + wb - 1 - k]);
    return out;
}

std::string toHex(const std::vector<std::uint8_t>& b)
{
    std::string s;
    char cell[4];
    for (std::uint8_t v : b) {
        std::snprintf(cell, sizeof cell, "%02x", v);
        s += cell;
    }
    return s;
}

const binlab::Cell& cellOf(const binlab::RecordFormat& f, const binlab::Record& r, const std::string& column)
{
    for (std::size_t i = 0; i < f.columns.size(); ++i)
        if (f.columns[i] == column)
            return r.cells[i];
    static binlab::Cell none;
    return none;
}

binlab::Outcome<binlab::RecordFormat> compileOne(const std::string& name, int wordBits,
                                                std::vector<binlab::RawField> fields,
                                                std::vector<binlab::RawRule> rules = {})
{
    binlab::RawFormat raw;
    raw.name = name;
    raw.source = "test";
    raw.wordBits = wordBits;
    raw.byteOrder = "lsb-first";
    raw.fields = std::move(fields);
    raw.rules = std::move(rules);
    return binlab::compileFormat(raw);
}

binlab::RawField rawField(const char* name, const char* type, int count = 1)
{
    binlab::RawField f;
    f.name = name;
    f.type = type;
    f.count = count;
    return f;
}

} // namespace

int main()
{
    // ---- bits: normal inputs --------------------------------------------------
    bool ok = false;
    CHECK(bitsOf("u8 0xA5 & 0x0F", ok) == 0x05 && ok, "bits u8 0xA5 & 0x0F");
    CHECK(bitsOf("u8 0xA5 | 0x0F", ok) == 0xAF && ok, "bits OR");
    CHECK(bitsOf("u8 0xA5 ^ 0xFF", ok) == 0x5A && ok, "bits XOR");
    CHECK(bitsOf("u8 ~0xA5", ok) == 0x5A && ok, "bits complement");
    CHECK(bitsOf("u8 0x01 << 7", ok) == 0x80 && ok, "bits left shift");
    CHECK(bitsOf("u8 0x80 >> 1", ok) == 0x40 && ok, "bits logical right shift");
    CHECK(bitsOf("u8 0xA5 rol 1", ok) == 0x4B && ok, "bits rol");
    CHECK(bitsOf("u8 0xA5 ror 1", ok) == 0xD2 && ok, "bits ror");
    CHECK(bitsOf("u8 0xA5 rol 8", ok) == 0xA5 && ok, "bits rol modulo width");
    CHECK(bitsOf("u16 0x1234 & 0x00FF", ok) == 0x34 && ok, "bits u16 mask");
    CHECK(bitsOf("u32 0xDEADBEEF", ok) == 0xDEADBEEFull && ok, "bits single value");
    CHECK(bitsOf("0xFF", ok, 32, false) == 0xFF && ok, "bits default width from settings");
    CHECK(bitsOf("u64 0x8000000000000000 >> 63", ok) == 1 && ok, "bits u64 wide shift");
    CHECK(bitsOf("u64 1 << 63", ok) == 0x8000000000000000ull && ok, "bits no 1<<64 UB");
    CHECK(bitsOf("u8 get 0xA5 7:4", ok) == 0x0A && ok, "bits field get");
    CHECK(bitsOf("u8 set 0xA5 7:4 0x3", ok) == 0x35 && ok, "bits field set");
    CHECK(bitsOf("u8 clear 0xA5 7:4", ok) == 0x05 && ok, "bits field clear");
    CHECK(bitsOf("u8 toggle 0xA5 7:4", ok) == 0x55 && ok, "bits field toggle");
    {
        const auto r = binlab::evalBits("i8 -2 >> 1", 32, false);
        CHECK(r.success && binlab::asSigned(r.value.result, 8) == -1, "bits arithmetic shift keeps the sign");
        const auto raw = binlab::evalBits("i8 @0xFF", 32, false);
        CHECK(raw.success && binlab::asSigned(raw.value.result, 8) == -1, "bits raw bit pattern reinterpretation");
        const auto wide = binlab::evalBits("u32 0x1", 32, false);
        CHECK(wide.success && wide.value.chain == "bytes hex 01 00 00 00", "bits chain into bytes");
        const auto repr = binlab::evalBits("u8 0xA5", 32, false);
        CHECK(repr.success && repr.value.detail.find("0o245") != std::string::npos,
              "bits octal rendering");
    }

    // ---- bits: empty, invalid and boundary inputs -----------------------------
    CHECK(errorCode(binlab::evalBits("", 32, false)) == "E_EMPTY", "bits empty input");
    CHECK(errorCode(binlab::evalBits("   ", 32, false)) == "E_EMPTY", "bits whitespace input");
    CHECK(errorCode(binlab::evalBits("u8", 32, false)) == "E_EMPTY", "bits type without expression");
    CHECK(errorCode(binlab::evalBits("0x", 32, false)) == "E_BAD_LITERAL", "bits empty hex literal");
    CHECK(errorCode(binlab::evalBits("0b102", 32, false)) == "E_BAD_LITERAL", "bits bad binary digit");
    CHECK(errorCode(binlab::evalBits("0o8", 32, false)) == "E_BAD_LITERAL", "bits bad octal digit");
    CHECK(errorCode(binlab::evalBits("u8 256", 32, false)) == "E_RANGE", "bits unsigned overflow");
    CHECK(errorCode(binlab::evalBits("i8 128", 32, false)) == "E_RANGE", "bits signed positive overflow");
    CHECK(errorCode(binlab::evalBits("u8 1 << 8", 32, false)) == "E_SHIFT", "bits shift >= width");
    CHECK(errorCode(binlab::evalBits("u8 1 >> 8", 32, false)) == "E_SHIFT", "bits right shift >= width");
    CHECK(errorCode(binlab::evalBits("u8 0xA5 +", 32, false)) == "E_SYNTAX", "bits dangling operator");
    CHECK(errorCode(binlab::evalBits("u8 1 2 3 4", 32, false)) == "E_SYNTAX", "bits wrong arity");
    CHECK(errorCode(binlab::evalBits("u8 get 0xA5 8:0", 32, false)) == "E_BAD_RANGE", "bits field range too wide");
    CHECK(errorCode(binlab::evalBits("u8 get 0xA5 4:7", 32, false)) == "E_BAD_RANGE", "bits inverted field range");
    CHECK(errorCode(binlab::evalBits("u8 set 0xA5 7:4 0x10", 32, false)) == "E_RANGE", "bits field value too wide");
    CHECK(errorCode(binlab::evalBits(std::string(17000, 'A'), 32, false)) == "E_INPUT_TOO_LARGE",
          "bits input size cap");

    // ---- bits: general expressions (precedence, associativity, parentheses) ---
    CHECK(bitsOf("u8 1 + 2 * 3", ok) == 7 && ok, "bits mul binds tighter than add");
    CHECK(bitsOf("u8 (1 + 2) * 3", ok) == 9 && ok, "bits parentheses regroup");
    CHECK(bitsOf("u8 1 << 2 + 1", ok) == 8 && ok, "bits add binds tighter than shift");
    CHECK(bitsOf("u8 0xF & 3 | 4", ok) == 7 && ok, "bits and binds tighter than or");
    CHECK(bitsOf("u8 1 ^ 3 & 1", ok) == 0 && ok, "bits and binds tighter than xor");
    CHECK(bitsOf("u16 2 ** 3 ** 2", ok) == 512 && ok, "bits power is right associative");
    CHECK(bitsOf("i8 -2 ** 2", ok) == 4 && ok, "bits unary minus binds tighter than power");
    CHECK(bitsOf("u8 (~0) & 0x0F", ok) == 0x0F && ok, "bits complement inside parentheses");
    CHECK(bitsOf("u8 7 / 2", ok) == 3 && ok, "bits integer division truncates");
    CHECK(bitsOf("u8 7 % 2", ok) == 1 && ok, "bits modulo");
    CHECK(bitsOf("u16 2 ** 9", ok) == 512 && ok, "bits power");
    CHECK(bitsOf("u8 (0xA5)", ok) == 0xA5 && ok, "bits parenthesized single value");
    CHECK(bitsOf("u8 1 << 8 - 1", ok) == 0x80 && ok, "bits count is a full expression");
    CHECK(bitsOf("u8 4M + 0", ok) == 4000000 % 256 && ok, "bits unit suffix inside expression");
    {
        const auto note = binlab::evalBits("u8 200 + 100", 32, false);
        CHECK(note.success && note.value.result == 0x2C, "bits fixed-mode add wraps");
        CHECK(note.value.detail.find("wrapped") != std::string::npos,
              "bits fixed-mode wrap is reported in the note");
        const auto signedWrap = binlab::evalBits("i8 -128 - 1", 32, false);
        CHECK(signedWrap.success && binlab::asSigned(signedWrap.value.result, 8) == 127,
              "bits fixed-mode signed underflow wraps");
        const auto trunc = binlab::evalBits("u8 1 << 7", 32, false);
        CHECK(trunc.success, "bits classic shift still works");
    }
    {
        using EM = binlab::EvalMode;
        // C mode: promotions, derived result type, signed overflow errors.
        const auto promoted = binlab::evalBits("1 + 2", 8, false, EM::C);
        CHECK(promoted.success && promoted.value.result == 3 && promoted.value.typeName == "i32",
              "bits C mode promotes literals to i32");
        const auto wide = binlab::evalBits("0xFFFFFFFF + 0", 8, false, EM::C);
        CHECK(wide.success && wide.value.typeName == "u32" && wide.value.result == 0xFFFFFFFFull,
              "bits C mode types big literals u32");
        CHECK(errorCode(binlab::evalBits("2147483647 + 1", 8, false, EM::C)) == "E_RANGE",
              "bits C mode signed add overflow errors");
        CHECK(errorCode(binlab::evalBits("9223372036854775807 + 1", 8, false, EM::C)) == "E_RANGE",
              "bits C mode i64 add overflow errors");
        CHECK(errorCode(binlab::evalBits("(-9223372036854775807 - 1) / -1", 8, false, EM::C))
                  == "E_RANGE", "bits C mode INT64_MIN / -1 errors");
        CHECK(errorCode(binlab::evalBits("2 ** 62", 8, false, EM::C)) == "E_RANGE",
              "bits C mode power overflow errors");
        CHECK(errorCode(binlab::evalBits("1 << 40", 8, false, EM::C)) == "E_SHIFT",
              "bits C mode shift count >= promoted width errors");
        CHECK(errorCode(binlab::evalBits("1 / 0", 8, false, EM::C)) == "E_DIV_ZERO",
              "bits C mode division by zero");
        CHECK(errorCode(binlab::evalBits("2 ** -1", 32, false)) == "E_RANGE",
              "bits negative exponent errors");
        // C mode wraps unsigned by definition; the note names it.
        const auto uwrap = binlab::evalBits("0xFFFFFFFF + 1", 8, false, EM::C);
        CHECK(uwrap.success && uwrap.value.result == 0, "bits C mode unsigned wrap is defined");
        // Explicit prefix forces fixed semantics even in C mode.
        const auto prefixed = binlab::evalBits("u8 200 + 100", 8, false, EM::C);
        CHECK(prefixed.success && prefixed.value.result == 0x2C && prefixed.value.typeName == "u8",
              "bits explicit prefix keeps fixed semantics in C mode");
    }
    CHECK(errorCode(binlab::evalBits("u8 1 / 0", 32, false)) == "E_DIV_ZERO", "bits fixed divide by zero");
    CHECK(errorCode(binlab::evalBits("u8 1 << 300", 32, false)) == "E_SHIFT", "bits huge shift count");
    CHECK(errorCode(binlab::evalBits("u8 (1 + 2", 32, false)) == "E_SYNTAX", "bits unclosed parenthesis");
    CHECK(errorCode(binlab::evalBits("u8 1.5 + 1", 32, false)) == "E_FLOAT_OP",
          "bits float literal rejected in expressions");
    CHECK(errorCode(binlab::evalBits("f32 1.5 + 1", 32, false)) == "E_FLOAT_OP",
          "bits float prefix takes a single value");

    // ---- bits: round-trip property --------------------------------------------
    {
        std::uint64_t state = 0x9E3779B97F4A7C15ull;
        int good = 0;
        for (int i = 0; i < 512; ++i) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const std::string expression = "u64 " + binlab::hexUpper(state, 64);
            const auto r = binlab::evalBits(expression, 64, false);
            if (r.success && r.value.result == state)
                ++good;
        }
        CHECK(good == 512, "bits u64 round-trip over 512 pseudo-random values");
    }

    // ---- bytes: normal inputs --------------------------------------------------
    {
        const auto r = binlab::evalBytes("hex DE AD BE EF", "le");
        CHECK(r.success && r.value.data.size() == 4 && r.value.data[0] == 0xDE && r.value.data[3] == 0xEF,
              "bytes hex tokens");
        const auto compact = binlab::evalBytes("hex DEADBEEF", "le");
        CHECK(compact.success && compact.value.normalized == "DE AD BE EF", "bytes compact hex token");
        const auto separated = binlab::evalBytes("hex 0xDE,AD BE:EF", "le");
        CHECK(separated.success && separated.value.normalized == "DE AD BE EF", "bytes separators and 0x prefix");
        const auto bin = binlab::evalBytes("bin 10101010 00001111", "le");
        CHECK(bin.success && bin.value.normalized == "AA 0F", "bytes bin tokens");
        const auto oct = binlab::evalBytes("oct 377 040 000", "le");
        CHECK(oct.success && oct.value.normalized == "FF 20 00", "bytes oct tokens");
        const auto le = binlab::evalBytes("hex u16le 34 12", "le");
        CHECK(le.success && !le.value.words.empty() && le.value.words[0].primary == "0x1234", "bytes u16le");
        const auto be = binlab::evalBytes("hex u16be 12 34", "le");
        CHECK(be.success && !be.value.words.empty() && be.value.words[0].primary == "0x1234", "bytes u16be");
        const auto defaultEndian = binlab::evalBytes("hex u16 34 12", "le");
        CHECK(defaultEndian.success && !defaultEndian.value.words.empty()
                  && defaultEndian.value.words[0].primary == "0x1234", "bytes default endian le");
        const auto defaultEndianBe = binlab::evalBytes("hex u16 12 34", "be");
        CHECK(defaultEndianBe.success && !defaultEndianBe.value.words.empty()
                  && defaultEndianBe.value.words[0].primary == "0x1234", "bytes default endian be");
        const auto word32 = binlab::evalBytes("hex u32le 78 56 34 12", "le");
        CHECK(word32.success && !word32.value.words.empty() && word32.value.words[0].primary == "0x12345678",
              "bytes u32le");
        const auto f32le = binlab::evalBytes("hex f32le 00 00 80 3F", "le");
        CHECK(f32le.success && !f32le.value.words.empty() && f32le.value.words[0].primary == "1",
              "bytes f32le 1.0");
        const auto f32be = binlab::evalBytes("hex f32be 3F 80 00 00", "le");
        CHECK(f32be.success && !f32be.value.words.empty() && f32be.value.words[0].primary == "1",
              "bytes f32be 1.0");
        const auto f64 = binlab::evalBytes("hex f64le 00 00 00 00 00 00 F0 3F", "le");
        CHECK(f64.success && !f64.value.words.empty() && f64.value.words[0].primary == "1", "bytes f64le 1.0");
        const auto signedWord = binlab::evalBytes("hex i8 FF", "le");
        CHECK(signedWord.success && !signedWord.value.words.empty() && signedWord.value.words[0].primary == "-1",
              "bytes i8 signed decoding");
        const auto trailing = binlab::evalBytes("hex u16le 34 12 78", "le");
        CHECK(trailing.success && trailing.value.trailing == 1 && trailing.value.words.size() == 1,
              "bytes trailing bytes are reported, not fabricated");
        const auto noWords = binlab::evalBytes("hex u16le 34", "le");
        CHECK(noWords.success && noWords.value.words.empty() && noWords.value.trailing == 1,
              "bytes no complete word");
        CHECK(le.success && le.value.dump.find("3412") == std::string::npos
                  && le.value.dump.find("34 12") != std::string::npos, "bytes hexdump layout");
    }

    // ---- bytes: empty, invalid, dependency-free failure -----------------------
    CHECK(!binlab::evalBytes("", "le").success, "bytes empty input");
    CHECK(!binlab::evalBytes("hex", "le").success, "bytes no data");
    CHECK(binlab::evalBytes("foo 00", "le").failure.code == "E_UNKNOWN_FORMAT", "bytes unknown format");
    CHECK(binlab::evalBytes("hex ABC", "le").failure.code == "E_HEX_ODD", "bytes odd hex token");
    CHECK(binlab::evalBytes("bin 1010", "le").failure.code == "E_BIN_WIDTH", "bytes bin token width");
    CHECK(binlab::evalBytes("oct 400", "le").failure.code == "E_OCT_RANGE", "bytes oct token range");
    CHECK(binlab::evalBytes("hex ZZ", "le").failure.code == "E_BAD_TOKEN", "bytes bad hex digit");
    {
        std::string big = "hex ";
        for (int i = 0; i < 4097; ++i)
            big += "FF ";
        CHECK(binlab::evalBytes(big, "le").failure.code == "E_BYTES_TOO_LARGE", "bytes 4096 byte cap");
    }
    CHECK(binlab::evalBytes(std::string(17000, 'A'), "le").failure.code == "E_INPUT_TOO_LARGE",
          "bytes input size cap");

    // ---- struct: hand-computed layouts ----------------------------------------
    {
        const auto natural = binlab::evalStruct("struct S { u8 a; u32 b; u16 c; };", 0);
        CHECK(natural.success && natural.value.size == 12 && natural.value.align == 4, "struct natural size/align");
        bool found = false;
        CHECK(natural.success && memberOffset(natural.value, "b", found) == 4 && found,
              "struct natural offsetof(b)");
        CHECK(natural.success && memberOffset(natural.value, "c", found) == 8 && found,
              "struct natural offsetof(c)");
        const auto packed = binlab::evalStruct("pack=1 struct S { u8 a; u32 b; u16 c; };", 0);
        CHECK(packed.success && packed.value.size == 7 && packed.value.align == 1, "struct pack=1 size/align");
        const auto frame = binlab::evalStruct(
            "pack=4 struct Frame { u8 sof; u32 seq; u16 len; u8 data[3]; };", 0);
        CHECK(frame.success && frame.value.size == 16 && frame.value.align == 4, "struct pack=4 frame");
        CHECK(frame.success && frame.value.chain.find("pack=1") != std::string::npos,
              "struct chain switches pack");
        CHECK(frame.success && frame.value.text.find("static_assert(sizeof(Frame) == 16") != std::string::npos,
              "struct static_assert text");
        CHECK(frame.success && frame.value.byteMap.find("A...BBBB") != std::string::npos,
              "struct byte map");
        const auto defaultPack = binlab::evalStruct("struct S { u8 a; u32 b; };", 1);
        CHECK(defaultPack.success && defaultPack.value.size == 5, "struct default pack from settings");
        const auto uni = binlab::evalStruct("union U { u8 a; u32 b; u16 c; };", 0);
        CHECK(uni.success && uni.value.size == 4 && uni.value.align == 4 && uni.value.isUnion,
              "union size/align");
        CHECK(uni.success && memberOffset(uni.value, "b", found) == 0 && found, "union members start at 0");
        const auto array = binlab::evalStruct("struct S { u8 a[3]; u32 b; };", 0);
        CHECK(array.success && array.value.size == 8 && memberOffset(array.value, "b", found) == 4 && found,
              "struct array member");
        const auto nested = binlab::evalStruct(
            "struct Inner { u8 a; u16 b; }; struct Outer { Inner i; u32 c; };", 0);
        CHECK(nested.success && nested.value.size == 8 && nested.value.align == 4,
              "struct named nested type reference");
        const auto inlineNested = binlab::evalStruct(
            "struct S { struct { u8 a; u16 b; } inner; u32 c; };", 0);
        CHECK(inlineNested.success && inlineNested.value.size == 8 && inlineNested.value.align == 4,
              "struct inline nested type");
        const auto namedInline = binlab::evalStruct(
            "struct S { struct Hdr { u8 a; u16 b; } h; u32 c; };", 0);
        CHECK(namedInline.success && namedInline.value.size == 8, "struct named inline nested type");
    }

    // ---- struct: invalid inputs ------------------------------------------------
    CHECK(binlab::evalStruct("struct S { int a; };", 0).failure.code == "E_UNSUPPORTED_TYPE",
          "struct rejects int");
    CHECK(binlab::evalStruct("struct S { u8 a:3; };", 0).failure.code == "E_BITFIELD",
          "struct rejects bitfields");
    CHECK(binlab::evalStruct("struct S { u8 *a; };", 0).failure.code == "E_POINTER",
          "struct rejects pointers");
    CHECK(binlab::evalStruct("struct S { u8 a; u8 a; };", 0).failure.code == "E_DUPLICATE_MEMBER",
          "struct rejects duplicate members");
    CHECK(binlab::evalStruct("struct S { };", 0).failure.code == "E_EMPTY_STRUCT",
          "struct rejects empty structs");
    CHECK(binlab::evalStruct("struct S { u8 a[0]; };", 0).failure.code == "E_ARRAY_SIZE",
          "struct rejects zero-length arrays");
    CHECK(binlab::evalStruct("struct S { Missing a; };", 0).failure.code == "E_UNKNOWN_TYPE",
          "struct rejects unknown types");
    CHECK(binlab::evalStruct("struct S { Later a; }; struct Later { u8 b; };", 0).failure.code == "E_UNKNOWN_TYPE",
          "struct rejects forward references");
    CHECK(binlab::evalStruct("pack=3 struct S { u8 a; };", 0).failure.code == "E_BAD_PACK",
          "struct rejects unsupported pack");
    CHECK(binlab::evalStruct("struct S { u8 a; u8 b; u8 c; u8 d;", 0).failure.code == "E_SYNTAX",
          "struct rejects truncated definitions");
    CHECK(binlab::evalStruct("struct S { u8 a[4096]; u8 b[4096]; };", 0).failure.code == "E_STRUCT_TOO_LARGE",
          "struct rejects oversized layouts");
    CHECK(binlab::evalStruct(std::string(17000, 'A'), 0).failure.code == "E_INPUT_TOO_LARGE",
          "struct input size cap");

    // ---- struct: real compiler oracle -----------------------------------------
    checkFrameOracle(0, sizeof(NaturalFrame), alignof(NaturalFrame), offsetof(NaturalFrame, seq),
                     offsetof(NaturalFrame, len), offsetof(NaturalFrame, data), "oracle natural");
    checkFrameOracle(1, sizeof(Pack1Frame), alignof(Pack1Frame), offsetof(Pack1Frame, seq),
                     offsetof(Pack1Frame, len), offsetof(Pack1Frame, data), "oracle pack=1");
    checkFrameOracle(2, sizeof(Pack2Frame), alignof(Pack2Frame), offsetof(Pack2Frame, seq),
                     offsetof(Pack2Frame, len), offsetof(Pack2Frame, data), "oracle pack=2");
    checkFrameOracle(4, sizeof(Pack4Frame), alignof(Pack4Frame), offsetof(Pack4Frame, seq),
                     offsetof(Pack4Frame, len), offsetof(Pack4Frame, data), "oracle pack=4");
    checkFrameOracle(8, sizeof(Pack8Frame), alignof(Pack8Frame), offsetof(Pack8Frame, seq),
                     offsetof(Pack8Frame, len), offsetof(Pack8Frame, data), "oracle pack=8");
    checkFrameOracle(16, sizeof(Pack16Frame), alignof(Pack16Frame), offsetof(Pack16Frame, seq),
                     offsetof(Pack16Frame, len), offsetof(Pack16Frame, data), "oracle pack=16");
    {
        const auto r = binlab::evalStruct(
            "pack=2 struct Inner { u8 a; u16 b; }; struct Outer { Inner i; u32 c; };", 0);
        CHECK(r.success && r.value.size == sizeof(NestedOuter) && r.value.align == alignof(NestedOuter),
              "oracle nested sizeof/alignof");
        bool found = false;
        CHECK(r.success && memberOffset(r.value, "c", found) == offsetof(NestedOuter, c) && found,
              "oracle nested offsetof(c)");
        const auto u = binlab::evalStruct("pack=2 union U { u8 a; u32 b; u16 c; };", 0);
        CHECK(u.success && u.value.size == sizeof(PackedUnion) && u.value.align == alignof(PackedUnion),
              "oracle packed union");
    }

    // ---- unit suffixes (SI / IEC, pure ASCII, decimal literals only) --------
    {
        bool ok = false;
        CHECK(bitsOf("4M", ok, 64) == 4000000ULL && ok, "unit 4M = 4 x 10^6");
        CHECK(bitsOf("4MB", ok, 64) == 4000000ULL && ok, "unit 4MB follows SI 1000");
        CHECK(bitsOf("4Mi", ok, 64) == 4194304ULL && ok, "unit 4Mi = 4 x 2^20");
        CHECK(bitsOf("4MiB", ok, 64) == 4194304ULL && ok, "unit 4MiB longest match");
        CHECK(bitsOf("1k", ok, 64) == 1000ULL && ok, "unit 1k");
        CHECK(bitsOf("1K", ok, 64) == 1000ULL && ok, "unit 1K");
        CHECK(bitsOf("1Ki", ok, 64) == 1024ULL && ok, "unit 1Ki");
        CHECK(bitsOf("2Gi", ok, 64) == (2ULL << 30) && ok, "unit 2Gi");
        CHECK(bitsOf("1T", ok, 64) == 1000000000000ULL && ok, "unit 1T");
        CHECK(bitsOf("1TiB", ok, 64) == (1ULL << 40) && ok, "unit 1TiB");
        CHECK(bitsOf("0xAB", ok, 64) == 0xABULL && ok, "hex 0xAB keeps its B digit");
        CHECK(bitsOf("0xDEADBEEF", ok, 64) == 0xDEADBEEFULL && ok, "hex digits are not unit suffixes");
        CHECK(bitsOf("0x1E6", ok, 64) == 0x1E6ULL && ok, "hex E stays a digit");
        CHECK(bitsOf("1_000", ok, 64) == 1000ULL && ok, "underscore separator still works");
        CHECK(bitsOf("4M & 0xFF", ok, 64) == 0 && ok, "units work inside a bitwise expression");
        const auto note = binlab::evalBits("4Mi", 64, false);
        CHECK(note.success && note.value.unitNote == "4Mi = 4 x 2^20 = 4194304",
              "unit conversion is reported, never silent");
        CHECK(errorCode(binlab::evalBits("4 MB", 32, false)) == "E_UNIT_ATTACH",
              "a detached suffix gets a specific error");
        CHECK(errorCode(binlab::evalBits("get 0xA5 7K:4", 32, false)) == "E_UNIT_ON_INDEX",
              "bit indices reject unit suffixes");
        CHECK(errorCode(binlab::evalBits("8G", 32, false)) == "E_RANGE",
              "a scaled value still respects the selected width");
        CHECK(errorCode(binlab::evalBits("1\xC2\xB5", 32, false)) == "E_BAD_UNIT",
              "U+00B5 micro sign points at the ASCII spellings");
        CHECK(errorCode(binlab::evalBits("1\xCE\xBC", 32, false)) == "E_BAD_UNIT",
              "U+03BC greek mu points at the ASCII spellings");
        const auto micro = binlab::evalBits("1\xC2\xB5", 32, false);
        CHECK(!micro.success && micro.failure.hint.find("1e-6") != std::string::npos,
              "micro-sign error suggests 'u', 'micro' or 1e-6");
    }

    // ---- IEEE-754 bit-pattern literals ---------------------------------------
    {
        bool ok = false;
        CHECK(bitsOf("1.5", ok, 64) == 0x3FF8000000000000ULL && ok, "f64 1.5 bit pattern");
        CHECK(bitsOf(".5", ok, 64) == 0x3FE0000000000000ULL && ok, "f64 .5 bit pattern");
        CHECK(bitsOf("1e6", ok, 64) == 0x412E848000000000ULL && ok, "f64 1e6 bit pattern");
        CHECK(bitsOf("1_000.25", ok, 64) == 0x408F420000000000ULL && ok, "f64 1000.25 bit pattern");
        CHECK(bitsOf("0.1", ok, 64) == 0x3FB999999999999AULL && ok, "f64 0.1 bit pattern");
        CHECK(bitsOf("-0.125", ok, 64) == 0xBFC0000000000000ULL && ok, "f64 -0.125 sign bit");
        CHECK(bitsOf("f32 1.5", ok, 64) == 0x3FC00000ULL && ok, "f32 1.5 bit pattern");
        CHECK(bitsOf("f32 0.1", ok, 64) == 0x3DCCCCCDULL && ok, "f32 0.1 bit pattern");
        CHECK(bitsOf("Inf", ok, 64) == 0x7FF0000000000000ULL && ok, "f64 +Inf bit pattern");
        CHECK(bitsOf("-Inf", ok, 64) == 0xFFF0000000000000ULL && ok, "f64 -Inf bit pattern");
        CHECK(bitsOf("1u", ok, 64) == 0x3EB0C6F7A0B5ED8DULL && ok, "1u = micro = 1e-6");
        CHECK(bitsOf("1micro", ok, 64) == 0x3EB0C6F7A0B5ED8DULL && ok, "1micro = 1u");
        CHECK(bitsOf("1m", ok, 64) == 0x3F50624DD2F1A9FCULL && ok, "1m = milli = 1e-3");
        CHECK(bitsOf("1n", ok, 64) == 0x3E112E0BE826D695ULL && ok, "1n = nano");
        CHECK(bitsOf("1p", ok, 64) == 0x3D719799812DEA11ULL && ok, "1p = pico");
        CHECK(bitsOf("f32 0x3FC00000", ok, 64) == 0x3FC00000ULL && ok, "f32 accepts a raw pattern");
        const auto shortest = binlab::evalBits("0.1", 64, false);
        CHECK(shortest.success && shortest.value.isFloat && shortest.value.floatText == "0.1"
              && shortest.value.typeName == "f64", "shortest round-tripping float text");
        const auto decoded = binlab::evalBits("f32 0x3FC00000", 64, false);
        CHECK(decoded.success && decoded.value.floatText == "1.5", "raw float pattern decodes to text");
        const auto breakdown = binlab::evalBits("1.5", 64, false);
        CHECK(breakdown.success && breakdown.value.detail.find("FLOAT 1.5") != std::string::npos
              && breakdown.value.detail.find("IEEE-754 binary64") != std::string::npos,
              "detail includes FLOAT text and S/E/M breakdown");
        CHECK(errorCode(binlab::evalBits("u32 1.5", 32, false)) == "E_FLOAT_TYPE",
              "an integer type prefix cannot take a float literal");
        CHECK(errorCode(binlab::evalBits("1.5 & 0xFF", 32, false)) == "E_FLOAT_OP",
              "no float arithmetic: a float literal stands alone");
        CHECK(bitsOf("1e-6", ok, 64) == 0x3EB0C6F7A0B5ED8DULL && ok,
              "1e-6 is a third spelling of micro");
    }

    // ---- the IEEE-754 container table ----------------------------------------
    {
        CHECK(binlab::floatNameList() == "f16 / f32 / f64", "the table advertises exactly its own rows");
        bool consistent = true;
        for (const binlab::FloatSpec& f : binlab::floatTable()) {
            if (f.bits != f.expBits + f.mantBits + 1)
                consistent = false;
            if (f.bias != (1 << (f.expBits - 1)) - 1)
                consistent = false;
            if (f.digitsMin > f.digitsMax || f.digitsMax < 2)
                consistent = false;
            if (binlab::floatByBits(f.bits) != &f || binlab::floatByName(f.name) != &f)
                consistent = false;
            if (std::string(f.ieee) != std::string("binary") + std::to_string(f.bits))
                consistent = false;
        }
        CHECK(consistent, "every row is self-consistent: layout, bias, name and lookup");
        CHECK(binlab::floatByName("F32") != nullptr, "float prefixes are case-insensitive");
        CHECK(binlab::floatByName("u32") == nullptr, "an integer name is not a float container");
        CHECK(binlab::floatByBits(16) != nullptr && binlab::floatByBits(24) == nullptr,
              "only table widths decode");

        // S/E/M field boundaries come from the table, not from a per-width if.
        const binlab::FloatSpec& h = *binlab::floatByName("f16");
        CHECK(binlab::floatSegment(15, h) == 2 && binlab::floatSegment(14, h) == 1
              && binlab::floatSegment(10, h) == 1 && binlab::floatSegment(9, h) == 0,
              "binary16 splits as S=15 E=14..10 M=9..0");
        const binlab::FloatSpec& s = *binlab::floatByName("f32");
        CHECK(binlab::floatSegment(31, s) == 2 && binlab::floatSegment(30, s) == 1
              && binlab::floatSegment(23, s) == 1 && binlab::floatSegment(22, s) == 0,
              "binary32 splits as S=31 E=30..23 M=22..0");
        const binlab::FloatSpec& d = *binlab::floatByName("f64");
        CHECK(binlab::floatSegment(63, d) == 2 && binlab::floatSegment(62, d) == 1
              && binlab::floatSegment(52, d) == 1 && binlab::floatSegment(51, d) == 0,
              "binary64 splits as S=63 E=62..52 M=51..0");

        CHECK(binlab::floatSpecialLabel(0x7E00, h) == "+NaN"
              && binlab::floatSpecialLabel(0xFC00, h) == "-Infinity"
              && binlab::floatSpecialLabel(0x8000, h) == "-zero"
              && binlab::floatSpecialLabel(0x0001, h) == "subnormal"
              && binlab::floatSpecialLabel(0x3E00, h).empty(),
              "special values are named from their fields");
    }

    // ---- binary16: the generic encoder path ----------------------------------
    {
        bool ok = false;
        CHECK(bitsOf("f16 1.5", ok) == 0x3E00 && ok, "f16 1.5 bit pattern");
        CHECK(bitsOf("f16 0.1", ok) == 0x2E66 && ok, "f16 0.1 bit pattern");
        CHECK(bitsOf("f16 3.0", ok) == 0x4200 && ok, "f16 3.0 bit pattern");
        CHECK(bitsOf("f16 -2.0", ok) == 0xC000 && ok, "f16 -2.0 keeps the sign");
        CHECK(bitsOf("f16 0.5", ok) == 0x3800 && ok, "f16 0.5 bit pattern");
        CHECK(bitsOf("f16 65504.0", ok) == 0x7BFF && ok, "f16 largest finite");
        CHECK(bitsOf("f16 65520.0", ok) == 0x7C00 && ok, "f16 at the tie rounds past the top");
        CHECK(bitsOf("f16 65519.0", ok) == 0x7BFF && ok, "f16 below the tie stays finite");
        CHECK(bitsOf("f16 6.103515625e-5", ok) == 0x0400 && ok, "f16 smallest normal");
        CHECK(bitsOf("f16 5.9604644775390625e-8", ok) == 0x0001 && ok, "f16 smallest subnormal");
        CHECK(bitsOf("f16 1e-6", ok) == 0x0011 && ok, "f16 subnormal 1e-6");
        CHECK(bitsOf("f16 -0.0", ok) == 0x8000 && ok, "f16 negative zero keeps its sign");
        CHECK(bitsOf("f16 Inf", ok) == 0x7C00 && ok, "f16 infinity");
        CHECK(bitsOf("f16 0x3E00", ok) == 0x3E00 && ok, "f16 accepts a raw pattern");

        // A plain decimal under a float prefix is a value, not raw bits: the
        // reading every user expects from "f32 88". Raw bits keep a base prefix
        // or '@'.
        CHECK(bitsOf("f32 88", ok) == 0x42B00000 && ok, "f32 88 encodes 88.0");
        CHECK(bitsOf("f16 65504", ok) == 0x7BFF && ok, "f16 65504 is the largest finite, not 0xFFE0");
        CHECK(bitsOf("f64 1", ok, 64) == 0x3FF0000000000000ULL && ok, "f64 1 encodes 1.0");
        CHECK(bitsOf("f32 -2", ok) == 0xC0000000 && ok, "f32 -2 keeps the sign");
        CHECK(bitsOf("f16 0", ok) == 0x0000 && ok, "f16 0 is +0");
        CHECK(bitsOf("f32 4K", ok) == 0x457A0000 && ok, "a unit applies before encoding: 4K = 4000.0");
        CHECK(bitsOf("f32 @88", ok) == 0x58 && ok, "'@' still spells a raw decimal pattern");
        CHECK(bitsOf("f32 0b1011000", ok) == 0x58 && ok, "a base prefix still spells raw bits");
        CHECK(binlab::evalBits("f16 65536", 32, false).value.floatNote.find("+Inf") != std::string::npos,
              "an integer beyond binary16 reports +Inf instead of E_RANGE");
        CHECK(bitsOf("u32 88", ok) == 88 && ok, "integer types are untouched");
        CHECK(bitsOf("f32 0x3FC00000 | 1", ok) == 0x3FC00001 && ok,
              "bit operations under a float prefix still take decimal masks as raw bits");

        const auto infNote = binlab::evalBits("f16 1e30", 32, false);
        CHECK(infNote.success && infNote.value.floatNote.find("beyond binary16") != std::string::npos
              && infNote.value.floatNote.find("+Inf") != std::string::npos,
              "overflow says so instead of pretending to be a number");
        const auto zeroNote = binlab::evalBits("f16 1e-8", 32, false);
        CHECK(zeroNote.success && zeroNote.value.floatNote.find("smallest binary16") != std::string::npos,
              "underflow to zero says so");
        CHECK(errorCode(binlab::evalBits("f64 1e30", 32, false)) == std::string(),
              "binary64 has no overflow to report here");
        CHECK(binlab::evalBits("f64 1e30", 32, false).value.floatNote.empty(),
              "an in-range value carries no range note");

        // Round-half-to-even at an exact midpoint, both directions.
        const binlab::FloatSpec& h = *binlab::floatByName("f16");
        CHECK(binlab::floatFromDouble(1365.5 / 4096.0, h) == 0x3556,
              "a mantissa tie picks the even field (up)");
        CHECK(binlab::floatFromDouble(1364.5 / 4096.0, h) == 0x3554,
              "a mantissa tie picks the even field (down)");
        CHECK(binlab::floatFromDouble(0.5 / 16777216.0, h) == 0x0000,
              "a subnormal tie picks zero");
        CHECK(binlab::floatFromDouble(1.5 / 16777216.0, h) == 0x0002,
              "a subnormal tie picks the even field");

        // Every stored pattern must survive decode -> encode, and its printed
        // text must encode back to itself: the bijection catches a broken
        // exponent carry, a wrong bias or an off-by-one in the field masks.
        bool patternRt = true;
        bool textRt = true;
        for (long p = 0; p < 0x10000; ++p) {
            const std::uint64_t raw = static_cast<std::uint64_t>(p);
            const auto fields = binlab::floatFields(raw, h);
            if (fields.nan || fields.inf)
                continue;
            if (binlab::floatFromDouble(binlab::floatToDouble(raw, h), h) != raw)
                patternRt = false;
            if (binlab::floatFromDouble(std::strtod(binlab::exactFloatText(raw, h).c_str(), nullptr), h)
                != raw)
                textRt = false;
        }
        CHECK(patternRt, "every finite binary16 pattern round-trips through the encoder");
        CHECK(textRt, "the shortest decimal text always encodes back to its own pattern");
        const auto f16Text = binlab::evalBits("f16 0.1", 32, false);
        CHECK(f16Text.success && f16Text.value.floatText == "0.1" && f16Text.value.typeName == "f16",
              "f16 prints the shortest round-tripping text");
        const auto f16Detail = binlab::evalBits("f16 1.5", 32, false);
        CHECK(f16Detail.success && f16Detail.value.detail.find("IEEE-754 binary16") != std::string::npos,
              "the breakdown names binary16");

        // bytes words read the same table, so f16 is a two-byte word now.
        const auto h16 = binlab::evalBytes("hex f16le 00 3E", "le");
        CHECK(h16.success && !h16.value.words.empty() && h16.value.words[0].primary == "1.5",
              "bytes f16le decodes 1.5");
        const auto h16be = binlab::evalBytes("hex f16be 3E 00", "le");
        CHECK(h16be.success && !h16be.value.words.empty() && h16be.value.words[0].primary == "1.5",
              "bytes f16be decodes 1.5");
        CHECK(binlab::evalBytes("hex f16le 00 3E", "le").value.typeName == "f16le",
              "the word type name keeps the endian suffix");
    }

    // ---- a float name the table lacks gets a real error ---------------------
    {
        static const char* const kBad[] = { "f128", "bf16", "bf32", "f8", "f96", "ld", "quad" };
        bool honest = true;
        for (const char* token : kBad) {
            const std::string expr = std::string(token) + " 1.5";
            const auto r = binlab::evalBits(expr, 32, false);
            if (errorCode(r) != "E_FLOAT_TYPE" || r.failure.hint.find("f16 / f32 / f64") == std::string::npos)
                honest = false;
        }
        CHECK(honest, "unsupported float prefixes name the supported list, not a bogus digit");
        const auto old = binlab::evalBits("f128 1.5", 32, false);
        CHECK(!old.success && old.failure.message.find("f128") != std::string::npos,
              "the error quotes the spelling the user typed");
        const auto bytesBad = binlab::evalBytes("hex f128le 00 00 00 00 00 00 00 00", "le");
        CHECK(!bytesBad.success && bytesBad.failure.code == "E_FLOAT_TYPE",
              "bytes rejects the same spelling honestly");
        CHECK(errorCode(binlab::evalBits("f 1.5", 32, false)) != "E_FLOAT_TYPE",
              "a bare 'f' is a syntax problem, not a float type");
        CHECK(errorCode(binlab::evalBits("fle 1.5", 32, false)) != "E_FLOAT_TYPE",
              "an empty digit run is a typo, not a float type");
        CHECK(errorCode(binlab::evalBits("f9999 1.5", 32, false)) != "E_FLOAT_TYPE",
              "an absurd width is treated as a typo, not a container");
    }

    // ---- record formats: decode, the hextract test vectors -------------------
    {
        const binlab::FormatSet set = binlab::builtinFormatSet();
        CHECK(set.entries.size() == 2 && set.entries[0].ok && set.entries[1].ok, "both built-in formats compile");
        const binlab::RecordFormat* f512 = set.find("blm_interlock_512");
        CHECK(f512 != nullptr && f512->recordBytes() == 64 && f512->msbFirst, "interlock: 64 B, msb-first");
        if (f512 != nullptr) {
            CHECK(f512->columns.size() == 16 && f512->columns[0] == "wr_time_s" && f512->columns[3] == "lock_addr"
                      && f512->columns[4] == "integral0" && f512->columns[9] == "integral5",
                  "interlock columns expand arrays");
            const auto r = binlab::evalRecords("fmt blm_interlock_512 " + toHex(msbFirst(interlockBlob(), 64)), set);
            CHECK(r.success && r.value.records.size() == 3 && r.value.trailing == 0, "interlock: three records");
            if (r.success && r.value.records.size() == 3) {
                const binlab::Record& r0 = r.value.records[0];
                CHECK(cellOf(*f512, r0, "wr_time_s").u == 100 && cellOf(*f512, r0, "wr_time_ns").u == 200,
                      "interlock r0 time");
                CHECK(cellOf(*f512, r0, "data_id").u == 1 && cellOf(*f512, r0, "lock_addr").u == 0xabc,
                      "interlock r0 id / addr");
                CHECK(cellOf(*f512, r0, "lock_addr").text == "0xabc", "radix=hex displays 0xabc");
                bool seq = true;
                for (int i = 0; i < 6; ++i)
                    seq = seq && cellOf(*f512, r0, "integral" + std::to_string(i)).i == i + 1;
                CHECK(seq, "interlock r0 integral = 1..6");
                CHECK(cellOf(*f512, r0, "count0").u == 7 && cellOf(*f512, r0, "count1").u == 0, "interlock r0 count");
                CHECK(cellOf(*f512, r.value.records[1], "integral0").i == -5
                          && cellOf(*f512, r.value.records[1], "integral0").text == "-5",
                      "interlock r1 integral0 = -5");
                CHECK(r.value.records[1].offset == 64, "record offset is the input byte offset");
            }
            // The order word overrides the format default (hextract reverse=).
            const std::vector<std::uint8_t> blob = interlockBlob();
            const std::string lsbText = toHex(std::vector<std::uint8_t>(blob.begin(), blob.begin() + 64));
            const auto asLsb = binlab::evalRecords("fmt blm_interlock_512 lsb " + lsbText, set);
            CHECK(asLsb.success && !asLsb.value.msbFirst && cellOf(*f512, asLsb.value.records[0], "wr_time_s").u == 100,
                  "lsb override reads the stream as is");
            const auto asMsb = binlab::evalRecords("fmt blm_interlock_512 msb " + lsbText, set);
            CHECK(asMsb.success && cellOf(*f512, asMsb.value.records[0], "wr_time_s").u != 100,
                  "msb reverses each record first");
            const auto trailing = binlab::evalRecords(
                "fmt blm_interlock_512 " + toHex(msbFirst(interlockBlob(), 64)) + "deadbeef", set);
            CHECK(trailing.success && trailing.value.records.size() == 3 && trailing.value.trailing == 4,
                  "trailing bytes are counted, not decoded");
            const auto shortRec = binlab::evalRecords("fmt blm_interlock_512 " + std::string(126, '0'), set);
            CHECK(!shortRec.success && shortRec.failure.code == "E_SHORT_RECORD"
                      && shortRec.failure.message.find("need 64 B") != std::string::npos,
                  "63 bytes is not a record");
        }
        const binlab::RecordFormat* f256 = set.find("BLM_RAWDATA_256");
        CHECK(f256 != nullptr && f256->recordBytes() == 32 && !f256->msbFirst, "rawdata: 32 B, lsb-first, case-insensitive");
        if (f256 != nullptr) {
            std::vector<std::uint8_t> blob;
            putLE(blob, 1, 4); putLE(blob, 2, 4);
            for (int v : {1, 2, 3, 4, 5, 6}) putLE(blob, static_cast<std::uint64_t>(v), 2);
            for (int v : {9, 0, 0, 0, 0, 0}) putLE(blob, static_cast<std::uint64_t>(v), 2);
            const auto r = binlab::evalRecords("fmt blm_rawdata_256 " + toHex(blob), set);
            CHECK(r.success && r.value.records.size() == 1, "rawdata: one record");
            if (r.success && !r.value.records.empty()) {
                CHECK(cellOf(*f256, r.value.records[0], "wr_time_ns").u == 2, "rawdata wr_time_ns");
                CHECK(cellOf(*f256, r.value.records[0], "integral5").i == 6, "rawdata integral5");
                CHECK(cellOf(*f256, r.value.records[0], "count0").u == 9, "rawdata count0");
            }
        }

        // Strict hex: real separators pass, anything else is an error with
        // its position instead of being spliced into the data.
        const auto sep = binlab::parseStrictHex("0x01, 02:03 04_05;06\n0X07");
        CHECK(sep.success && sep.value.size() == 7 && sep.value[6] == 7, "separators and 0x prefixes are accepted");
        const auto noisy = binlab::parseStrictHex("ILA 01 02");
        CHECK(!noisy.success && noisy.failure.code == "E_BAD_TOKEN"
                  && noisy.failure.message.find("character 1") != std::string::npos,
              "a non-hex word is rejected with its position");
        CHECK(binlab::parseStrictHex("abc").failure.code == "E_HEX_ODD", "odd hex digit count");
        CHECK(binlab::parseStrictHex(std::string(8194, '0')).failure.code == "E_BYTES_TOO_LARGE", "4096 byte cap");
        CHECK(binlab::evalRecords("fmt nosuch 00", set).failure.code == "E_FORMAT_NAME", "unknown format name");
        CHECK(binlab::evalRecords("fmt", set).failure.code == "E_FORMAT_NAME", "missing format name");
    }

    // ---- record formats: bit slices, scale, radix and validation ------------
    {
        binlab::RawField flag = rawField("flag7", "u8");
        flag.hasBits = true; flag.bitHi = 7; flag.bitLo = 7;
        binlab::RawField nib = rawField("nib", "u16");
        nib.hasBits = true; nib.bitHi = 3; nib.bitLo = 0;
        binlab::RawField mv = rawField("mv", "u8");
        mv.hasScale = true; mv.scale = 2.5;
        const auto bf = compileOne("bf", 32, {flag, nib, mv});
        CHECK(bf.success, "bitfield format compiles");
        if (bf.success) {
            const binlab::RecordsEval e = binlab::decodeRecords(bf.value, false, {0x85, 0xcd, 0xab, 4});
            CHECK(e.records.size() == 1, "one 4-byte record");
            CHECK(cellOf(bf.value, e.records[0], "flag7").u == 1, "bits = 7 takes the top bit of 0x85");
            CHECK(cellOf(bf.value, e.records[0], "nib").u == 0xd, "bits = 3:0 of 0xabcd");
            CHECK(cellOf(bf.value, e.records[0], "mv").u == 4 && cellOf(bf.value, e.records[0], "mv").text == "10",
                  "scale 2.5 displays 10, the raw value stays 4");
        }
        binlab::RawField hexNeg = rawField("h", "i8");
        hexNeg.radix = "hex";
        const auto hn = compileOne("hn", 8, {hexNeg});
        CHECK(hn.success && binlab::decodeRecords(hn.value, false, {0xFF}).records[0].cells[0].text == "-0x1",
              "radix=hex prints negatives as -0x1");
        binlab::RawField h16 = rawField("half", "f16");
        const auto fh = compileOne("fh", 16, {h16});
        CHECK(fh.success && binlab::decodeRecords(fh.value, false, {0x00, 0x3E}).records[0].cells[0].text == "1.5",
              "f16 fields decode through the shared float table");
        const auto pad = compileOne("pad", 32, {rawField("a", "u8")});
        CHECK(pad.success && binlab::decodeRecords(pad.value, false, {1, 2, 3, 4}).records[0].cells[0].u == 1,
              "fields may leave padding at the top of the record");

        CHECK(!compileOne("x", 8, {rawField("a", "u9")}).success, "unknown type");
        CHECK(!compileOne("x", 8, {rawField("a", "u16le")}).success, "fields take no endian suffix");
        CHECK(!compileOne("x", 7, {rawField("a", "u8")}).success, "word_bits must be a multiple of 8");
        CHECK(!compileOne("x", 8, {rawField("a", "u16")}).success, "fields overflow the word");
        CHECK(!compileOne("x", 8, {rawField("a", "u8"), rawField("a", "u8")}).success, "duplicate field name");
        CHECK(!compileOne("x", 8, {rawField("_a", "u8")}).success, "private names are rejected");
        CHECK(!compileOne("x", 8, {rawField("any", "u8")}).success, "reserved names are rejected");
        binlab::RawField wide = rawField("a", "u8");
        wide.hasBits = true; wide.bitHi = 8; wide.bitLo = 0;
        CHECK(!compileOne("x", 16, {wide}).success, "bits beyond the type");
        binlab::RawField fbits = rawField("a", "f32");
        fbits.hasBits = true; fbits.bitHi = 3; fbits.bitLo = 0;
        CHECK(!compileOne("x", 32, {fbits}).success, "bits needs an integer type");
        CHECK(!compileOne("x", 8, {rawField("a", "u8", 0)}).success, "count must be >= 1");
        binlab::RawFormat order;
        order.name = "x"; order.wordBits = 8; order.byteOrder = "raw"; order.fields = {rawField("a", "u8")};
        CHECK(!binlab::compileFormat(order).success, "byte_order 'raw' is rejected");
        CHECK(!compileOne("x", 8192, {rawField("a", "u8")}).success, "record size cap");

        binlab::FormatSet set = binlab::builtinFormatSet();
        binlab::RawFormat user = binlab::builtinRawFormats()[0];
        user.source = "mine.toml";
        user.description = "user copy";
        binlab::addFormat(set, binlab::entryFor(user));
        CHECK(set.entries.size() == 2 && set.entries[0].source == "mine.toml" && set.notes.size() == 1,
              "a user format with a built-in name overrides it and says so");
    }
    // ---- record formats: the bounded rule evaluator ---------------------------
    {
        const binlab::FormatSet set = binlab::builtinFormatSet();
        const binlab::RecordFormat* f512 = set.find("blm_interlock_512");
        const auto r = binlab::evalRecords("fmt blm_interlock_512 " + toHex(msbFirst(interlockBlob(), 64)), set);
        // hextract's GUI smoke test: rule1 (count) / rule0 (negative) / rule2 (all zero)
        CHECK(r.success && r.value.records.size() == 3 && r.value.records[0].rule == 1
                  && r.value.records[1].rule == 0 && r.value.records[2].rule == 2,
              "first matching rule per row matches hextract");
        CHECK(r.success && r.value.ruleHits == 3 && r.value.ruleErrors == 0, "three hits, no errors");
        CHECK(f512 != nullptr && f512->rules[0].name == "Rule 1", "unnamed rules are numbered");

        const auto withRule = [&](const char* when) {
            binlab::RawRule rr;
            rr.when = when;
            rr.bg = "#ff0000";
            return compileOne("r", 32, {rawField("a", "i8"), rawField("arr", "u8", 3)}, {rr});
        };
        const auto row = [&](const binlab::RecordFormat& f, std::vector<std::uint8_t> bytes) {
            return binlab::decodeRecords(f, false, std::move(bytes)).records[0];
        };
        const auto matches = [&](const char* when, std::vector<std::uint8_t> bytes) {
            const auto c = withRule(when);
            if (!c.success)
                return -2;
            return row(c.value, std::move(bytes)).rule;
        };
        CHECK(matches("a < 0", {0xFF, 1, 2, 3}) == 0, "signed field below zero");
        CHECK(matches("0 < a <= 5", {3, 0, 0, 0}) == 0 && matches("0 < a <= 5", {6, 0, 0, 0}) == -1,
              "chained comparison");
        CHECK(matches("len(arr) == 3 and arr[-1] == 3", {0, 1, 2, 3}) == 0, "len and a negative index");
        CHECK(matches("min(arr) == 1 and max(arr) == 3", {0, 1, 2, 3}) == 0, "min / max of an array");
        CHECK(matches("max(a, 7) == 7", {2, 0, 0, 0}) == 0, "max of two numbers");
        CHECK(matches("abs(a) == 2", {0xFE, 0, 0, 0}) == 0, "abs");
        CHECK(matches("any(v > 2 for v in arr)", {0, 1, 2, 3}) == 0, "any with a generator");
        CHECK(matches("all(v & 1 for v in arr)", {0, 1, 3, 5}) == 0, "bit ops inside a generator");
        CHECK(matches("sum_ok == 1" , {0, 0, 0, 0}) == -2, "unknown names are rejected at load");
        CHECK(matches("-7 // 2 == -4 and -7 % 2 == 1 and 7 / 2 == 3.5 and 2 ** 10 == 1024", {0, 0, 0, 0}) == 0,
              "Python integer division, modulo, true division and power");
        CHECK(matches("not a or True", {0, 0, 0, 0}) == 0, "not / or / True");

        const auto err = withRule("arr[5] == 0");
        CHECK(err.success, "an index is only checked at evaluation time");
        if (err.success) {
            const binlab::RecordsEval e = binlab::decodeRecords(err.value, false, {0, 1, 2, 3});
            CHECK(e.records[0].rule == -1 && e.records[0].ruleError.find("out of range") != std::string::npos
                      && e.ruleErrors == 1,
                  "an out-of-range index is a row error, not a crash");
        }
        CHECK(!withRule("__import__('os')").success, "__import__ is rejected");
        CHECK(!withRule("a.bit_length() > 1").success, "attribute access is rejected");
        CHECK(!withRule("_secret > 1").success, "private names are rejected");
        CHECK(!withRule("nosuch < 1").success, "unknown names are rejected");
        CHECK(!withRule("lambda: 1").success, "lambda is rejected");
        CHECK(!withRule("sum(arr) > 1").success, "only whitelisted calls");
        CHECK(!withRule("arr[0:2] == 1").success, "slices are rejected");
        CHECK(!withRule("a == 'x'").success, "strings are rejected");
        CHECK(!withRule("any(v for v in arr if v)").success, "filtered generators are rejected");
        std::string deep;
        for (int i = 0; i < 40; ++i)
            deep += "(";
        deep += "a";
        for (int i = 0; i < 40; ++i)
            deep += ")";
        const auto tooDeep = withRule(deep.c_str());
        CHECK(!tooDeep.success && tooDeep.failure.message.find("deeper") != std::string::npos, "depth cap");
        std::string wide = "a";
        for (int i = 0; i < 200; ++i)
            wide += " + a";
        const auto tooWide = withRule(wide.c_str());
        CHECK(!tooWide.success && tooWide.failure.message.find("nodes") != std::string::npos, "node cap");
        const auto slow = withRule("1 ** 5000 > 0");
        CHECK(slow.success && row(slow.value, {0, 0, 0, 0}).ruleError.find("steps") != std::string::npos,
              "step cap stops a runaway power");
        const auto big = withRule("2 ** 70 > 0");
        CHECK(big.success && row(big.value, {0, 0, 0, 0}).ruleError.find("overflow") != std::string::npos,
              "integer overflow is a row error instead of a silent wrap");

        binlab::RawRule a1;
        a1.when = "a > 0";
        a1.bg = "#ff0000";
        binlab::RawRule a2 = a1;
        a2.bg = "#00ff00";
        const auto dup = compileOne("d", 8, {rawField("a", "u8")}, {a1, a2});
        CHECK(dup.success && dup.value.warnings.size() == 1, "duplicate rule is a warning");
        binlab::RawRule noColor;
        noColor.when = "a > 0";
        CHECK(!compileOne("d", 8, {rawField("a", "u8")}, {noColor}).success, "a rule needs a color");
        binlab::RawRule badColor = a1;
        badColor.bg = "red";
        CHECK(!compileOne("d", 8, {rawField("a", "u8")}, {badColor}).success, "colors must be #rrggbb");
    }
    {
        // ---- record format files (toml++) -------------------------------------
        namespace fs = std::filesystem;
        const fs::path fixtures = fs::u8path(BINLAB_FIXTURES);
        const auto readText = [](const fs::path& p) {
            std::ifstream in(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        for (const binlab::RawFormat& b : binlab::builtinRawFormats()) {
            const std::string file = b.name + ".toml";
            const auto r = binlab::parseFormatToml(readText(fixtures / file), file);
            CHECK(r.success, file + " parses");
            if (!r.success)
                continue;
            const binlab::RawFormat& t = r.value;
            bool same = t.name == b.name && t.description == b.description && t.wordBits == b.wordBits
                        && t.byteOrder == b.byteOrder && t.fields.size() == b.fields.size()
                        && t.rules.size() == b.rules.size();
            for (std::size_t i = 0; same && i < t.fields.size(); ++i) {
                const binlab::RawField& x = t.fields[i];
                const binlab::RawField& y = b.fields[i];
                same = x.name == y.name && x.type == y.type && x.count == y.count && x.hasBits == y.hasBits
                       && x.radix == y.radix && x.hasScale == y.hasScale;
            }
            for (std::size_t i = 0; same && i < t.rules.size(); ++i)
                same = t.rules[i].when == b.rules[i].when && t.rules[i].bg == b.rules[i].bg
                       && t.rules[i].fg == b.rules[i].fg && t.rules[i].name == b.rules[i].name;
            CHECK(same, file + " matches the built-in table field for field");
        }

        const auto parse = [](const std::string& text) { return binlab::parseFormatToml(text, "t.toml"); };
        const auto compiles = [&](const std::string& text) {
            const auto r = parse(text);
            return r.success && binlab::compileFormat(r.value).success;
        };
        const std::string head = "[format]\nname = \"t\"\nword_bits = 16\n";
        CHECK(compiles(head + "[[field]]\nname = \"a\"\ntype = \"u16\"\n"), "minimal format compiles");
        const auto bitsScale = parse(head + "[[field]]\nname = \"a\"\ntype = \"u16\"\nbits = \"11:4\"\nscale = 0.5\nradix = \"hex\"\n");
        CHECK(bitsScale.success && bitsScale.value.fields[0].bitHi == 11 && bitsScale.value.fields[0].bitLo == 4
                  && bitsScale.value.fields[0].hasScale && bitsScale.value.fields[0].scale == 0.5,
              "bits hi:lo, scale and radix are read");
        const auto oneBit = parse(head + "[[field]]\nname = \"a\"\ntype = \"u16\"\nbits = 3\n");
        CHECK(oneBit.success && oneBit.value.fields[0].bitHi == 3 && oneBit.value.fields[0].bitLo == 3,
              "an integer bits value is a single bit");
        CHECK(parse(head + "[[field]]\nname = \"a\"\ntype = \"u16\"\nbits = \"x:1\"\n").failure.message.find("hi:lo")
                  != std::string::npos, "bad bits text is rejected");
        const auto syntax = parse("[format]\nname = \"t\"\nword_bits = 16\n[[field]\n");
        CHECK(!syntax.success && syntax.failure.message.find("t.toml:4") == 0, "TOML syntax error carries its line");
        const auto unknownKey = parse(head + "[[field]]\nname = \"a\"\ntype = \"u16\"\ncolour = 1\n");
        CHECK(!unknownKey.success && unknownKey.failure.message.find("t.toml:7") == 0
                  && unknownKey.failure.message.find("colour") != std::string::npos,
              "unknown key is an error with the line of the key");
        CHECK(!parse("[format]\nname = \"t\"\nword_bits = \"16\"\n").success, "word_bits must be an integer");
        CHECK(!parse("[format]\nword_bits = 16\n").success, "format.name is required");
        CHECK(!parse("name = \"t\"\n").success, "a [format] table is required");
        CHECK(!parse(head + "field = 1\n").success, "field must be an array of tables");
        CHECK(!compiles("[format]\nname = \"t\"\nword_bits = 12\n[[field]]\nname = \"a\"\ntype = \"u8\"\n"),
              "word_bits not a multiple of 8");
        CHECK(!compiles(head + "[[field]]\nname = \"a\"\ntype = \"u32\"\n"), "fields wider than the word");
        CHECK(!compiles(head + "[[field]]\nname = \"a\"\ntype = \"u12\"\n"), "unknown field type");
        CHECK(!compiles(head + "[[field]]\nname = \"a\"\ntype = \"u16\"\nbits = \"16:0\"\n"), "bits beyond the type");
        CHECK(!compiles(head + "[[field]]\nname = \"a\"\ntype = \"u16\"\n[[rule]]\nwhen = \"a > 0\"\n"),
              "a rule without colors");
        const auto dupRule = parse(head + "[[field]]\nname = \"a\"\ntype = \"u16\"\n"
                                   "[[rule]]\nwhen = \"a > 0\"\nbg = \"#ff0000\"\n[[rule]]\nwhen = \"a > 0\"\nfg = \"#00ff00\"\n");
        CHECK(dupRule.success && binlab::compileFormat(dupRule.value).success
                  && binlab::compileFormat(dupRule.value).value.warnings.size() == 1,
              "a duplicate rule only warns");

        // Directory loading: the fixtures override both built-ins by name.
        const binlab::FormatSet fromFixtures = binlab::loadFormatDir(fixtures.wstring());
        CHECK(fromFixtures.entries.size() == 2 && fromFixtures.notes.size() == 2
                  && fromFixtures.find("blm_rawdata_256") != nullptr
                  && fromFixtures.find("blm_rawdata_256")->source == "blm_rawdata_256.toml",
              "fixture files override the built-in formats");
        CHECK(binlab::loadFormatDir(L"").entries.size() == 2, "no folder: built-ins only");
        const binlab::FormatSet missing = binlab::loadFormatDir(L"Z:\\binarylab\\no\\such\\dir");
        CHECK(missing.entries.size() == 2 && missing.notes.size() == 1, "missing folder is a note");

        std::error_code ec;
        const fs::path tmp = fs::temp_directory_path(ec) / "binarylab_format_test";
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        const auto put = [&](const char* name, const std::string& text) {
            std::ofstream out(tmp / name, std::ios::binary);
            out << text;
        };
        put("a_good.toml", head + "[[field]]\nname = \"a\"\ntype = \"u16\"\n");
        put("b_broken.toml", "[format\n");
        put("c_huge.toml", std::string(70 * 1024, '#'));
        put("notes.txt", "ignored");
        const binlab::FormatSet mixed = binlab::loadFormatDir(tmp.wstring());
        CHECK(mixed.entries.size() == 5, "built-ins + three toml files, other extensions ignored");
        const binlab::FormatEntry* broken = mixed.entry("b_broken");
        CHECK(broken != nullptr && !broken->ok && broken->error.find("b_broken.toml:1") == 0,
              "a broken file is listed with its error");
        const binlab::FormatEntry* huge = mixed.entry("c_huge");
        CHECK(huge != nullptr && !huge->ok && huge->error.find("64 KiB") != std::string::npos, "file size cap");
        CHECK(mixed.find("t") != nullptr, "a good file next to broken ones still loads");
        const auto decoded = binlab::evalRecords("fmt t lsb 34 12", mixed);
        CHECK(decoded.success && decoded.value.records.size() == 1 && decoded.value.records[0].cells[0].u == 0x1234,
              "a user format decodes");
        CHECK(!binlab::evalRecords("fmt b_broken 34 12", mixed).success, "a broken format cannot decode");
        fs::remove_all(tmp, ec);
    }
    std::cout << "binarylab logic tests: " << (g_total - g_failed) << "/" << g_total << " passed\n";
    if (g_failed)
        std::cerr << g_failed << " test(s) failed\n";
    return g_failed == 0 ? 0 : 1;
}
