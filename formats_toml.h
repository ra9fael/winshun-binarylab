// Record format files: hextract-compatible TOML read with toml++.
//
// parseFormatToml() turns one file's text into a RawFormat (compileFormat()
// then validates it). loadFormatDir() is the only file access in the plugin:
// it reads *.toml from the folder the user picked in settings, read-only and
// bounded (64 files, 64 KiB each). Data to decode never comes from files.
//
// toml++ is built with TOML_EXCEPTIONS 0, so parse errors come back as values,
// matching the no-exceptions contract of logic.h.
#pragma once

#include "formats.h"

#ifndef TOML_EXCEPTIONS
#define TOML_EXCEPTIONS 0
#endif
#ifndef TOML_ENABLE_FORMATTERS
#define TOML_ENABLE_FORMATTERS 0
#endif
#include <toml.hpp>  // third_party/tomlplusplus, a SYSTEM include dir (no warnings)

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace binlab {

constexpr std::size_t kMaxFormatFiles = 64;
constexpr std::uintmax_t kMaxFormatFileBytes = 64 * 1024;

namespace tomlfmt {

struct Reader {
    std::string source;
    std::string error;

    bool bad(const toml::node* at, const std::string& what)
    {
        if (error.empty()) {
            error = source;
            if (at != nullptr && at->source().begin.line > 0)
                error += ":" + std::to_string(at->source().begin.line);
            error += ": " + what;
        }
        return false;
    }

    bool onlyKeys(const toml::table& t, const std::string& where, std::initializer_list<const char*> allowed)
    {
        for (auto&& [key, node] : t) {
            const std::string k(key.str());
            if (std::none_of(allowed.begin(), allowed.end(), [&](const char* a) { return k == a; })) {
                std::string list;
                for (const char* a : allowed)
                    list += (list.empty() ? "" : ", ") + std::string(a);
                return bad(&node, "unknown key '" + k + "' in " + where + " (allowed: " + list + ")");
            }
        }
        return true;
    }

    bool str(const toml::table& t, const char* key, const std::string& where, std::string& out, bool required)
    {
        const toml::node* n = t.get(key);
        if (n == nullptr) {
            if (required)
                return bad(&t, where + "." + key + " missing");
            return true;
        }
        const auto* s = n->as_string();
        if (s == nullptr)
            return bad(n, where + "." + key + " must be a string");
        out = s->get();
        if (required && out.empty())
            return bad(n, where + "." + key + " must not be empty");
        return true;
    }

    bool integer(const toml::table& t, const char* key, const std::string& where, int& out, bool required,
                 long long lo, long long hi)
    {
        const toml::node* n = t.get(key);
        if (n == nullptr) {
            if (required)
                return bad(&t, where + "." + key + " missing");
            return true;
        }
        const auto* v = n->as_integer();
        if (v == nullptr)
            return bad(n, where + "." + key + " must be an integer");
        if (v->get() < lo || v->get() > hi)
            return bad(n, where + "." + key + " must be " + std::to_string(lo) + ".." + std::to_string(hi));
        out = static_cast<int>(v->get());
        return true;
    }

    const toml::array* tables(const toml::table& root, const char* key, bool& ok)
    {
        const toml::node* n = root.get(key);
        if (n == nullptr)
            return nullptr;
        const auto* a = n->as_array();
        if (a == nullptr || !a->is_array_of_tables()) {
            ok = bad(n, std::string(key) + " must be written as [[" + key + "]] tables");
            return nullptr;
        }
        return a;
    }

    bool bits(const toml::node& n, const std::string& where, RawField& f)
    {
        f.hasBits = true;
        if (const auto* v = n.as_integer()) {
            if (v->get() < 0 || v->get() > 63)
                return bad(&n, where + ".bits out of range");
            f.bitHi = f.bitLo = static_cast<int>(v->get());
            return true;
        }
        if (const auto* s = n.as_string()) {
            const std::string text = s->get();
            const std::size_t colon = text.find(':');
            const auto number = [](const std::string& p, int& out) {
                const std::string t = trim(p);
                if (t.empty() || t.size() > 2 || !std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    return false;
                out = std::stoi(t);
                return true;
            };
            if (colon != std::string::npos && number(text.substr(0, colon), f.bitHi)
                && number(text.substr(colon + 1), f.bitLo))
                return true;
            return bad(&n, where + ".bits '" + text + "' is not 'hi:lo'");
        }
        return bad(&n, where + ".bits must be an int or 'hi:lo' string");
    }
};

}  // namespace tomlfmt

// One TOML file -> RawFormat. Errors read "<source>:<line>: <what>".
inline Outcome<RawFormat> parseFormatToml(std::string_view text, const std::string& source)
{
    const std::string hint = "see README: record formats";
    tomlfmt::Reader rd;
    rd.source = source;
    const auto failed = [&]() { return failWith<RawFormat>(fail("E_FORMAT", rd.error, hint)); };

    toml::parse_result parsed = toml::parse(text, source);
    if (!parsed) {
        const toml::parse_error& e = parsed.error();
        return failWith<RawFormat>(fail("E_FORMAT", source + ":" + std::to_string(e.source().begin.line) + ": "
                                                        + std::string(e.description()), hint));
    }
    const toml::table& root = parsed.table();
    if (!rd.onlyKeys(root, "the file", {"format", "field", "rule"}))
        return failed();

    RawFormat raw;
    raw.source = source;
    const toml::table* meta = root.get_as<toml::table>("format");
    if (meta == nullptr) {
        rd.bad(root.get("format"), "missing [format] table");
        return failed();
    }
    if (!rd.onlyKeys(*meta, "[format]", {"name", "description", "word_bits", "byte_order"})
        || !rd.str(*meta, "name", "format", raw.name, true)
        || !rd.str(*meta, "description", "format", raw.description, false)
        || !rd.integer(*meta, "word_bits", "format", raw.wordBits, true, 1, kMaxRecordBits)
        || !rd.str(*meta, "byte_order", "format", raw.byteOrder, false))
        return failed();

    bool ok = true;
    const toml::array* fields = rd.tables(root, "field", ok);
    if (!ok)
        return failed();
    if (fields != nullptr) {
        int index = 0;
        for (const toml::node& node : *fields) {
            const toml::table& t = *node.as_table();
            const std::string where = "field " + std::to_string(++index);
            RawField f;
            if (!rd.onlyKeys(t, "[[field]]", {"name", "type", "count", "bits", "radix", "scale"})
                || !rd.str(t, "name", where, f.name, true) || !rd.str(t, "type", where, f.type, true)
                || !rd.integer(t, "count", where, f.count, false, 1, kMaxFieldCount)
                || !rd.str(t, "radix", where, f.radix, false))
                return failed();
            if (const toml::node* b = t.get("bits"); b != nullptr && !rd.bits(*b, where, f))
                return failed();
            if (const toml::node* s = t.get("scale")) {
                if (const auto* i = s->as_integer())
                    f.scale = static_cast<double>(i->get());
                else if (const auto* d = s->as_floating_point())
                    f.scale = d->get();
                else {
                    rd.bad(s, where + ".scale must be a number");
                    return failed();
                }
                f.hasScale = true;
            }
            raw.fields.push_back(std::move(f));
            if (raw.fields.size() > kMaxFields) {
                rd.bad(&node, "more than " + std::to_string(kMaxFields) + " fields");
                return failed();
            }
        }
    }

    const toml::array* rules = rd.tables(root, "rule", ok);
    if (!ok)
        return failed();
    if (rules != nullptr) {
        int index = 0;
        for (const toml::node& node : *rules) {
            const toml::table& t = *node.as_table();
            const std::string where = "rule " + std::to_string(++index);
            RawRule r;
            if (!rd.onlyKeys(t, "[[rule]]", {"name", "when", "bg", "fg"}) || !rd.str(t, "when", where, r.when, true)
                || !rd.str(t, "bg", where, r.bg, false) || !rd.str(t, "fg", where, r.fg, false))
                return failed();
            if (t.get("name") != nullptr && !rd.str(t, "name", where, r.name, true))
                return failed();
            raw.rules.push_back(std::move(r));
            if (raw.rules.size() > kMaxRules) {
                rd.bad(&node, "more than " + std::to_string(kMaxRules) + " rules");
                return failed();
            }
        }
    }
    return Outcome<RawFormat>::good(std::move(raw));
}

// One file's text -> a list entry. A file that fails still gets an entry named
// after the file, so the selector can show it in red with its error.
inline FormatEntry formatEntryFromToml(std::string_view text, const std::string& source, const std::string& stem)
{
    const auto raw = parseFormatToml(text, source);
    if (!raw.success) {
        FormatEntry e;
        e.name = stem;
        e.source = source;
        e.error = raw.failure.message;
        return e;
    }
    FormatEntry e = entryFor(raw.value);
    if (!e.ok)
        e.error = source + ": " + e.error;
    return e;
}

// Built-ins first, then every *.toml in `dir` in file-name order. A user
// format with a built-in's name replaces it (with a note); a broken file is
// listed but never replaces a working format.
inline FormatSet loadFormatDir(const std::wstring& dir)
{
    namespace fs = std::filesystem;
    FormatSet set = builtinFormatSet();
    if (dir.empty())
        return set;
    const fs::path root(dir);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        set.notes.push_back("format folder not found: " + root.u8string());
        return set;
    }
    std::vector<fs::path> files;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc))
            continue;
        std::string ext = it->path().extension().u8string();
        if (lower(ext) == ".toml")
            files.push_back(it->path());
    }
    if (ec)
        set.notes.push_back("format folder could not be listed: " + ec.message());
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        return lower(a.filename().u8string()) < lower(b.filename().u8string());
    });
    if (files.size() > kMaxFormatFiles) {
        set.notes.push_back(std::to_string(files.size()) + " format files found; only the first "
                            + std::to_string(kMaxFormatFiles) + " are loaded");
        files.resize(kMaxFormatFiles);
    }
    for (const fs::path& p : files) {
        const std::string source = p.filename().u8string();
        const std::string stem = p.stem().u8string();
        FormatEntry e;
        std::error_code sizeEc;
        const std::uintmax_t size = fs::file_size(p, sizeEc);
        if (sizeEc || size > kMaxFormatFileBytes) {
            e.name = stem;
            e.source = source;
            e.error = source + (sizeEc ? ": cannot read file size" : ": larger than 64 KiB");
        } else {
            std::ifstream in(p, std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            if (!in && !in.eof()) {
                e.name = stem;
                e.source = source;
                e.error = source + ": cannot be read";
            } else {
                e = formatEntryFromToml(text.str(), source, stem);
            }
        }
        if (e.ok)
            addFormat(set, std::move(e));
        else
            set.entries.push_back(std::move(e));
    }
    return set;
}

}  // namespace binlab
