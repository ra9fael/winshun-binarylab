// Row rules for record formats: a bounded evaluator for the expression subset
// hextract allows, without executing any Python.
//
// Accepted: integer / float literals, True / False, field names (an array
// field is a tuple), loop variables, arr[i] (negative indices count from the
// end), unary + - not ~, ** * / // % + - << >> & ^ |, chained comparisons
// == != < <= > >=, and / or, calls to any all min max abs len, and a single
// generator argument `f(EXPR for v in ARR)`. Everything else - attributes,
// slices, lambdas, strings, unknown names or functions, names starting with
// "_" - is rejected when the format loads, so a bad rule never reaches a row.
//
// Bounds: <= 256 AST nodes and nesting depth <= 32 per rule, <= 4096
// evaluation steps per rule per row; a row that runs out reports a rule error.
#pragma once

#include "formats.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <set>
#include <memory>
#include <string>
#include <vector>

namespace binlab {

constexpr int kMaxRuleNodes = 256;
constexpr int kMaxRuleDepth = 32;
constexpr int kMaxRuleSteps = 4096;

struct RuleValue {
    enum Kind { Int, Real, Tuple } kind = Int;
    std::int64_t i = 0;
    double d = 0;
    std::vector<RuleValue> items;

    static RuleValue ofInt(std::int64_t v) { RuleValue r; r.kind = Int; r.i = v; return r; }
    static RuleValue ofReal(double v) { RuleValue r; r.kind = Real; r.d = v; return r; }
    double num() const { return kind == Int ? static_cast<double>(i) : d; }
    bool truthy() const { return kind == Tuple ? !items.empty() : kind == Int ? i != 0 : d != 0.0; }
};

struct RuleNode {
    enum Kind { Lit, Field, Local, Unary, Binary, Compare, And, Or, Not, Call, Gen, Index } kind = Lit;
    std::string op;                  // operator text or function name
    RuleValue lit;
    int slot = 0;                    // field index / local depth
    std::vector<RuleNodePtr> kids;   // operands; Compare: n operands, ops in `ops`
    std::vector<std::string> ops;
};

namespace rules_detail {

struct Tok {
    enum Kind { End, Num, Name, Op } kind = End;
    std::string text;
    std::size_t pos = 0;
};

inline Outcome<std::vector<Tok>> lex(const std::string& s)
{
    std::vector<Tok> out;
    std::size_t i = 0;
    const auto bad = [&](const std::string& m) {
        return failWith<std::vector<Tok>>(fail("E_RULE_SYNTAX", m + " at character " + std::to_string(i + 1), ""));
    };
    while (i < s.size()) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        Tok t;
        t.pos = i;
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < s.size()
                                                            && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            std::size_t j = i;
            if (c == '0' && j + 1 < s.size() && (s[j + 1] == 'x' || s[j + 1] == 'X')) {
                j += 2;
                while (j < s.size() && (std::isxdigit(static_cast<unsigned char>(s[j])) || s[j] == '_'))
                    ++j;
            } else {
                while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '.' || s[j] == '_'
                                        || ((s[j] == '+' || s[j] == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E'))))
                    ++j;
            }
            t.kind = Tok::Num;
            t.text = s.substr(i, j - i);
            i = j;
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t j = i;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_'))
                ++j;
            t.kind = Tok::Name;
            t.text = s.substr(i, j - i);
            i = j;
        } else {
            static const char* const kOps[] = { "**", "//", "<<", ">>", "<=", ">=", "==", "!=",
                                                "+", "-", "*", "/", "%", "&", "|", "^", "~", "<", ">",
                                                "(", ")", "[", "]", ",", ".", ":", "=" };
            bool found = false;
            for (const char* op : kOps) {
                const std::size_t n = std::strlen(op);
                if (s.compare(i, n, op) == 0) {
                    t.kind = Tok::Op;
                    t.text = op;
                    i += n;
                    found = true;
                    break;
                }
            }
            if (!found)
                return bad(std::string("unexpected '") + c + "'");
        }
        out.push_back(t);
    }
    Tok end;
    end.pos = s.size();
    out.push_back(end);
    return Outcome<std::vector<Tok>>::good(out);
}

class Parser {
public:
    Parser(std::vector<Tok> toks, const std::vector<std::string>& names, const std::vector<int>& counts)
        : m_toks(std::move(toks)), m_names(names), m_counts(counts) {}

