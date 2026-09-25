#include "target/TargetUtils.hpp"
#include "model/ShaderDataSchema.hpp"
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

std::string MakeScopedName(const ReflectedBinding& binding)
{
    if (binding.ScopeName.empty())
    {
        return binding.Name;
    }

    return std::format("{}_{}", binding.ScopeName, binding.Name);
}

}
