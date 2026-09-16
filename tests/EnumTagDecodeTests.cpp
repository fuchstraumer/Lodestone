#include "compile/EnumTagDecode.hpp"
#include "TestHarness.hpp"

#include <cstdint>
#include <cstring>

using lodestone::DecodeEnumTag;
using lodestone::EnumTagKind;

namespace
{
    // Lay one integer down in host byte order, exactly as Slang's value blob arrives, then read it
    // back through the decode under test. The template mirrors the memcpy the decode itself does, so
    // the test states the intended value and never hand-assembles bytes.
    template<typename T>
    int64_t Decode(EnumTagKind kind, T value)
    {
        unsigned char bytes[sizeof(T)]{};
        std::memcpy(bytes, &value, sizeof(T));
        return DecodeEnumTag(kind, bytes);
    }
}

int main()
{
    lodestone::tests::TestRunner runner{ "EnumTagDecodeTests" };

    runner.BeginSection("an unsigned tag zero-extends");
    runner.Check(Decode<uint8_t>(EnumTagKind::UInt8, 0u) == 0, "a zero byte reads as zero");
    runner.Check(Decode<uint8_t>(EnumTagKind::UInt8, 200u) == 200, "a byte above the signed range stays positive");
    runner.Check(Decode<uint16_t>(EnumTagKind::UInt16, 40000u) == 40000, "a 16-bit value stays positive");
    runner.Check(Decode<uint32_t>(EnumTagKind::UInt32, 3000000000u) == 3000000000LL, "a 32-bit value above 2^31 stays positive");

    runner.BeginSection("a positive signed tag reads its value");
    runner.Check(Decode<int8_t>(EnumTagKind::Int8, static_cast<int8_t>(5)) == 5, "a small Int8 reads its value");
    runner.Check(Decode<int16_t>(EnumTagKind::Int16, static_cast<int16_t>(1000)) == 1000, "an Int16 reads its value");
    runner.Check(Decode<int32_t>(EnumTagKind::Int32, 1) == 1, "an Int32 reads its value");
    runner.Check(Decode<int64_t>(EnumTagKind::Int64, 10) == 10, "an Int64 reads its value");

    runner.BeginSection("a negative signed tag sign-extends at each width");
    runner.Check(Decode<int8_t>(EnumTagKind::Int8, static_cast<int8_t>(-1)) == -1, "an Int8 of -1 sign-extends");
    runner.Check(Decode<int8_t>(EnumTagKind::Int8, static_cast<int8_t>(-128)) == -128, "the Int8 minimum sign-extends");
    runner.Check(Decode<int16_t>(EnumTagKind::Int16, static_cast<int16_t>(-1000)) == -1000, "an Int16 sign-extends");
    runner.Check(Decode<int32_t>(EnumTagKind::Int32, -2000000000) == -2000000000LL, "an Int32 sign-extends");
    runner.Check(Decode<int64_t>(EnumTagKind::Int64, -5000000000LL) == -5000000000LL, "an Int64 keeps a value past the 32-bit range");

    runner.BeginSection("the maximum unsigned value at each width reads whole");
    runner.Check(Decode<uint8_t>(EnumTagKind::UInt8, static_cast<uint8_t>(0xFFu)) == 255, "the UInt8 maximum reads whole");
    runner.Check(Decode<uint16_t>(EnumTagKind::UInt16, static_cast<uint16_t>(0xFFFFu)) == 65535, "the UInt16 maximum reads whole");
    runner.Check(Decode<uint32_t>(EnumTagKind::UInt32, 0xFFFFFFFFu) == 4294967295LL, "the UInt32 maximum reads whole");

    runner.BeginSection("a UInt64 tag at or above 2^63 wraps to a negative int64");
    runner.Check(Decode<uint64_t>(EnumTagKind::UInt64, 9223372036854775808ull) == INT64_MIN, "2^63 wraps to the int64 minimum");
    runner.Check(Decode<uint64_t>(EnumTagKind::UInt64, 0xFFFFFFFFFFFFFFFFull) == -1, "the UInt64 maximum wraps to -1");
    runner.Check(Decode<uint64_t>(EnumTagKind::UInt64, 42ull) == 42, "a small UInt64 reads its value");

    return runner.Report();
}
