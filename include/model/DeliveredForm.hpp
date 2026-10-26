#pragma once
#ifndef LODESTONE_DELIVERED_FORM_HPP
#define LODESTONE_DELIVERED_FORM_HPP
#include "CookerErrors.hpp"
#include <string_view>

namespace lodestone
{

class DiagnosticSink;
struct CompiledVariant;

/** @brief Checks that each entry point's delivered form needs no more than its base form: used bindings, sampler pairs,
 * capability names (less the names debug information adds), workgroup memory, and entry point constants. */
CookError VerifyDeliveredForm(const CompiledVariant& variant, std::string_view target_name, DiagnosticSink& sink);

} // namespace lodestone

#endif // !LODESTONE_DELIVERED_FORM_HPP
