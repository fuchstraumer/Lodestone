#pragma once
#ifndef LODESTONE_TESTS_HOST_HEADER_TEST_TYPES_HPP
#define LODESTONE_TESTS_HOST_HEADER_TEST_TYPES_HPP
#include <cstdint>

/** @file The host types of the generated header test. The types have no alignment beyond their members, so each size is
 * the packed size of its components. The header test compares the layout that Slang gives with these. */

struct half
{
    uint16_t Bits;
};

template <class Component, int ComponentCount>
struct HostVector
{
    Component Values[ComponentCount];
};

template <class Component, int RowCount, int ColumnCount>
struct HostMatrix
{
    Component Values[RowCount * ColumnCount];
};

using float2 = HostVector<float, 2>;
using float3 = HostVector<float, 3>;
using float4 = HostVector<float, 4>;
using double2 = HostVector<double, 2>;
using double3 = HostVector<double, 3>;
using double4 = HostVector<double, 4>;
using int2 = HostVector<int32_t, 2>;
using int3 = HostVector<int32_t, 3>;
using int4 = HostVector<int32_t, 4>;
using uint2 = HostVector<uint32_t, 2>;
using uint3 = HostVector<uint32_t, 3>;
using uint4 = HostVector<uint32_t, 4>;
using half2 = HostVector<half, 2>;
using half3 = HostVector<half, 3>;
using half4 = HostVector<half, 4>;

using float2x2 = HostMatrix<float, 2, 2>;
using float2x3 = HostMatrix<float, 2, 3>;
using float2x4 = HostMatrix<float, 2, 4>;
using float3x2 = HostMatrix<float, 3, 2>;
using float3x3 = HostMatrix<float, 3, 3>;
using float3x4 = HostMatrix<float, 3, 4>;
using float4x2 = HostMatrix<float, 4, 2>;
using float4x3 = HostMatrix<float, 4, 3>;
using float4x4 = HostMatrix<float, 4, 4>;

#endif // !LODESTONE_TESTS_HOST_HEADER_TEST_TYPES_HPP
