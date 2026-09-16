#pragma once
#ifndef LODESTONE_ENUM_TAG_DECODE_HPP
#define LODESTONE_ENUM_TAG_DECODE_HPP
#include <cstdint>

// The integer read of a Slang enum case tag, factored out from behind the Slang wall.
//
// Slang reflection gives each enum case a raw value blob and a scalar type. The scalar type names
// the width and the signedness of the enum's underlying integer. `SlangModuleContext` maps that
// Slang scalar type to an `EnumTagKind` at the wall, and this function does the read. The function
// names no Slang type, so a unit test can prove the widen-and-sign logic with no compiler present.
namespace lodestone
{

/** The underlying integer type of a Slang enum, as a Slang-free kind.
 *
 * Each value names one fixed-width integer type. `DecodeEnumTag` reads a blob of that width. */
enum class EnumTagKind : uint8_t
{
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
};

/** Read one enum case tag from its raw bytes, and widen it to `int64_t`.
 *
 * `bytes` must point at a buffer of at least the width that `kind` names. The read is in host byte
 * order. A signed `kind` sign-extends to `int64_t`, and an unsigned `kind` zero-extends. A `UInt64`
 * tag at or above 2^63 wraps to a negative `int64_t`. The caller accepts this wrap, because a size
 * expression evaluates an enum tag as a signed 64-bit value. */
int64_t DecodeEnumTag(EnumTagKind kind, const void* bytes) noexcept;

} // namespace lodestone

#endif // !LODESTONE_ENUM_TAG_DECODE_HPP
