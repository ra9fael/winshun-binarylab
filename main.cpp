// main.cpp - WinShun resident plugin: Binary Lab.
//
// The SDK owns stdio and the JSON-RPC framing. This file only maps queries to
// pure functions in logic.h, builds result rows, and answers actions. Queries
// never write files, launch programs, touch the clipboard, or open a window;
// copy and setQuery effects are returned only when the user triggers an action.
//
// Code and comments are English; the user-visible strings are localized with
// the host's locale (Chinese by default, English when locale starts with "en").
#include "winshun.h"

#include "editor.h"
#include "logic.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Settings {
    int defaultWidth = 32;
    std::string defaultEndian = "le";
    int defaultPack = 0;
    bool editorOnEnter = false;
};

Settings readSettings(const winshun::Json& s)
{
    Settings st;
    const auto text = [&](const char* key, const char* fallback) -> std::string {
        const winshun::Json& v = s[key];
        if (v.isString())
            return v.toString();
        if (v.isNumber())
            return std::to_string(v.toInt());
        return fallback;
    };
    const std::string width = text("defaultWidth", "32");
    if (width == "8" || width == "16" || width == "32" || width == "64")
        st.defaultWidth = std::stoi(width);
    st.defaultEndian = binlab::lower(text("defaultEndian", "le")) == "be" ? "be" : "le";
    const std::string pack = text("defaultPack", "0");
    if (pack == "1" || pack == "2" || pack == "4" || pack == "8" || pack == "16")
        st.defaultPack = std::stoi(pack);
    st.editorOnEnter = binlab::lower(text("bitsEnter", "copy")) == "editor";
    return st;
}

std::string tr(bool zh, const char* chinese, const char* english)
{
    return zh ? chinese : english;
}

// Keyboard mapping. The host binds the displayed keys in array order:
//   [0] Enter, [1] Ctrl+Enter, [2] Shift+Enter, and a 4th action with an empty
//   key string is shown only through the host's action menu (no hotkey).
//
// For bits rows the editor is available, so `allowEditor` is true and copy and
// openEditor share Enter / Ctrl+Enter as chosen by `editorFirst`:
//   editorFirst=false (default, bitsEnter=copy): Enter=copy, Ctrl+Enter=editor
//   editorFirst=true  (bitsEnter=editor):        Enter=editor, Ctrl+Enter=copy
// chainQuery stays on Shift+Enter; copyDetail is the unbound menu action.
// For bytes/struct rows there is no editor, so `allowEditor` is false and the
// classic copy / copyDetail / chain mapping is used.
std::vector<winshun::Action> rowActions(bool zh, bool expandOnEnter, bool editorFirst, bool allowEditor)
{
    const std::string openTitle = tr(zh, "打开交互编辑器", "Open interactive editor");
    const std::string copyTitle = expandOnEnter ? tr(zh, "展开成员", "Expand members")
                                                : tr(zh, "复制主值", "Copy value");
    const std::string detailTitle = tr(zh, "复制完整明细", "Copy full details");
    const std::string chainTitle = tr(zh, "改写为下一步查询", "Chain follow-up query");
    std::vector<winshun::Action> a;
    if (allowEditor && editorFirst) {
        a.push_back({"openEditor", openTitle, "Enter"});
        a.push_back({"copyPrimary", copyTitle, "Ctrl+Enter"});
        a.push_back({"chainQuery", chainTitle, "Shift+Enter"});
        a.push_back({"copyDetail", detailTitle, ""});
    } else if (allowEditor) {
        a.push_back({"copyPrimary", copyTitle, "Enter"});
        a.push_back({"openEditor", openTitle, "Ctrl+Enter"});
        a.push_back({"chainQuery", chainTitle, "Shift+Enter"});
        a.push_back({"copyDetail", detailTitle, ""});
    } else {
        a.push_back({"copyPrimary", copyTitle, "Enter"});
        a.push_back({"copyDetail", detailTitle, "Ctrl+Enter"});
        a.push_back({"chainQuery", chainTitle, "Shift+Enter"});
    }
    return a;
}

