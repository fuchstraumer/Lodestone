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
/** Returns the name that emitted code gives to the member of a binding: `ShimMember` for a shim resource, else `Name`. */
std::string_view EmittedMemberName(const ReflectedBinding& binding) noexcept;
/** Returns `<scope>_<name>` for a plain binding and `<scope>_<name>_<member>` for a shim resource, which is the WGSL
 * emitter's name for the binding. Slang joins the names of the scopes, the shim field, and the member with underscores. */
std::string MakeScopedName(const ReflectedBinding& binding);

}


#endif // !LODESTONE_TARGET_UTILS_HPP
