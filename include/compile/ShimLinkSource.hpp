#pragma once
#ifndef LODESTONE_SHIM_LINK_SOURCE_HPP
#define LODESTONE_SHIM_LINK_SOURCE_HPP
#include "CookerErrors.hpp"
#include "compile/RawLibrary.hpp"
#include "target/TargetProfile.hpp"
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lodestone
{

class DiagnosticSink;

/** @brief The comma-separated type arguments of a shim extern, trimmed. Empty when there are none, and also
 * when an argument is not a plain identifier, so the caller compares the count with the interface. */
std::vector<std::string_view> SplitShimTypeArguments(std::string_view type_arguments);
/**@brief The source of the module that links the shim implementations of one profile.
 * @return Generated source string on success. `ShimFormUnavailable` for unsupported shim forms.
 * `ShimExternInvalid` for invalid externs. */
CookResult<std::string> MakeShimLinkSource(std::span<const RawShimExtern> externs,
                                           AccessModel access,
                                           std::string_view module_name,
                                           std::string_view profile_name,
                                           DiagnosticSink& sink);

} // namespace lodestone

#endif // !LODESTONE_SHIM_LINK_SOURCE_HPP