winshun::Item makeRow(const std::string& id,
                      const std::string& title,
                      const std::string& subtitle,
                      const std::string& primary,
                      const std::string& detail,
                      const std::string& chain,
                      const std::string& preview,
                      const std::string& icon,
                      int score,
                      bool zh,
                      bool expandOnEnter,
                      const std::string& kind,
                      const winshun::Json& members = winshun::Json(),
                      bool editorFirst = false,
                      const winshun::Json& extra = winshun::Json(),
                      bool allowEditor = false)
{
    winshun::Item item(id, title, subtitle);
    item.withCopy(primary)
        .withPreview(preview)
        .withIcon(icon)
        .withScore(score)
        .withActions(rowActions(zh, expandOnEnter, editorFirst, allowEditor));
    winshun::Json::Object data {
        {"kind", kind},
        {"primary", primary},
        {"detail", detail},
        {"chain", chain},
        {"expand", expandOnEnter},
    };
    if (members.isArray())
        data["members"] = members;
    for (const auto& kv : extra.toObject())
        data[kv.first] = kv.second;
    item.withData(winshun::Json(std::move(data)));
    return item;
}

winshun::Item makeError(const binlab::Failure& f, bool zh)
{
    const std::string hint = f.hint;
    const std::string title = tr(zh, "输入有误 · ", "Invalid input · ") + f.code;
    const std::string subtitle = tr(zh, "示例：", "Example: ") + hint + " · " + f.message;
    const std::string detail = title + "\n" + subtitle + "\n"
                               + tr(zh, "查询阶段不会修改任何文件或剪贴板；请按示例改写后重试。",
                                    "Queries never modify files or the clipboard; rewrite the input and retry.");
    winshun::Item item("error." + f.code, title, subtitle);
    item.invalid()
        .withIcon("color:#C0392B")
        .withScore(0)
        .withCopy(hint)
        .withAutocomplete(hint)
        .withPreview(detail)
        .withData(winshun::Json(winshun::Json::Object {
            {"kind", "error"},
            {"primary", hint},
            {"detail", detail},
            {"chain", hint},
            {"expand", false},
        }));
    return item;
}

const char* kSyntaxHelp =
    "bits  [u8|u16|u32|u64|i8|i16|i32|i64] VALUE [OP VALUE]\n"
    "      OP: & | ^ << >> rol ror   unary: ~   raw pattern: @0xFF\n"
    "      fields: get|set|clear|toggle VALUE HIGH:LOW [VALUE]\n"
    "bytes [hex|bin|oct] [u8|i8|u16le|u16be|u32le|u32be|u64le|u64be|f32le|f32be|f64le|f64be] DATA...\n"
    "      hex tokens: even number of digits; bin tokens: 8 bits; oct tokens: 000..377\n"
    "struct [pack=0|1|2|4|8|16] struct Name { u8 a; u32 b; u16 c[3]; };\n"
    "      also union, arrays, inline nested types, and named type references\n";

winshun::Items helpItems(const std::string& command, bool zh)
{
    struct Example {
        const char* id;
        const char* keyword;
        const char* text;
        const char* chineseSubtitle;
        const char* englishSubtitle;
    };
    static const Example examples[] = {
        {"help.bits", "bits", "bits u8 0xA5 & 0x0F", "位运算与进制转换；悬停看位网格", "Bit operations and base conversion; hover for the bit grid"},
        {"help.bytes", "bytes", "bytes hex u16le 34 12 78 56", "字节流解码、hexdump、浮点位拆解", "Byte stream decoding, hexdump, float breakdown"},
        {"help.struct", "struct", "struct pack=4 struct Frame { u8 sof; u32 seq; u16 len; u8 data[3]; };",
         "结构体空间排布、padding、static_assert", "Struct layout, padding and static_assert"},
    };
    winshun::Items items;
    for (int pass = 0; pass < 2; ++pass) {
        for (const Example& ex : examples) {
            const bool want = command.empty() || command == ex.keyword;
            if ((pass == 0) != want)
                continue;
            const std::string full = ex.text;
            winshun::Item item(ex.id, full, tr(zh, ex.chineseSubtitle, ex.englishSubtitle));
            const std::string detail = std::string(full) + "\n\n" + kSyntaxHelp;
            item.invalid()
                .withIcon("color:#185FA5")
                .withScore(90)
                .withCopy(full)
                .withAutocomplete(full)
                .withPreview(detail)
                .withData(winshun::Json(winshun::Json::Object {
                    {"kind", "help"}, {"primary", full}, {"detail", detail},
                    {"chain", full}, {"expand", false},
                }));
            items.push_back(std::move(item));
            if (items.size() >= 3)
                return items;
        }
    }
    return items;
}