    Outcome<RuleNodePtr> parse()
    {
        RuleNodePtr n = orExpr();
        if (m_failed)
            return failWith<RuleNodePtr>(m_error);
        if (peek().kind != Tok::End)
            return failWith<RuleNodePtr>(err("unexpected '" + peek().text + "'"));
        return Outcome<RuleNodePtr>::good(n);
    }

private:
    std::vector<Tok> m_toks;
    std::size_t m_at = 0;
    const std::vector<std::string>& m_names;
    const std::vector<int>& m_counts;
    std::vector<std::string> m_locals;
    int m_nodes = 0;
    int m_depth = 0;
    bool m_failed = false;
    Failure m_error;

    const Tok& peek(std::size_t k = 0) const { return m_toks[std::min(m_at + k, m_toks.size() - 1)]; }
    bool isOp(const char* op, std::size_t k = 0) const { return peek(k).kind == Tok::Op && peek(k).text == op; }
    bool isWord(const char* w) const { return peek().kind == Tok::Name && peek().text == w; }

    Failure err(const std::string& m) const
    {
        return fail("E_RULE_SYNTAX", m + " at character " + std::to_string(peek().pos + 1), "");
    }
    RuleNodePtr stop(const std::string& m, const char* code = "E_RULE_SYNTAX")
    {
        if (!m_failed) {
            m_failed = true;
            m_error = err(m);
            m_error.code = code;
        }
        return nullptr;
    }
    std::shared_ptr<RuleNode> node(RuleNode::Kind k)
    {
        if (++m_nodes > kMaxRuleNodes) {
            stop("rule has more than " + std::to_string(kMaxRuleNodes) + " nodes", "E_RULE_LIMIT");
            return nullptr;
        }
        auto n = std::make_shared<RuleNode>();
        n->kind = k;
        return n;
    }
    struct DepthGuard {
        Parser& p;
        explicit DepthGuard(Parser& parser) : p(parser) { ++p.m_depth; }
        ~DepthGuard() { --p.m_depth; }
    };
    bool deep()
    {
        if (m_depth > kMaxRuleDepth) {
            stop("rule nests deeper than " + std::to_string(kMaxRuleDepth), "E_RULE_LIMIT");
            return true;
        }
        return false;
    }

    RuleNodePtr binaryChain(RuleNodePtr (Parser::*next)(), std::initializer_list<const char*> ops, RuleNode::Kind kind)
    {
        RuleNodePtr left = (this->*next)();
        while (!m_failed) {
            const char* hit = nullptr;
            for (const char* op : ops)
                if (isOp(op))
                    hit = op;
            if (hit == nullptr)
                break;
            ++m_at;
            RuleNodePtr right = (this->*next)();
            auto n = node(kind);
            if (!n || m_failed)
                return nullptr;
            n->op = hit;
            n->kids = { left, right };
            left = n;
        }
        return left;
    }

