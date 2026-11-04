// test_logic.cpp - self-contained tests for logic.h (no plugin host needed).
//
// Build: cmake -B build && cmake --build build && ctest --test-dir build
// The layout tests compare against real compiler output produced by
// #pragma pack, sizeof, alignof and offsetof in this translation unit.
#include "logic.h"

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
    CHECK(errorCode(binlab::evalBits("i8 -129", 32, false)) == "E_RANGE", "bits signed negative overflow");
    CHECK(errorCode(binlab::evalBits("i8 128", 32, false)) == "E_RANGE", "bits signed positive overflow");
    CHECK(errorCode(binlab::evalBits("u8 -1", 32, false)) == "E_NEGATIVE_UNSIGNED", "bits negative unsigned");
    CHECK(errorCode(binlab::evalBits("u8 1 << 8", 32, false)) == "E_SHIFT", "bits shift >= width");
    CHECK(errorCode(binlab::evalBits("u8 1 >> 8", 32, false)) == "E_SHIFT", "bits right shift >= width");
    CHECK(errorCode(binlab::evalBits("u8 1 + 2", 32, false)) == "E_UNKNOWN_OP", "bits unsupported operator");
    CHECK(errorCode(binlab::evalBits("u8 1 2 3 4", 32, false)) == "E_SYNTAX", "bits wrong arity");
    CHECK(errorCode(binlab::evalBits("u8 get 0xA5 8:0", 32, false)) == "E_BAD_RANGE", "bits field range too wide");
    CHECK(errorCode(binlab::evalBits("u8 get 0xA5 4:7", 32, false)) == "E_BAD_RANGE", "bits inverted field range");
    CHECK(errorCode(binlab::evalBits("u8 set 0xA5 7:4 0x10", 32, false)) == "E_RANGE", "bits field value too wide");
    CHECK(errorCode(binlab::evalBits(std::string(17000, 'A'), 32, false)) == "E_INPUT_TOO_LARGE",
          "bits input size cap");

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

    std::cout << "binarylab logic tests: " << (g_total - g_failed) << "/" << g_total << " passed\n";
    if (g_failed)
        std::cerr << g_failed << " test(s) failed\n";
    return g_failed == 0 ? 0 : 1;
}