winshun::Items bitsItems(const binlab::BitsEval& e, bool zh, bool editorOnEnter)
{
    const binlab::Repr r = binlab::represent(e.result, e.width, e.signedType);
    const std::string context = e.typeName + " · " + e.expr;
    const std::string icon = "color:#185FA5";
    // The editor opens from these fields; the action must not re-parse the text.
    // hexUpper without a prefix would lose the pattern for width < 64, so store
    // the raw 64-bit pattern and mask in the window instead.
    const winshun::Json extra(winshun::Json::Object {
        {"valueHex", binlab::hexUpper(e.result, 64)},
        {"width", e.width},
    });
    winshun::Items items;
    items.push_back(makeRow("bits.result.hex", r.hex + tr(zh, " · 十六进制", " · HEX"), context,
                            r.hex, e.detail, e.chain, e.preview, icon, 100, zh, false, "bits",
                            winshun::Json(), editorOnEnter, extra, true));
    items.push_back(makeRow("bits.result.bin", r.bin + tr(zh, " · 二进制", " · BIN"),
                            e.typeName + " · " + std::to_string(e.width) + " bit · " + binlab::binGrouped(e.result, e.width),
                            r.bin, e.detail, e.chain, e.preview, icon, 98, zh, false, "bits",
                            winshun::Json(), editorOnEnter, extra, true));
    items.push_back(makeRow("bits.result.oct", r.oct + tr(zh, " · 八进制", " · OCT"), context,
                            r.oct, e.detail, e.chain, e.preview, icon, 96, zh, false, "bits",
                            winshun::Json(), editorOnEnter, extra, true));
    items.push_back(makeRow("bits.result.dec", r.dec + tr(zh, " · 十进制", " · DEC"),
                            e.signedType ? e.typeName + " · signed" : e.typeName + " · unsigned",
                            r.dec, e.detail, e.chain, e.preview, icon, 95, zh, false, "bits",
                            winshun::Json(), editorOnEnter, extra, true));
    if (e.signedType)
        items.push_back(makeRow("bits.result.unsigned", r.unsignedDec + tr(zh, " · 无符号", " · unsigned"),
                                std::string("@") + r.hex + tr(zh, " 按位模式重解释", " reinterpreted as a bit pattern"),
                                r.unsignedDec, e.detail, e.chain, e.preview, icon, 94, zh, false, "bits",
                                winshun::Json(), editorOnEnter, extra, true));
    items.push_back(makeRow("bits.result.grid", tr(zh, "位网格与计算说明", "Bit grid and operation notes"),
                            tr(zh, "Ctrl+Enter 复制完整文本；Shift+Enter 把结果当字节流", "Ctrl+Enter copies the text; Shift+Enter chains into bytes"),
                            e.preview, e.detail, e.chain, e.preview, "color:#7C3AED", 90, zh, false, "bits",
                            winshun::Json(), editorOnEnter, extra, true));
    return items;
}

