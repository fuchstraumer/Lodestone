#pragma once
#ifndef LODESTONE_TARGET_SPV_MODULE_READER_HPP
#define LODESTONE_TARGET_SPV_MODULE_READER_HPP
#include "CookerErrors.hpp"
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"
#include "target/TargetProfile.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "spirv-tools/libspirv.h"
#define SPV_ENABLE_UTILITY_CODE
#include "spirv/unified1/spirv.hpp11"

/** Parses one SPIR-V module with SPIRV-Tools for `SpvValidator`. It names SPIR-V types, so it stays in `impl/`. */
namespace lodestone
{
class DiagnosticSink;
}

namespace lodestone::spirv
{

struct EntryPointInfo
{
    ShaderStageKind Stage{ ShaderStageKind::Invalid };
    std::string_view Name;
    // every global the entry point uses (SPIR-V 1.4+), unsorted
    std::vector<uint32_t> Interfaces;
    bool LocalSizeFromIds{ false };
    // the size, or the ids of its constants when LocalSizeFromIds
    uint32_t LocalSizeX{ 0u };
    uint32_t LocalSizeY{ 0u };
    uint32_t LocalSizeZ{ 0u };
};

struct MemberRecord
{
    uint32_t TypeId{ 0u };
    uint32_t Offset{ ~0u };
    uint32_t MatrixStride{ 0u };
    // as written. Slang writes ColMajor for what its reflection calls RowMajor, so a comparison inverts it
    MatrixLayout MatrixMajor{ MatrixLayout::Invalid };
    std::string_view Name;
};

struct PendingMemberDecoration
{
    uint32_t StructId{ 0u };
    uint32_t MemberIndex{ 0u };
    spv::Decoration Decoration{ spv::Decoration::Max };
    std::span<const uint32_t> Values;
    std::string_view Name;
};

/** Only `Spec` records with a SpecId are pipeline inputs. `SpecDerived` values are expressions, with no value. */
enum class ConstantKind : uint8_t
{
    None = 0,
    Constant,
    Spec,
    SpecDerived,
};

/** One SPIR-V id. It only becomes a resource after the parse. */
struct IdRecord
{
    std::string_view Name;
    uint32_t Binding{ ~0u };
    uint32_t Group{ ~0u };
    // SPIR-V access is opt-out
    ResourceAccess Access{ ResourceAccess::ReadWrite };
    bool Block{ false };
    spv::Op OpCode{ spv::Op::OpNop };
    uint32_t StorageClass{ 0u };
    uint32_t PointeeType{ 0u };
    uint32_t TypeId{ 0u };
    ResourceShape Shape{ ResourceShape::Invalid };
    // OpTypeImage "Sampled": 0 decided at runtime, 1 sampled, 2 storage
    uint8_t SampledMode{ 0u };
    uint32_t SampledType{ 0u };
    uint32_t Format{ 0u };
    uint32_t FirstMember{ 0u };
    uint32_t MemberCount{ 0u };
    uint32_t SpecId{ ~0u };
    uint32_t ScalarWidth{ 0u };
    bool ScalarSigned{ false };
    // arrays: element type, and a LengthId of 0 for runtime arrays. Vectors and matrices: component or column type
    uint32_t ElementType{ 0u };
    uint32_t ComponentCount{ 0u };
    uint32_t LengthId{ 0u };
    uint32_t ArrayStride{ 0u };
    ConstantKind Constant{ ConstantKind::None };
    uint64_t ConstantBits{ 0u };
    // the source of a load, access chain, extract, or copy, which the pair tracing follows
    uint32_t DerivedFrom{ 0u };
    // the indices of an access chain (constant ids) or a composite extract (literals), in `ParseState::Indices`
    uint32_t FirstIndex{ 0u };
    uint32_t IndexCount{ 0u };
    bool LiteralIndices{ false };
    // a Function variable: the value of its one store, or ~0u after a second store
    uint32_t StoredFrom{ 0u };
    // the Location decoration, or ~0u without one
    uint32_t Location{ ~0u };
    bool BuiltIn{ false };
};

struct SampledImageUse
{
    uint32_t ImageId{ 0u };
    uint32_t SamplerId{ 0u };
};

/** One access chain with at least one index. The first index of a chain on a binding array picks the element. */
struct ChainAccess
{
    uint32_t BaseId{ 0u };
    uint32_t FirstIndexId{ 0u };
};

struct ParseState
{
    // names point into these words
    std::vector<uint32_t> Words;
    EntryPointInfo EntryPoint;
    std::vector<IdRecord> Ids;
    std::vector<std::string_view> Capabilities;
    std::vector<std::string_view> Extensions;

