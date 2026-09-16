#include "compile/EnumTagDecode.hpp"
#include <cstring>

namespace lodestone
{

namespace
{
    // Copy the exact width out of the blob, then let the widening conversion do the work. A signed
    // T sign-extends to int64_t, and an unsigned T zero-extends. memcpy avoids an alignment fault,
    // because the blob pointer has no guaranteed alignment.
    template<typename T>
    int64_t WidenTag(const void* bytes) noexcept
    {
        T value{};
        std::memcpy(&value, bytes, sizeof(T));
        return static_cast<int64_t>(value);
    }
}

int64_t DecodeEnumTag(EnumTagKind kind, const void* bytes) noexcept
{
    switch (kind)
    {
    case EnumTagKind::Int8:
        return WidenTag<int8_t>(bytes);
    case EnumTagKind::UInt8:
        return WidenTag<uint8_t>(bytes);
    case EnumTagKind::Int16:
        return WidenTag<int16_t>(bytes);
    case EnumTagKind::UInt16:
        return WidenTag<uint16_t>(bytes);
    case EnumTagKind::Int32:
        return WidenTag<int32_t>(bytes);
    case EnumTagKind::UInt32:
        return WidenTag<uint32_t>(bytes);
    case EnumTagKind::Int64:
        return WidenTag<int64_t>(bytes);
    case EnumTagKind::UInt64:
        return WidenTag<uint64_t>(bytes);
    }
    return 0;
}

} // namespace lodestone