winshun::Items bytesItems(const binlab::BytesEval& e, bool zh)
{
    const std::string icon = "color:#0F766E";
    const std::string streamTitle = std::to_string(e.data.size()) + tr(zh, " 字节 · ", " bytes · ")
                                    + (e.normalized.size() > 40 ? e.normalized.substr(0, 40) + "..." : e.normalized);
    const std::string streamSubtitle = e.format + " → " + e.typeName + " · " + std::to_string(e.words.size())
                                       + tr(zh, " 个完整字 · 末尾 ", " words · trailing ") + std::to_string(e.trailing)
                                       + tr(zh, " 字节", " bytes");
    winshun::Items items;
    items.push_back(makeRow("bytes.stream", streamTitle, streamSubtitle,
                            e.normalized, e.detail, e.chain, e.preview, icon, 100, zh, false, "bytes"));

    for (std::size_t i = 0; i < e.words.size() && i < binlab::kMaxWordRows; ++i) {
        const binlab::ByteWord& w = e.words[i];
        const std::string id = "bytes.word." + std::to_string(w.offset);
        const std::string title = w.primary + " · " + w.typeName;
        const std::string subtitle = "@" + std::to_string(w.offset) + tr(zh, " · 悬停看位/浮点拆解", " · hover for the bit or float breakdown");
        items.push_back(makeRow(id, title, subtitle, w.primary, w.detail, w.chain,
                                e.preview + "\n" + w.detail, "color:#7C3AED",
                                static_cast<int>(90 - i), zh, false, "bytes"));
    }
    if (e.words.size() > binlab::kMaxWordRows) {
        const std::size_t more = e.words.size() - binlab::kMaxWordRows;
        winshun::Item item("bytes.more",
                           tr(zh, "另有 ", "") + std::to_string(more) + tr(zh, " 个字未逐行列出", " more words (see full details)"),
                           tr(zh, "Ctrl+Enter 可复制全部解码结果", "Ctrl+Enter copies every decoded word"));
        item.invalid()
            .withIcon("color:#B45309")
            .withScore(60)
            .withPreview(e.preview)
            .withData(winshun::Json(winshun::Json::Object {
                {"kind", "bytes"}, {"primary", e.normalized}, {"detail", e.detail},
                {"chain", e.chain}, {"expand", false},
            }));
        items.push_back(std::move(item));
    }
    items.push_back(makeRow("bytes.dump", tr(zh, "Hexdump（offset · hex · ASCII）", "Hexdump (offset · hex · ASCII)"),
                            std::to_string(e.data.size()) + tr(zh, " 字节", " bytes"),
                            e.dump, e.dump, e.chain, e.preview, icon, 80, zh, false, "bytes"));
    if (e.trailing) {
        winshun::Item item("bytes.trailing",
                           tr(zh, "末尾 ", "") + std::to_string(e.trailing) + tr(zh, " 字节未解码", " trailing bytes not decoded"),
                           e.typeName + tr(zh, " 需要完整的一组字节；不返回伪造结果", " needs a complete group; no fabricated value"));
        item.invalid()
            .withIcon("color:#C0392B")
            .withScore(50)
            .withCopy(e.normalized)
            .withPreview(e.detail)
            .withData(winshun::Json(winshun::Json::Object {
                {"kind", "bytes"}, {"primary", e.normalized}, {"detail", e.detail},
                {"chain", e.chain}, {"expand", false},
            }));
        items.push_back(std::move(item));
    }
    return items;
}

winshun::Json::Array membersJson(const binlab::Layout& l, bool zh)
{
    winshun::Json::Array out;
    for (std::size_t i = 0; i < l.members.size(); ++i) {
        const binlab::MemberRow& m = l.members[i];
        const bool isMember = m.kind == "member";
        const std::string owner = l.name.empty() ? "Anonymous" : l.name;
        const std::string range = binlab::rangeText(m.offset, m.size);
        const std::string label = isMember ? m.name : tr(zh, "填充", "padding");
        const std::string title = range + "  " + label;
        const std::string subtitle = (m.typeName.empty() ? "-" : m.typeName) + " · " + std::to_string(m.size)
                                     + tr(zh, " 字节", " B") + (isMember ? "" : tr(zh, " · 自动补齐", " · inserted by alignment"));
        const std::string primary = isMember
            ? "offsetof(" + owner + ", " + m.name + ") == " + std::to_string(m.offset)
            : std::string("padding @") + std::to_string(m.offset) + "  " + std::to_string(m.size) + " B";
        const std::string detail = primary + "\n" + subtitle + "\n" + l.text;
        out.push_back(winshun::Json(winshun::Json::Object {
            {"id", "struct.member." + std::to_string(i)},
            {"title", title},
            {"subtitle", subtitle},
            {"primary", primary},
            {"detail", detail},
            {"chain", l.chain},
            {"preview", l.text},
            {"kind", m.kind},
            {"range", range},
            {"valid", isMember},
        }));
    }
    return out;
}

