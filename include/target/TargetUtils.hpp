#pragma once
#ifndef LODESTONE_TARGET_UTILS_HPP
#define LODESTONE_TARGET_UTILS_HPP
#include <string>
#include <string_view>

namespace lodestone
{

struct ReflectedBinding;

/** Removes Slang's numeric suffix (`IfftParams_0` -> `IfftParams`). */
std::string_view StripSlangNameMangling(std::string_view mangled_name) noexcept;
/** Returns `<scope>_<name>`, the WGSL emitter's name for a scoped binding. */
std::string MakeScopedName(const ReflectedBinding& binding);

}


#endif // !LODESTONE_TARGET_UTILS_HPP
