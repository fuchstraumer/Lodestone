#pragma once
#ifndef LODESTONE_ENUM_TAG_DECODE_HPP
#define LODESTONE_ENUM_TAG_DECODE_HPP
#include <cstddef>
#include <cstdint>

// Decodes Slang enum case tags with no Slang type, so that the widen-and-sign logic tests without a compiler.
namespace lodestone
{

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

/** Reads a tag in host byte order and widens it to `int64_t`. A `UInt64` tag at or above 2^63 wraps negative, which
 * size expressions accept: they evaluate as signed 64-bit. */
int64_t DecodeEnumTag(EnumTagKind kind, const void* bytes) noexcept;

size_t EnumTagByteWidth(EnumTagKind kind) noexcept;

} // namespace lodestone

#endif // !LODESTONE_ENUM_TAG_DECODE_HPP