    RuleNodePtr orExpr()
    {
        DepthGuard g(*this);
        if (deep())
            return nullptr;
        RuleNodePtr left = andExpr();
        while (!m_failed && isWord("or")) {
            ++m_at;
            RuleNodePtr right = andExpr();
            auto n = node(RuleNode::Or);
            if (!n || m_failed)
                return nullptr;
            n->kids = { left, right };
            left = n;
        }
        return left;
    }
    RuleNodePtr andExpr()
    {
        RuleNodePtr left = notExpr();
        while (!m_failed && isWord("and")) {
            ++m_at;
            RuleNodePtr right = notExpr();
            auto n = node(RuleNode::And);
            if (!n || m_failed)
                return nullptr;
            n->kids = { left, right };
            left = n;
        }
        return left;
    }
    RuleNodePtr notExpr()
    {
        if (isWord("not")) {
            ++m_at;
            DepthGuard g(*this);
            if (deep())
                return nullptr;
            RuleNodePtr operand = notExpr();
            auto n = node(RuleNode::Not);
            if (!n || m_failed)
                return nullptr;
            n->kids = { operand };
            return n;
        }
        return compare();
    }
    RuleNodePtr compare()
    {
        RuleNodePtr first = bitOr();
        static const char* const kCmp[] = { "==", "!=", "<=", ">=", "<", ">" };
        std::vector<RuleNodePtr> operands = { first };
        std::vector<std::string> ops;
        while (!m_failed) {
            const char* hit = nullptr;
            for (const char* op : kCmp)
                if (isOp(op)) {
                    hit = op;
                    break;
                }
            if (hit == nullptr) {
                if (isWord("in") || isWord("is"))
                    return stop("'" + peek().text + "' comparisons are not supported");
                break;
            }
            ++m_at;
            ops.push_back(hit);
            operands.push_back(bitOr());
        }
        if (m_failed || ops.empty())
            return first;
        auto n = node(RuleNode::Compare);
        if (!n)
            return nullptr;
        n->kids = operands;
        n->ops = ops;
        return n;
    }
    RuleNodePtr bitOr() { return binaryChain(&Parser::bitXor, { "|" }, RuleNode::Binary); }
    RuleNodePtr bitXor() { return binaryChain(&Parser::bitAnd, { "^" }, RuleNode::Binary); }
    RuleNodePtr bitAnd() { return binaryChain(&Parser::shift, { "&" }, RuleNode::Binary); }
    RuleNodePtr shift() { return binaryChain(&Parser::sum, { "<<", ">>" }, RuleNode::Binary); }
    RuleNodePtr sum() { return binaryChain(&Parser::term, { "+", "-" }, RuleNode::Binary); }
    RuleNodePtr term() { return binaryChain(&Parser::unary, { "*", "/", "//", "%" }, RuleNode::Binary); }
    RuleNodePtr unary()
    {
        if (isOp("-") || isOp("+") || isOp("~")) {
            const std::string op = peek().text;
            ++m_at;
            DepthGuard g(*this);
            if (deep())
                return nullptr;
            RuleNodePtr operand = unary();
            auto n = node(RuleNode::Unary);
            if (!n || m_failed)
                return nullptr;
            n->op = op;
            n->kids = { operand };
            return n;
        }
        return power();
    }
    RuleNodePtr power()
    {
        RuleNodePtr base = postfix();
        if (!m_failed && isOp("**")) {
            ++m_at;
            RuleNodePtr exp = unary(); // right-associative, binds tighter than unary on its left
            auto n = node(RuleNode::Binary);
            if (!n || m_failed)
                return nullptr;
            n->op = "**";
            n->kids = { base, exp };
            return n;
        }
        return base;
    }
    RuleNodePtr postfix()
    {
        RuleNodePtr p = primary();
        while (!m_failed) {
            if (isOp(".")) {
                return stop("attribute access is not allowed");
            } else if (isOp("[")) {
                ++m_at;
                RuleNodePtr index = orExpr();
                if (m_failed)
                    return nullptr;
                if (isOp(":"))
                    return stop("slices are not supported");
                if (!isOp("]"))
                    return stop("expected ']'");
                ++m_at;
                auto n = node(RuleNode::Index);
                if (!n)
                    return nullptr;
                n->kids = { p, index };
                p = n;
            } else if (isOp("(")) {
                return stop("only any all min max abs len can be called");
            } else {
                break;
            }
        }
        return p;
    }
    RuleNodePtr number()
    {
        std::string t;
        for (char c : peek().text)
            if (c != '_')
                t += c;
        ++m_at;
        auto n = node(RuleNode::Lit);
        if (!n)
            return nullptr;
        const bool hex = t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X');
        const bool real = !hex && t.find_first_of(".eE") != std::string::npos;
        char* end = nullptr;
        errno = 0;
        if (real) {
            const double d = std::strtod(t.c_str(), &end);
            if (end == t.c_str() || *end != '\0')
                return stop("bad number '" + t + "'");
            n->lit = RuleValue::ofReal(d);
        } else {
            const unsigned long long v = std::strtoull(t.c_str(), &end, hex ? 16 : 10);
            if (end == t.c_str() || *end != '\0' || errno == ERANGE
                || v > static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max()))
                return stop("bad or too large integer '" + t + "'");
            n->lit = RuleValue::ofInt(static_cast<std::int64_t>(v));
        }
        return n;
    }
    RuleNodePtr primary()
    {
        DepthGuard g(*this);
        if (deep())
            return nullptr;
        const Tok& t = peek();
        if (t.kind == Tok::Num)
            return number();
        if (isOp("(")) {
            ++m_at;
            RuleNodePtr inner = orExpr();
            if (m_failed)
                return nullptr;
            if (isOp(","))
                return stop("tuple literals are not supported");
            if (!isOp(")"))
                return stop("expected ')'");
            ++m_at;
            return inner;
        }
        if (t.kind != Tok::Name)
            return stop(t.kind == Tok::End ? "rule ends early" : "unexpected '" + t.text + "'");
        const std::string name = t.text;
        if (!name.empty() && name[0] == '_')
            return stop("rule references private name '" + name + "'");
        static const std::set<std::string> kKeywords = { "and", "or", "not", "for", "in", "if", "else",
                                                         "lambda", "is", "None", "import" };
        if (kKeywords.count(name))
            return stop("'" + name + "' is not allowed here");
        ++m_at;
        if (name == "True" || name == "False" || name == "true" || name == "false") {
            auto n = node(RuleNode::Lit);
            if (!n)
                return nullptr;
            n->lit = RuleValue::ofInt(name[0] == 'T' || name[0] == 't' ? 1 : 0);
            return n;
        }
        static const std::set<std::string> kCalls = { "any", "all", "min", "max", "abs", "len" };
        if (kCalls.count(name)) {
            if (!isOp("("))
                return stop("'" + name + "' must be called");
            ++m_at;
            return call(name);
        }
        for (std::size_t k = m_locals.size(); k-- > 0;)
            if (m_locals[k] == name) {
                auto n = node(RuleNode::Local);
                if (!n)
                    return nullptr;
                n->slot = static_cast<int>(k);
                return n;
            }
        for (std::size_t k = 0; k < m_names.size(); ++k)
            if (m_names[k] == name) {
                auto n = node(RuleNode::Field);
                if (!n)
                    return nullptr;
                n->slot = static_cast<int>(k);
                return n;
            }
        if (isOp("("))
            return stop("rule calls disallowed function '" + name + "'");
        return stop("rule references unknown name '" + name + "'");
    }
    RuleNodePtr call(const std::string& fn)
    {
        auto n = node(RuleNode::Call);
        if (!n)
            return nullptr;
        n->op = fn;
        if (isOp(")"))
            return stop(fn + "() needs an argument");
        // A generator is `EXPR for NAME in ITER`; the loop name is only known
        // after EXPR, so scan ahead for a top-level `for` and bind it first.
        std::size_t depth = 0;
        std::size_t forAt = 0;
        for (std::size_t k = m_at; k < m_toks.size() && m_toks[k].kind != Tok::End; ++k) {
            const Tok& tk = m_toks[k];
            if (tk.kind == Tok::Op && (tk.text == "(" || tk.text == "["))
                ++depth;
            else if (tk.kind == Tok::Op && (tk.text == ")" || tk.text == "]")) {
                if (depth == 0)
                    break;
                --depth;
            } else if (depth == 0 && tk.kind == Tok::Name && tk.text == "for") {
                forAt = k;
                break;
            }
        }
        if (forAt != 0) {
            if (forAt + 3 >= m_toks.size() || m_toks[forAt + 1].kind != Tok::Name || m_toks[forAt + 2].text != "in")
                return stop("a generator must look like 'EXPR for v in ARRAY'");
            const std::string var = m_toks[forAt + 1].text;
            if (var.empty() || var[0] == '_')
                return stop("rule references private name '" + var + "'");
            // ITER is parsed outside the loop scope, EXPR inside it.
            const std::size_t exprAt = m_at;
            m_at = forAt + 3;
            RuleNodePtr iter = orExpr();
            if (m_failed)
                return nullptr;
            if (isWord("for") || isWord("if"))
                return stop("nested or filtered generators are not supported");
            if (!isOp(")"))
                return stop("expected ')' after the generator");
            const std::size_t after = m_at + 1;
            m_at = exprAt;
            m_locals.push_back(var);
            RuleNodePtr body = orExpr();
            m_locals.pop_back();
            if (m_failed)
                return nullptr;
            if (m_at != forAt)
                return stop("unexpected '" + peek().text + "' in the generator");
            m_at = after;
            auto g = node(RuleNode::Gen);
            if (!g)
                return nullptr;
            g->slot = static_cast<int>(m_locals.size());
            g->kids = { body, iter };
            n->kids = { g };
            return n;
        }
        while (!m_failed) {
            n->kids.push_back(orExpr());
            if (m_failed)
                return nullptr;
            if (isOp(",")) {
                ++m_at;
                continue;
            }
            if (!isOp(")"))
                return stop("expected ')'");
            ++m_at;
            break;
        }
        const std::size_t argc = n->kids.size();
        if ((fn == "abs" || fn == "len" || fn == "any" || fn == "all") && argc != 1)
            return stop(fn + "() takes exactly one argument");
        return n;
    }
};

struct Eval {
    const std::vector<RuleValue>& fields;
    std::vector<RuleValue> locals;
    int steps = 0;
    std::string error;