    std::vector<PendingMemberDecoration> PendingMembers;
    std::vector<MemberRecord> Members;
    spv::AddressingModel AddressingModel{ spv::AddressingModel::Max };
    std::vector<SampledImageUse> SampledImages;
    std::vector<ChainAccess> ChainAccesses;
    std::vector<uint32_t> Indices;
    // the caller sets it after the parse. A sampled image from this set is reached through a handle
    std::optional<uint32_t> DescriptorHeapSet;
};

/** Where a stored value lives: the container, its block's group and binding (0 and 0 for push constants), the offset. */
struct StoredValueLocation
{
    StoredValueContainer Container{ StoredValueContainer::None };
    uint32_t Group{ 0u };
    uint32_t Binding{ 0u };
    uint32_t ByteOffset{ 0u };
};

struct BlockMember
{
    std::string_view Name;
    uint32_t TypeId{ 0u };
};

/** `Unsupported` marks a form this validator does not read yet. */
struct DeclaredBinding
{
    uint32_t Group{ 0u };
    uint32_t Binding{ 0u };
    BindingKind Kind{ BindingKind::Invalid };
    ResourceShape Shape{ ResourceShape::Invalid };
    ResourceAccess Access{ ResourceAccess::Invalid };
    TextureFormat StorageFormat{ TextureFormat::Invalid };
    uint32_t ArrayCount{ 1u };
    // an OpTypeArray, also of length 1
    bool Array{ false };
    std::string_view Name;
    std::string_view Unsupported;
};

std::vector<DeclaredBinding> CollectDeclaredBindings(const ParseState& parse_state);
/** Returns the pointee struct of the container's variable: a `Block` uniform buffer at the group and binding, or the push
 * constant block. 0 when the code declares none. */
uint32_t FindContainerStruct(const ParseState& parse_state, const StoredValueLocation& container);
/** Returns the member that starts at the offset, through nested structs, or nothing when no member starts there. */
std::optional<BlockMember> FindMemberAtOffset(const ParseState& parse_state, uint32_t struct_type, uint32_t byte_offset);
bool IsHandleType(const ParseState& parse_state, uint32_t type_id) noexcept;
bool IsDevicePointerType(const ParseState& parse_state, uint32_t type_id) noexcept;
/** The device features that a dynamic index into a binding array needs. Slang does not state them as capabilities. The
 * names repeat when several accesses need one. A texel buffer array is not covered. */
std::vector<std::string_view> CollectDynamicIndexingCapabilities(const ParseState& parse_state);
/** `validate` runs spirv-val with the Vulkan 1.2 rules first. The `--no-validate` read path skips it. */
CookError ParseModule(spv_context_t* context,
                      std::span<const std::byte> source_code,
                      bool validate,
                      ParseState& out_state,
                      DiagnosticSink& sink);
/** Fails when an operand does not trace to a decorated variable, since a dropped pair would hide a sampler. */
CookResult<std::vector<SamplerTexturePlacement>> CollectSamplerTexturePairs(const ParseState& parse_state,
                                                                             DiagnosticSink& sink);
/** Returns the end of the last member of the push constant block, or no value without a block. */
CookResult<std::optional<uint32_t>> ReadPushConstantExtent(const ParseState& parse_state, DiagnosticSink& sink);
CookResult<uint32_t> ReadWorkgroupMemoryBytes(const ParseState& parse_state, DiagnosticSink& sink);
/** Returns the vertex inputs that a vertex entry point reads, sorted by location. Other stages return none. A built-in
 * and a variable without a location are not vertex inputs. */
std::vector<EmittedVertexInput> CollectVertexInputs(const ParseState& parse_state);
CookResult<EmittedCodeFacts> CollectEmittedCodeFacts(const ParseState& parse_state, DiagnosticSink& sink);

} // namespace lodestone::spirv

#endif // !LODESTONE_TARGET_SPV_MODULE_READER_HPP
