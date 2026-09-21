#include "target/TargetUtils.hpp"
#include "model/ShaderDataSchema.hpp"
#include <cassert>
#include <format>
#include <string>
#include <string_view>

namespace lodestone
{

std::string_view StripSlangNameMangling(std::string_view mangled_name) noexcept
{
    size_t end = mangled_name.size();
    while (end > 0u && mangled_name[end - 1u] >= '0' && mangled_name[end - 1u] <= '9')
    {
        --end;
    }

    if (end > 0u && end < mangled_name.size() && mangled_name[end - 1u] == '_')
    {
        return mangled_name.substr(0u, end - 1u);
    }

    return mangled_name;
}

std::string MakeScopedName(const ReflectedBinding& binding, std::string_view target_name)
{
    if (binding.ScopeName.empty())
    {
        return binding.Name;
    }

    // we'll need to update this as we add other targets, depending on how they expect targets
    // to structure their scoped names
    // for now, asserting on target_name so that it's immediately clear to me i forgot to update this
    assert(target_name == "wgsl");
    return std::format("{}_{}", binding.ScopeName, binding.Name);
}

}
