#include "target/TargetUtils.hpp"
#include "model/ShaderDataSchema.hpp"
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

std::string_view EmittedMemberName(const ReflectedBinding& binding) noexcept
{
    return binding.ShimMember.empty() ? std::string_view{ binding.Name } : std::string_view{ binding.ShimMember };
}

std::string MakeScopedName(const ReflectedBinding& binding)
{
    std::string scoped = binding.ScopeName;
    scoped += scoped.empty() ? "" : "_";
    scoped += binding.Name;

    if (!binding.ShimMember.empty())
    {
        scoped += "_";
        scoped += binding.ShimMember;
    }

    return scoped;
}

}