    bool failNow(const std::string& m)
    {
        if (error.empty())
            error = m;
        return false;
    }

    bool num(const RuleValue& v, const char* what)
    {
        if (v.kind == RuleValue::Tuple)
            return failNow(std::string(what) + " needs a number, got an array");
        return true;
    }

    // Python int semantics where it matters to a rule: exact integer math with
    // an explicit overflow error instead of silent wrap, floor division and a
    // modulo that follows the sign of the divisor.
    bool arith(const std::string& op, const RuleValue& a, const RuleValue& b, RuleValue& out)
    {
        if (!num(a, op.c_str()) || !num(b, op.c_str()))
            return false;
        const bool ints = a.kind == RuleValue::Int && b.kind == RuleValue::Int;
        if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
            if (!ints)
                return failNow("'" + op + "' needs integers");
            if (op == "&") out = RuleValue::ofInt(a.i & b.i);
            else if (op == "|") out = RuleValue::ofInt(a.i | b.i);
            else if (op == "^") out = RuleValue::ofInt(a.i ^ b.i);
            else {
                if (b.i < 0)
                    return failNow("negative shift count");
                if (op == ">>")
                    out = RuleValue::ofInt(b.i >= 63 ? (a.i < 0 ? -1 : 0) : (a.i >> b.i));
                else {
                    if (b.i >= 63 || (a.i != 0 && (a.i > (std::numeric_limits<std::int64_t>::max() >> b.i)
                                                   || a.i < (std::numeric_limits<std::int64_t>::min() >> b.i))))
                        return failNow("integer overflow in <<");
                    out = RuleValue::ofInt(static_cast<std::int64_t>(static_cast<std::uint64_t>(a.i) << b.i));
                }
            }
            return true;
        }
        if (op == "/") {
            if (b.num() == 0.0)
                return failNow("division by zero");
            out = RuleValue::ofReal(a.num() / b.num());
            return true;
        }
        if (!ints) {
            const double x = a.num(), y = b.num();
            if (op == "+") out = RuleValue::ofReal(x + y);
            else if (op == "-") out = RuleValue::ofReal(x - y);
            else if (op == "*") out = RuleValue::ofReal(x * y);
            else if (op == "**") out = RuleValue::ofReal(std::pow(x, y));
            else {
                if (y == 0.0)
                    return failNow("division by zero");
                const double q = std::floor(x / y);
                out = RuleValue::ofReal(op == "//" ? q : x - q * y);
            }
            return true;
        }
        const std::int64_t x = a.i, y = b.i;
        std::int64_t r = 0;
        if (op == "+") {
            if (addOverflows(x, y, r))
                return failNow("integer overflow");
        } else if (op == "-") {
            if (addOverflows(x, -y, r) || y == std::numeric_limits<std::int64_t>::min())
                return failNow("integer overflow");
        } else if (op == "*") {
            if (x != 0 && y != 0) {
                const long double p = static_cast<long double>(x) * static_cast<long double>(y);
                if (p > static_cast<long double>(std::numeric_limits<std::int64_t>::max())
                    || p < static_cast<long double>(std::numeric_limits<std::int64_t>::min()))
                    return failNow("integer overflow");
            }
            r = x * y;
        } else if (op == "**") {
            if (y < 0) {
                out = RuleValue::ofReal(std::pow(static_cast<double>(x), static_cast<double>(y)));
                return true;
            }
            r = 1;
            for (std::int64_t k = 0; k < y; ++k) {
                if (++steps > kMaxRuleSteps)
                    return failNow("rule ran past " + std::to_string(kMaxRuleSteps) + " steps");
                const long double p = static_cast<long double>(r) * static_cast<long double>(x);
                if (p > static_cast<long double>(std::numeric_limits<std::int64_t>::max())
                    || p < static_cast<long double>(std::numeric_limits<std::int64_t>::min()))
                    return failNow("integer overflow");
                r *= x;
            }
        } else {
            if (y == 0)
                return failNow("division by zero");
            if (x == std::numeric_limits<std::int64_t>::min() && y == -1)
                return failNow("integer overflow");
            std::int64_t q = x / y;
            std::int64_t m = x % y;
            if (m != 0 && ((m < 0) != (y < 0))) {
                --q;
                m += y;
            }
            r = op == "//" ? q : m;
        }
        out = RuleValue::ofInt(r);
        return true;
    }