winshun::Items structItems(const binlab::Layout& l, bool zh)
{
    const winshun::Json members(winshun::Json::Array(membersJson(l, zh)));
    const std::string name = l.name.empty() ? "Anonymous" : l.name;
    const std::string packText = l.pack == 0 ? tr(zh, "自然对齐", "natural")
                                             : std::string("pack=") + std::to_string(l.pack);
    const std::string summary = name + " · sizeof " + std::to_string(l.size) + " B · align "
                                + std::to_string(l.align) + " B";
    const std::string subtitle = packText + " · " + std::to_string(l.members.size())
                                 + tr(zh, " 行（含 padding）· Enter 展开成员", " rows (incl. padding) · Enter expands");
    const std::string primary = "sizeof(" + name + ") = " + std::to_string(l.size)
                                + "  alignof(" + name + ") = " + std::to_string(l.align);
    winshun::Items items;
    items.push_back(makeRow("struct.summary", summary, subtitle, primary, l.text, l.chain,
                            l.text, "color:#B45309", 100, zh, true, "struct", members));
    items.push_back(makeRow("struct.map", tr(zh, "字节排布图", "Byte map"),
                            tr(zh, "每字符一个字节；大写字母代表成员，. 代表 padding", "One char per byte; letters are members, . is padding"),
                            l.byteMap, l.text, l.chain, l.text, "color:#B45309", 96, zh, true, "struct", members));
    items.push_back(makeRow("struct.members", tr(zh, "成员表（offset / size）", "Member table (offset / size)"),
                            tr(zh, "Ctrl+Enter 复制完整排布文本", "Ctrl+Enter copies the full layout text"),
                            l.memberTable, l.text, l.chain, l.text, "color:#185FA5", 94, zh, true, "struct", members));
    items.push_back(makeRow("struct.assert", tr(zh, "static_assert 文本", "static_assert text"),
                            tr(zh, "可直接粘到 C/C++ 头文件里做 ABI 校验", "Paste into a C/C++ header to verify the ABI"),
                            l.assertions, l.text, l.chain, l.text, "color:#0F766E", 92, zh, false, "struct", members));
    return items;
}

winshun::Items memberItems(const winshun::Json& data, bool zh)
{
    winshun::Items items;
    for (const winshun::Json& m : data["members"].toArray()) {
        winshun::Item item(m["id"].toString(), m["title"].toString(), m["subtitle"].toString());
        const std::string primary = m["primary"].toString();
        const std::string detail = m["detail"].toString();
        item.withCopy(primary)
            .withPreview(m["preview"].toString())
            .withIcon(m["valid"].toBool(true) ? "color:#185FA5" : "color:#B45309")
            .withScore(80)
            .withActions(rowActions(zh, false, false, false));
        if (!m["valid"].toBool(true))
            item.invalid();
        item.withData(winshun::Json(winshun::Json::Object {
            {"kind", "struct.member"},
            {"primary", primary},
            {"detail", detail},
            {"chain", m["chain"].toString()},
            {"expand", false},
        }));
        items.push_back(std::move(item));
    }
    return items;
}

struct BinaryLab : winshun::Plugin {
    std::string name() const override { return "renfei.binarylab"; }

    // Cleanup runs after serve() has returned: close the editor window and
    // join its thread so the process exits with no UI thread left behind.
    ~BinaryLab() override
    {
        binlab::closeEditorAndWait(1500);
    }

