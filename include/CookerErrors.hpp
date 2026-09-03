#pragma once
#ifndef LODESTONE_ERRORS_HPP
#define LODESTONE_ERRORS_HPP
#include <cstdint>
#include <expected>
#include <string_view>
#include <source_location>

namespace lodestone
{

//NOLINTNEXTLINE(performance-enum-size)
enum class CookError : uint16_t
{
    Invalid = 0,
    Success = 1,
    GlobalSessionCreationFailed = 10,
    SessionCreationFailed = 11,
    ModuleLoadFailed = 12,
    EntryPointEnumerationFailed = 13,
    VariantModuleCreationFailed = 14,
    CompositeCreationFailed = 15,
    LinkFailed = 16,
    CodeGenerationFailed = 17,
    CompilerNotInitialized = 18,
    CompilerGlobalSessionCreationFailed = 19,
    FailedToLoadModuleDependencySource = 20,
    VariantNotCompiled = 21,

    ReflectionUnavailable = 40,
    ReflectionMismatch = 41,
    ReflectionSizeUnresolved = 42,
    AttributeExpressionParseFailed = 43,
    AttributeExpressionUnknownSymbol = 44,
    AttributeExpressionDivideByZero = 45,
    AttributeExpressionOutOfRange = 46,
    PointerTypeNotSupported = 47, // tried to use pointer placements w unsupported target
    ReflectionCouldNotFindBufferElementSize = 48,
    ReflectionUnsupportedBindingKind = 49,

    NoModulesSpecified = 60,
    NoOutputSpecified = 61,
    UnknownArgument = 62,
    MalformedArgument = 63,
    UnknownTargetProfile = 64,

    PermutationSpaceNotFound = 80,
    PermutationValueNotInAxis = 81,
    PermutationAxisNotDeclared = 82,
    PermutationVariantIndexCollision = 83,

    LibraryRoundTripFailed = 90,
    CookNotDeterministic = 91,
    ModulePolicyViolated = 92,
    ManifestVariantWorkgroupSizeMismatch = 93,
    ManifestVariantSourceCodeMismatch = 94,
    ManifestMissingVariant = 95,
    ManifestVariantMissingEntryPoint = 96,
    ManifestVariantResourceVisibilityMismatch = 97,
    ManifestVariantResourceResolveOutOfRange = 98,
    ManifestVariantResourceBindingMismatch = 99,
    ManifestVertexInputMismatch = 100,
    ManifestColorTargetMismatch = 101,
    ManifestRasterStateMismatch = 102,


    // output and diagnostic sink operation errors
    OutputPathInvalid = 120,
    OutputFileOpenFailed = 121,
    OutputWriteFailed = 122,
    ArtifactAlreadyWritten = 123,

    // start system errors
    SystemError = 200,
    FilesystemError = 201, // will expand later

    SlangErrors = 220,
    SlangCoreModuleLoadFailed = 221,
    SlangCoreModuleBuildFailed = 222
};

constexpr bool operator!(CookError error) noexcept
{
    return error != CookError::Success;
}

constexpr bool IsSuccess(CookError error) noexcept
{
    return error == CookError::Success;
}

template<typename T>
using CookResult = std::expected<T, CookError>;

std::string_view ToString(CookError error) noexcept;

} // namespace lodestone

#endif // !LODESTONE_ERRORS_HPP
