#pragma once
#ifndef LODESTONE_TARGET_UTILS_HPP
#define LODESTONE_TARGET_UTILS_HPP
#include <string>
#include <string_view>

// Common functionality for parsing reflection data, agnostic of target we're validating against
// Things like stripping slang name mangling, constructing structures from reflection data, etc
namespace lodestone
{

struct ReflectedBinding;

/** Removes the numeric suffix Slang appends to an emitted identifier (`IfftParams` ->
 * `IfftParams_0`), so comparison is on locations first and on de-mangled names second. */
std::string_view StripSlangNameMangling(std::string_view mangled_name) noexcept;
/** The name the WGSL emitter gives a scoped binding: `<scope>_<name>`. Only `WgslValidator` uses it. */
std::string MakeScopedName(const ReflectedBinding& binding);

}


#endif // !LODESTONE_TARGET_UTILS_HPP
