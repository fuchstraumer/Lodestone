#pragma once
#ifndef LODESTONE_ERRORS_HPP
#define LODESTONE_ERRORS_HPP
#include <cstdint>
#include <expected>
#include <string_view>

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
    TargetProfileNotFound = 22,

    ReflectionUnavailable = 40,
    ReflectionMismatch = 41,
    ReflectionSizeUnresolved = 42,
    AttributeExpressionParseFailed = 43,
    AttributeExpressionUnknownSymbol = 44,
    AttributeExpressionDivideByZero = 45,
    AttributeExpressionOutOfRange = 46,
    PointerTypeNotSupported = 47, // tried to use pointer placements w unsupported target
    ReflectionCouldNotFindBufferElementLayout = 48,
    ReflectionUnsupportedBindingKind = 49,

    NoModulesSpecified = 60,
    NoOutputSpecified = 61,
    UnknownArgument = 62,
    MalformedArgument = 63,
    UnknownTargetProfile = 64,
    NoTargetSpecified = 65,

    PermutationSpaceNotFound = 80,
    PermutationValueNotInAxis = 81,
    PermutationAxisNotDeclared = 82,
    PermutationVariantIndexCollision = 83,
    PermutationConstraintForwardReference = 84,
    PermutationConstraintUnknownSymbol = 85,
    PermutationConstraintInvalidExpression = 86,
    PermutationConstraintEmptyRequireExpression = 87,
    PermutationVariantBudgetExceeded = 88,
    PermutationKeySpaceTooLarge = 89, // the key space for permutations exceeds the allowed limit (log2(i) < 64 (bits))

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
    ManifestAxisTooManyValues = 103, // a module axis holds more values than the 32-bit value mask can name
    ManifestModuleShapeMismatch = 104, // two profiles of one module disagree on its entry points or axes
    ManifestVariantKeyMismatch = 105, // a key decodes through the module axes to a different assignment
    ManifestDuplicateVariantKey = 106,
    ManifestVariantAxisMaskMismatch = 107,

    ManifestShaderLayoutVariantIndicesInvalid = 110,
    ManifestShaderLayoutVisiblityIndexInvalid = 111,
    ManifestShaderLayoutResourceListIndexInvalid = 112,

    // output and diagnostic sink operation errors
    OutputPathInvalid = 120,
    OutputPathTooLong = 121,
    OutputFileOpenFailed = 122,
    OutputWriteFailed = 123,
    ArtifactAlreadyWritten = 124,

    // policy file (TOML) checked against the module's declared axes
    PolicyAxisNotDeclared = 130,
    PolicyValueNotInAxis = 131,
    PolicyCookIfInvalid = 132,
    PolicyDocumentLoadFailed = 133,
    PolicyInertAxisNotInertWhenCooked = 134,

    // start system errors
    SystemError = 200,
    DirectoryDoesNotExist = 201,
    DirectoryCouldNotBeCreated = 202,
    FileNotFound = 203,
    FileWriteFailed = 204,
    FromCharsFailed = 205,
    PermissionDenied = 206,
    FileAlreadyExists = 207,
    InvalidPath = 208,

    SlangErrors = 220,
    SlangCoreModuleLoadFailed = 221,
    SlangCoreModuleBuildFailed = 222,
    SlangGetAttributeValueStrFailed = 223,
    SlangGetSourceLocationFailed = 224,
    SlangGetVariableNameFailed = 225,
    SlangInvalidAttributesOnDecl = 226,
    SlangProgramLayoutNotFound = 227,
    SlangMultipleConformingInterfaces = 228,
    SlangNoConformingInterfaces = 229,
    SlangInterfaceHasResourceMember = 230,
    SlangCachedModuleWriteFailed = 231,

    // start of target validation errors
    TargetValidatorUnavailable = 300,
    TargetValidationEntryPointParseFailed = 301
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
