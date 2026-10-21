#pragma once
#ifndef LODESTONE_TARGET_SPV_MODULE_READER_HPP
#define LODESTONE_TARGET_SPV_MODULE_READER_HPP
#include "CookerErrors.hpp"
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"
#include "target/TargetProfile.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "spirv-tools/libspirv.h"
#define SPV_ENABLE_UTILITY_CODE
#include "spirv/unified1/spirv.hpp11"

/** Reads one SPIR-V module with SPIRV-Tools: the parse into id records, the bindings that the module declares,
 * and the sampler and texture pairs that it samples. `SpvValidator` compares what this reads against
 * reflection. This header names SPIRV-Tools and SPIR-V types, so it sits beside its source in
 * `src/target/impl/` and never in `include/`. */
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
    // Variables attached to the entry point. From SPIR-V 1.4 this lists every global the entry point
    // uses, in every storage class. It is not sorted.
    std::vector<uint32_t> Interfaces;
    bool LocalSizeFromIds{ false };
    // stores the LocalSize directly, or the IDs of the size constants otherwise
    uint32_t LocalSizeX{ 0u };
    uint32_t LocalSizeY{ 0u };
    uint32_t LocalSizeZ{ 0u };
};

struct MemberRecord
{
    uint32_t TypeId{ 0u };
    uint32_t Offset{ ~0u };
    uint32_t MatrixStride{ 0u };
    // The SPIR-V decoration, as written. Slang writes ColMajor for what its reflection calls RowMajor
    // (measured on KsGeometry ViewProjection, 2026-09-26), so a comparison must invert it.
    MatrixLayout MatrixMajor{ MatrixLayout::Invalid };
    std::string_view Name;
};

struct PendingMemberDecoration
{
    uint32_t StructId{ 0u };
    uint32_t MemberIndex{ 0u };
    // Max marks a member name (OpMemberName) rather than a decoration
    spv::Decoration Decoration{ spv::Decoration::Max };
    std::span<const uint32_t> Values;
    // names are read from the debug section, which is before
    // the actual structs, so we have to also store the name here temporarily.
    std::string_view Name;
};

/** How an id defines its value. Only `Spec` records with a SpecId are pipeline inputs. `SpecDerived`
 * values are expressions over other constants (OpSpecConstantOp, OpSpecConstantComposite), so they
 * have no value here. */
enum class ConstantKind : uint8_t
{
    None = 0,
    Constant,
    Spec,
    SpecDerived,
};

/** Initially called this resource info bc I thought we'd be able to go right into
  * that mapping concept, but really we need to track individual SPIR-V ID records.
  * This won't become a concept we could recognize as a resource until after parse */
struct IdRecord
{
    std::string_view Name;
    uint32_t Binding{ ~0u };
    uint32_t Group{ ~0u };
    // assume RW by default, because of SPIR-V's behavior being "opt out" focused
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
    // OpTypeInt / OpTypeFloat
    uint32_t ScalarWidth{ 0u };
    bool ScalarSigned{ false };
    // OpTypeArray / OpTypeRuntimeArray. LengthId is the id of a constant, and 0 for a runtime array.
    // OpTypeVector and OpTypeMatrix use ElementType for the component or column type, and ComponentCount
    uint32_t ElementType{ 0u };
    uint32_t ComponentCount{ 0u };
    uint32_t LengthId{ 0u };
    uint32_t ArrayStride{ 0u };
    ConstantKind Constant{ ConstantKind::None };
    // raw bits: the type record says how to read them
    uint64_t ConstantBits{ 0u };
    // OpLoad, OpAccessChain, OpInBoundsAccessChain, OpCopyObject: the id this value came from. The sampler
    // and texture pairs follow it back to the resource variable
    uint32_t DerivedFrom{ 0u };
};

/** The image and sampler operands of one OpSampledImage, as ids inside a function. */
struct SampledImageUse
{
    uint32_t ImageId{ 0u };
    uint32_t SamplerId{ 0u };
};

struct ParseState
{
    // have to persist a copy of the SPIR-V words, because all the string views
    // for names, caps, extensions etc, point into this vector
    std::vector<uint32_t> Words;
    // in our cooker, we only allow one entry point per SPIR-V module
    EntryPointInfo EntryPoint;
    std::vector<IdRecord> Ids;
    std::vector<std::string_view> Capabilities;
    std::vector<std::string_view> Extensions;

    // Members can't be fully associated until parse is completed
    std::vector<PendingMemberDecoration> PendingMembers;
    std::vector<MemberRecord> Members;
    spv::AddressingModel AddressingModel{ spv::AddressingModel::Max };
    std::vector<SampledImageUse> SampledImages;
};

/** One resource that the SPIR-V declares for the entry point, classified from its variable, pointer,
 * and pointee records. `Unsupported` marks a form that this validator does not read yet. */
struct DeclaredBinding
{
    uint32_t Group{ 0u };
    uint32_t Binding{ 0u };
    BindingKind Kind{ BindingKind::Invalid };
    ResourceShape Shape{ ResourceShape::Invalid };
    ResourceAccess Access{ ResourceAccess::Invalid };
    // set only for a storage image. A format outside the curated set reads as Invalid, the same as reflection
    TextureFormat StorageFormat{ TextureFormat::Invalid };
    std::string_view Name;
    std::string_view Unsupported;
};

std::vector<DeclaredBinding> CollectDeclaredBindings(const ParseState& parse_state);

/** Checks the word count, parses the module into `out_state`, and then attaches each member decoration to its
 * struct. `validate` runs spirv-val first, with the Vulkan 1.2 rules: the read path for `--no-validate` skips
 * that legality check. */
CookError ParseModule(spv_context_t* context,
                      std::span<const std::byte> source_code,
                      bool validate,
                      ParseState& out_state,
                      DiagnosticSink& sink);

/** Follows each OpSampledImage operand back to its resource variable, and returns the placements. A value
 * that does not come from a decorated variable (a function parameter, OpPhi, or OpSelect) fails, because a
 * pair that the cook drops would make a sampler look unpaired. */
CookResult<std::vector<SamplerTexturePlacement>> CollectSamplerTexturePairs(const ParseState& parse_state,
                                                                             DiagnosticSink& sink);

/** Returns the bytes that the module's push constant block covers: the end of its last member, with no trailing
 * padding, from the member offsets and the type sizes. No value when the module declares no push constant variable.
 * A member type that this reader cannot size fails. */
CookResult<std::optional<uint32_t>> ReadPushConstantExtent(const ParseState& parse_state, DiagnosticSink& sink);

/** The sampler and texture pairs and the push constant extent of one module. */
CookResult<EmittedCodeFacts> CollectEmittedCodeFacts(const ParseState& parse_state, DiagnosticSink& sink);

} // namespace lodestone::spirv

#endif // !LODESTONE_TARGET_SPV_MODULE_READER_HPP