    winshun::Items query(const winshun::Query& q) override
    {
        const bool zh = chinese();
        const Settings st = readSettings(settings());
        std::string command = q.command;
        std::string text = winshun::trim(q.text);
        if (command != "bits" && command != "bytes" && command != "struct") {
            const std::string raw = winshun::trim(q.raw);
            for (const char* keyword : {"bits", "bytes", "struct"}) {
                const std::string prefix = std::string(keyword) + " ";
                if (binlab::startsWith(raw, prefix)) {
                    command = keyword;
                    text = winshun::trim(raw.substr(prefix.size()));
                    break;
                }
                if (raw == keyword) {
                    command = keyword;
                    text.clear();
                    break;
                }
            }
        }
        if (q.raw.size() > binlab::kMaxInputChars)
            return {makeError(binlab::fail("E_INPUT_TOO_LARGE", "input exceeds 16 KiB", "bits u8 0xA5 & 0x0F"), zh)};
        if (text.empty() || text == "help" || text == "?" || text == "帮助")
            return helpItems(command, zh);

        winshun::Items items;
        if (command == "bits") {
            const auto r = binlab::evalBits(text, st.defaultWidth, false);
            items = r.success ? bitsItems(r.value, zh, st.editorOnEnter)
                              : winshun::Items {makeError(r.failure, zh)};
        } else if (command == "bytes") {
            const auto r = binlab::evalBytes(text, st.defaultEndian);
            items = r.success ? bytesItems(r.value, zh)
                              : winshun::Items {makeError(r.failure, zh)};
        } else if (command == "struct") {
            const auto r = binlab::evalStruct(text, st.defaultPack);
            items = r.success ? structItems(r.value, zh)
                              : winshun::Items {makeError(r.failure, zh)};
        } else {
            items = {makeError(binlab::fail("E_UNKNOWN_COMMAND", "unknown command",
                                            "bits u8 0xA5 & 0x0F"), zh)};
        }
        const int limit = q.limit > 0 ? q.limit : 200;
        if (static_cast<int>(items.size()) > limit)
            items.resize(static_cast<std::size_t>(limit));
        return items;
    }

    winshun::Effects action(const winshun::ActionRequest& a) override
    {
        const winshun::Json data = a.item["data"];
        const std::string copyField = a.item["copy"].isString() ? a.item["copy"].toString() : a.item["title"].toString();
        const std::string primary = data["primary"].isString() && !data["primary"].toString().empty()
                                        ? data["primary"].toString()
                                        : copyField;
        const std::string detail = data["detail"].isString() && !data["detail"].toString().empty()
                                       ? data["detail"].toString()
                                       : primary;
        const std::string chain = data["chain"].isString() && !data["chain"].toString().empty()
                                      ? data["chain"].toString()
                                      : a.text;
        const bool zh = chinese();
        if (a.action == "openEditor") {
            const winshun::Json d = a.item["data"];
            binlab::EditorSpec spec;
            spec.value = std::strtoull(d["valueHex"].toString().c_str(), nullptr, 16);
            spec.width = static_cast<int>(d["width"].toInt(32));
            if (spec.width <= 0 || spec.width > 64)
                spec.width = 32;
            spec.caption = a.text.empty() ? d["primary"].toString() : a.text;
            spec.zh = zh;
            std::string error;
            if (!binlab::openEditorWindow(spec, error)) {
                winshun::debug("renfei.binarylab: editor window failed: " + error);
                return {winshun::effect::notify(tr(zh, "无法打开编辑器窗口：", "Could not open the editor window: ") + error),
                        winshun::effect::hide()};
            }
            winshun::debug("renfei.binarylab: editor window opened");
            return {winshun::effect::notify(tr(zh, "交互编辑器已打开（Esc 关闭）", "Interactive editor opened (Esc closes)")),
                    winshun::effect::hide()};
        }
        if (a.action == "copyDetail")
            return {winshun::effect::copy(detail),
                    winshun::effect::notify(tr(zh, "已复制完整明细", "Copied full details")),
                    winshun::effect::hide()};
        if (a.action == "chainQuery")
            return {winshun::effect::setQuery(chain), winshun::effect::keep()};
        if ((a.action == "copyPrimary" || a.action == "default") && data["expand"].toBool()
            && data["members"].isArray())
            return {winshun::effect::replace(memberItems(data, zh)), winshun::effect::keep()};
        return {winshun::effect::copy(primary),
                winshun::effect::notify(tr(zh, "已复制", "Copied")),
                winshun::effect::hide()};
    }

    void settingsChanged(const winshun::Json& s) override
    {
        // The base class already replaced settings(); log only to stderr.
        (void)s;
        winshun::debug("renfei.binarylab: settingsChanged");
    }

    void cancel(long long queryId) override
    {
        // Every query is a bounded synchronous computation; the host discards
        // stale answers by queryId. There is no worker to abort here.
        winshun::debug("renfei.binarylab: cancel " + std::to_string(queryId));
    }
};

} // namespace

int main()
{
    return BinaryLab().serve();
}