    static bool addOverflows(std::int64_t a, std::int64_t b, std::int64_t& out)
    {
        if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b)
            || (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))
            return true;
        out = a + b;
        return false;
    }

    static int cmp3(const RuleValue& a, const RuleValue& b)
    {
        if (a.kind == RuleValue::Int && b.kind == RuleValue::Int)
            return a.i < b.i ? -1 : a.i > b.i ? 1 : 0;
        const double x = a.num(), y = b.num();
        return x < y ? -1 : x > y ? 1 : 0;
    }

    // The values a generator or an array argument produces.
    bool iterate(const RuleNode& n, std::vector<RuleValue>& out)
    {
        if (n.kind == RuleNode::Gen) {
            RuleValue arr;
            if (!eval(*n.kids[1], arr))
                return false;
            if (arr.kind != RuleValue::Tuple)
                return failNow("a generator needs an array to loop over");
            for (const RuleValue& item : arr.items) {
                locals.push_back(item);
                RuleValue v;
                const bool ok = eval(*n.kids[0], v);
                locals.pop_back();
                if (!ok)
                    return false;
                out.push_back(v);
            }
            return true;
        }
        RuleValue v;
        if (!eval(n, v))
            return false;
        if (v.kind != RuleValue::Tuple)
            return failNow("expected an array");
        out = v.items;
        return true;
    }

    bool callFn(const RuleNode& n, RuleValue& out)
    {
        const std::string& fn = n.op;
        if (fn == "any" || fn == "all") {
            const bool want = fn == "any";
            // Short-circuit like Python: stop at the first deciding element.
            if (n.kids[0]->kind == RuleNode::Gen) {
                const RuleNode& g = *n.kids[0];
                RuleValue arr;
                if (!eval(*g.kids[1], arr))
                    return false;
                if (arr.kind != RuleValue::Tuple)
                    return failNow("a generator needs an array to loop over");
                for (const RuleValue& item : arr.items) {
                    locals.push_back(item);
                    RuleValue v;
                    const bool ok = eval(*g.kids[0], v);
                    locals.pop_back();
                    if (!ok)
                        return false;
                    if (v.truthy() == want) {
                        out = RuleValue::ofInt(want ? 1 : 0);
                        return true;
                    }
                }
                out = RuleValue::ofInt(want ? 0 : 1);
                return true;
            }
            std::vector<RuleValue> items;
            if (!iterate(*n.kids[0], items))
                return false;
            bool r = !want;
            for (const RuleValue& v : items)
                if (v.truthy() == want) {
                    r = want;
                    break;
                }
            out = RuleValue::ofInt(r ? 1 : 0);
            return true;
        }
        if (fn == "len") {
            if (n.kids[0]->kind == RuleNode::Gen)
                return failNow("len() of a generator");
            RuleValue v;
            if (!eval(*n.kids[0], v))
                return false;
            if (v.kind != RuleValue::Tuple)
                return failNow("len() needs an array");
            out = RuleValue::ofInt(static_cast<std::int64_t>(v.items.size()));
            return true;
        }
        if (fn == "abs") {
            RuleValue v;
            if (!eval(*n.kids[0], v) || !num(v, "abs()"))
                return false;
            if (v.kind == RuleValue::Int) {
                if (v.i == std::numeric_limits<std::int64_t>::min())
                    return failNow("integer overflow");
                out = RuleValue::ofInt(v.i < 0 ? -v.i : v.i);
            } else {
                out = RuleValue::ofReal(std::fabs(v.d));
            }
            return true;
        }
        // min / max: one iterable, or two or more numbers.
        std::vector<RuleValue> items;
        if (n.kids.size() == 1) {
            if (!iterate(*n.kids[0], items))
                return false;
        } else {
            for (const RuleNodePtr& k : n.kids) {
                RuleValue v;
                if (!eval(*k, v))
                    return false;
                items.push_back(v);
            }
        }
        if (items.empty())
            return failNow(fn + "() of an empty sequence");
        RuleValue best = items[0];
        for (const RuleValue& v : items) {
            if (!num(v, fn.c_str()))
                return false;
            const int c = cmp3(v, best);
            if ((fn == "min" && c < 0) || (fn == "max" && c > 0))
                best = v;
        }
        out = best;
        return true;
    }

    bool eval(const RuleNode& n, RuleValue& out)
    {
        if (++steps > kMaxRuleSteps)
            return failNow("rule ran past " + std::to_string(kMaxRuleSteps) + " steps");
        switch (n.kind) {
        case RuleNode::Lit:
            out = n.lit;
            return true;
        case RuleNode::Field:
            out = fields[static_cast<std::size_t>(n.slot)];
            return true;
        case RuleNode::Local:
            out = locals[static_cast<std::size_t>(n.slot)];
            return true;
        case RuleNode::Not: {
            RuleValue v;
            if (!eval(*n.kids[0], v))
                return false;
            out = RuleValue::ofInt(v.truthy() ? 0 : 1);
            return true;
        }
        case RuleNode::And:
        case RuleNode::Or: {
            RuleValue a;
            if (!eval(*n.kids[0], a))
                return false;
            if (a.truthy() == (n.kind == RuleNode::Or)) {
                out = a;
                return true;
            }
            return eval(*n.kids[1], out);
        }
        case RuleNode::Unary: {
            RuleValue v;
            if (!eval(*n.kids[0], v) || !num(v, n.op.c_str()))
                return false;
            if (n.op == "+") {
                out = v;
            } else if (n.op == "~") {
                if (v.kind != RuleValue::Int)
                    return failNow("'~' needs an integer");
                out = RuleValue::ofInt(~v.i);
            } else if (v.kind == RuleValue::Int) {
                if (v.i == std::numeric_limits<std::int64_t>::min())
                    return failNow("integer overflow");
                out = RuleValue::ofInt(-v.i);
            } else {
                out = RuleValue::ofReal(-v.d);
            }
            return true;
        }
        case RuleNode::Binary: {
            RuleValue a, b;
            if (!eval(*n.kids[0], a) || !eval(*n.kids[1], b))
                return false;
            return arith(n.op, a, b, out);
        }
        case RuleNode::Compare: {
            RuleValue left;
            if (!eval(*n.kids[0], left))
                return false;
            for (std::size_t k = 0; k < n.ops.size(); ++k) {
                RuleValue right;
                if (!eval(*n.kids[k + 1], right))
                    return false;
                if (!num(left, n.ops[k].c_str()) || !num(right, n.ops[k].c_str()))
                    return false;
                const int c = cmp3(left, right);
                const std::string& op = n.ops[k];
                const bool ok = op == "==" ? c == 0 : op == "!=" ? c != 0 : op == "<" ? c < 0
                              : op == "<=" ? c <= 0 : op == ">" ? c > 0 : c >= 0;
                if (!ok) {
                    out = RuleValue::ofInt(0);
                    return true;
                }
                left = right;
            }
            out = RuleValue::ofInt(1);
            return true;
        }
        case RuleNode::Index: {
            RuleValue arr, idx;
            if (!eval(*n.kids[0], arr) || !eval(*n.kids[1], idx))
                return false;
            if (arr.kind != RuleValue::Tuple)
                return failNow("only an array can be indexed");
            if (idx.kind != RuleValue::Int)
                return failNow("an index must be an integer");
            std::int64_t k = idx.i;
            const std::int64_t size = static_cast<std::int64_t>(arr.items.size());
            if (k < 0)
                k += size;
            if (k < 0 || k >= size)
                return failNow("index " + std::to_string(idx.i) + " out of range");
            out = arr.items[static_cast<std::size_t>(k)];
            return true;
        }
        case RuleNode::Call:
            return callFn(n, out);
        case RuleNode::Gen:
            return failNow("a generator is only allowed as the argument of any all min max");
        }
        return failNow("internal: unknown rule node");
    }
};

