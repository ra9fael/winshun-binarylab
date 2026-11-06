// Row rules for record formats. Placeholder until the bounded evaluator
// lands: every rule compiles to "never matches", so formats with rules still
// load and decode.
#pragma once

#include "formats.h"

namespace binlab {

struct RuleNode {};

inline Outcome<RuleNodePtr> compileRule(const std::string&, const std::vector<std::string>&, const std::vector<int>&)
{
    return Outcome<RuleNodePtr>::good(std::make_shared<const RuleNode>());
}

inline int evalRuleOn(const RecordFormat&, const Record&, std::string&)
{
    return -1;
}

}  // namespace binlab