inline RuleValue cellValue(const Cell& c)
{
    if (c.kind == Cell::Float)
        return RuleValue::ofReal(c.f);
    if (c.kind == Cell::Signed)
        return RuleValue::ofInt(c.i);
    if (c.u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return RuleValue::ofReal(static_cast<double>(c.u));
    return RuleValue::ofInt(static_cast<std::int64_t>(c.u));
}

}  // namespace rules_detail

inline Outcome<RuleNodePtr> compileRule(const std::string& src, const std::vector<std::string>& fieldNames,
                                        const std::vector<int>& fieldCounts)
{
    if (src.size() > 1024)
        return failWith<RuleNodePtr>(fail("E_RULE_LIMIT", "rule is longer than 1024 characters", ""));
    auto toks = rules_detail::lex(src);
    if (!toks.success)
        return failWith<RuleNodePtr>(toks.failure);
    rules_detail::Parser p(std::move(toks.value), fieldNames, fieldCounts);
    return p.parse();
}

// Field values for one record: a scalar field is a number, an array field a
// tuple, both from the raw (post-bit-slice, unscaled) cell values.
inline std::vector<RuleValue> ruleFields(const RecordFormat& f, const Record& r)
{
    std::vector<RuleValue> out;
    for (const FieldSpec& fs : f.fields) {
        if (fs.count == 1) {
            out.push_back(rules_detail::cellValue(r.cells[static_cast<std::size_t>(fs.firstColumn)]));
        } else {
            RuleValue t;
            t.kind = RuleValue::Tuple;
            for (int k = 0; k < fs.count; ++k)
                t.items.push_back(rules_detail::cellValue(r.cells[static_cast<std::size_t>(fs.firstColumn + k)]));
            out.push_back(t);
        }
    }
    return out;
}

// Result of one rule on one record: 1 match, 0 no match, -1 error.
inline int evalOneRule(const RuleSpec& rule, const std::vector<RuleValue>& fields, std::string& error)
{
    rules_detail::Eval ev { fields, {}, 0, {} };
    RuleValue v;
    if (!ev.eval(*rule.code, v)) {
        error = ev.error;
        return -1;
    }
    return v.truthy() ? 1 : 0;
}

// First matching rule wins, as in hextract; an erroring rule counts as "no
// match" and the first error is kept for the row.
inline int evalRuleOn(const RecordFormat& f, const Record& r, std::string& error)
{
    if (f.rules.empty())
        return -1;
    const std::vector<RuleValue> fields = ruleFields(f, r);
    for (std::size_t i = 0; i < f.rules.size(); ++i) {
        std::string e;
        const int hit = evalOneRule(f.rules[i], fields, e);
        if (hit < 0 && error.empty())
            error = f.rules[i].name + ": " + e;
        if (hit == 1)
            return static_cast<int>(i);
    }
    return -1;
}

}  // namespace binlab
