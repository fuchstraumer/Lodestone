#include "SpvModuleReader.hpp"
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"
#include "VulkanCapabilityTable.hpp"
#include "spirv-tools/libspirv.h"
#include "spirv/unified1/spirv.hpp11"
#include "target/TargetProfile.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <magic_enum/magic_enum.hpp>

namespace lodestone::spirv
{

namespace
{
    spv_result_t OnHeader(void* user_data,
                          spv_endianness_t endianness,
                          uint32_t magic,
                          uint32_t version,
                          uint32_t generator,
                          uint32_t id_bound,
                          uint32_t schema);

    spv_result_t OnInstruction(void* user_data,
                               const spv_parsed_instruction_t* inst);

    void ApplyPendingMembers(ParseState& parse_state);

    spv_result_t OnHeader(void* user_data,
                          spv_endianness_t endianness,
                          uint32_t magic,
                          uint32_t version,
                          uint32_t generator,
                          uint32_t id_bound,
                          uint32_t schema)
    {
        // ids are below id_bound, so a flat table replaces a map
        ParseState* parseState = static_cast<ParseState*>(user_data);
        parseState->Ids.resize(id_bound);
        return SPV_SUCCESS;
    }

    void OnOpCapability(ParseState& parse_state,
                        std::span<const uint32_t> words)
    {
        parse_state.Capabilities.emplace_back(
            spv::CapabilityToString(static_cast<spv::Capability>(words[1])));
    }

    void OnOpExtension(ParseState& parse_state, std::span<const uint32_t> words)
    {
        const char* extensionName = reinterpret_cast<const char*>(&words[1]);
        parse_state.Extensions.emplace_back(extensionName);
    }

    ShaderStageKind StageFromExecutionModel(spv::ExecutionModel exec_model) noexcept
    {
        switch (exec_model)
        {
        case spv::ExecutionModel::Vertex:
            return ShaderStageKind::Vertex;
        case spv::ExecutionModel::TessellationControl:
            return ShaderStageKind::TessellationControl;
        case spv::ExecutionModel::TessellationEvaluation:
            return ShaderStageKind::TessellationEvaluation;
        case spv::ExecutionModel::Geometry:
            return ShaderStageKind::Geometry;
        case spv::ExecutionModel::Fragment:
            return ShaderStageKind::Fragment;
        case spv::ExecutionModel::GLCompute:
            return ShaderStageKind::Compute;
        case spv::ExecutionModel::TaskNV:
        case spv::ExecutionModel::TaskEXT:
            return ShaderStageKind::Task;
        case spv::ExecutionModel::MeshNV:
        case spv::ExecutionModel::MeshEXT:
            return ShaderStageKind::Mesh;
        case spv::ExecutionModel::RayGenerationKHR:
            return ShaderStageKind::RayGeneration;
        case spv::ExecutionModel::IntersectionKHR:
            return ShaderStageKind::Intersection;
        case spv::ExecutionModel::AnyHitKHR:
            return ShaderStageKind::AnyHit;
        case spv::ExecutionModel::ClosestHitKHR:
            return ShaderStageKind::ClosestHit;
        case spv::ExecutionModel::MissKHR:
            return ShaderStageKind::Miss;
        case spv::ExecutionModel::CallableKHR:
            return ShaderStageKind::Callable;
        default:
            return ShaderStageKind::Invalid;
        }
    }

    void OnOpEntryPoint(ParseState& parse_state,
                        std::span<const uint32_t> words,
                        std::span<const spv_parsed_operand_t> operands)
    {
        EntryPointInfo entryPoint;
        entryPoint.Stage = StageFromExecutionModel(static_cast<spv::ExecutionModel>(words[1]));
        entryPoint.Name = reinterpret_cast<const char*>(words.data() + operands[2].offset);

        // operands 0-2 are the execution model, the function id, and the name
        for (size_t i = 3u; i < operands.size(); ++i)
        {
            entryPoint.Interfaces.emplace_back(words[operands[i].offset]);
        }

        parse_state.EntryPoint = std::move(entryPoint);
    }

    void OnOpName(ParseState& parse_state,
                  std::span<const uint32_t> words)
    {
        const uint32_t* nameWords = words.data() + 2;
        parse_state.Ids[words[1]].Name = reinterpret_cast<const char*>(nameWords);
    }

    void OnOpTypeStruct(ParseState& parse_state,
                        std::span<const uint32_t> words,
                        uint32_t result_id)
    {
        IdRecord& record = parse_state.Ids[result_id];
        record.FirstMember = static_cast<uint32_t>(parse_state.Members.size());
        record.MemberCount = static_cast<uint32_t>(words.size() - 2u);
        for (const uint32_t memberTypeId : words.subspan(2u))
        {
            parse_state.Members.emplace_back(memberTypeId);
        }
    }

    void OnOpDecorate(ParseState& parse_state, std::span<const uint32_t> words)
    {
        const spv::Decoration opDec = static_cast<spv::Decoration>(words[2]);
        switch (opDec)
        {
        case spv::Decoration::DescriptorSet:
            parse_state.Ids[words[1]].Group = words[3];
            break;
        case spv::Decoration::Binding:
            parse_state.Ids[words[1]].Binding = words[3];
            break;
        case spv::Decoration::NonWritable:
            parse_state.Ids[words[1]].Access = ResourceAccess::ReadOnly;
            break;
        case spv::Decoration::NonReadable:
            parse_state.Ids[words[1]].Access = ResourceAccess::WriteOnly;
            break;
        case spv::Decoration::Block:
            parse_state.Ids[words[1]].Block = true;
            break;
        case spv::Decoration::SpecId:
            parse_state.Ids[words[1]].SpecId = words[3];
            break;
        case spv::Decoration::ArrayStride:
            parse_state.Ids[words[1]].ArrayStride = words[3];
            break;
        default:
            break;
        }
    }

    void OnOpMemberDecorate(ParseState& parse_state, std::span<const uint32_t> words)
    {
        parse_state.PendingMembers.push_back(PendingMemberDecoration{ .StructId = words[1],
                                                                      .MemberIndex = words[2],
                                                                      .Decoration = static_cast<spv::Decoration>(words[3]),
                                                                      .Values = words.subspan(4u) });
    }

    void OnOpMemberName(ParseState& parse_state,
                        std::span<const uint32_t> words)
    {
        // OpMemberName precedes OpTypeStruct, so it is deferred like a member decoration
        const char* name = reinterpret_cast<const char*>(words.data() + 3u);
        parse_state.PendingMembers.push_back(PendingMemberDecoration{ .StructId = words[1],
                                                                      .MemberIndex = words[2],
                                                                      .Name = name });
    }

    void OnOpTypePointer(ParseState& parse_state,
                         std::span<const uint32_t> words,
                         uint32_t result_id)
    {
        // words[2] is the storage class, words[3] the type id
        parse_state.Ids[result_id].StorageClass = words[2];
        parse_state.Ids[result_id].PointeeType = words[3];
    }

    void OnOpTypeImage(ParseState& parse_state,
                       std::span<const uint32_t> words,
                       uint32_t result_id)
    {
        IdRecord& record = parse_state.Ids[result_id];
        record.SampledType = words[2];
        const spv::Dim imageDim = static_cast<spv::Dim>(words[3]);
        switch (imageDim)
        {
        case spv::Dim::Dim1D:
            record.Shape |= ResourceShape::Texture1D;
            break;
        case spv::Dim::Dim2D:
            record.Shape |= ResourceShape::Texture2D;
            break;
        case spv::Dim::Dim3D:
            record.Shape |= ResourceShape::Texture3D;
            break;
        case spv::Dim::Cube:
            record.Shape |= ResourceShape::TextureCube;
            break;
        case spv::Dim::SubpassData:
            record.Shape |= ResourceShape::TextureSubpass;
            break;
        case spv::Dim::Rect:
            [[fallthrough]];
        case spv::Dim::Buffer:
            [[fallthrough]];
        default:
            break;
        }

        // only 1 means depth: 0 and 2 are "not depth" and "unknown"
        if (words[4] == 1u)
        {
            record.Shape |= ResourceShape::ShadowFlag;
        }

        if (static_cast<bool>(words[5]))
        {
            record.Shape |= ResourceShape::ArrayFlag;
        }

        if (static_cast<bool>(words[6]))
        {
            record.Shape |= ResourceShape::MultisampleFlag;
        }

        record.SampledMode = static_cast<uint8_t>(words[7]);
        record.Format = words[8];
        // the access qualifier is Kernel-only: Vulkan storage images use NonWritable / NonReadable
    }

    /** Maps the curated set of `FromSlangImageFormat`. Other formats, `Unknown` included, are Invalid on both sides. */
    TextureFormat TextureFormatFromSpirv(spv::ImageFormat format) noexcept
    {
        switch (format)
        {
        case spv::ImageFormat::R8:
            return TextureFormat::R8Unorm;
        case spv::ImageFormat::Rg8:
            return TextureFormat::RG8Unorm;
        case spv::ImageFormat::Rgba8:
            return TextureFormat::RGBA8Unorm;
        case spv::ImageFormat::Rgba8Snorm:
            return TextureFormat::RGBA8Snorm;
        case spv::ImageFormat::R16f:
            return TextureFormat::R16Float;
        case spv::ImageFormat::Rg16f:
            return TextureFormat::RG16Float;
        case spv::ImageFormat::Rgba16f:
            return TextureFormat::RGBA16Float;
        case spv::ImageFormat::R32f:
            return TextureFormat::R32Float;
        case spv::ImageFormat::Rg32f:
            return TextureFormat::RG32Float;
        case spv::ImageFormat::Rgba32f:
            return TextureFormat::RGBA32Float;
        case spv::ImageFormat::R32ui:
            return TextureFormat::R32Uint;
        case spv::ImageFormat::Rg32ui:
            return TextureFormat::RG32Uint;
        case spv::ImageFormat::Rgba32ui:
            return TextureFormat::RGBA32Uint;
        case spv::ImageFormat::Rgb10A2:
            return TextureFormat::R10G10B10A2Unorm;
        case spv::ImageFormat::R11fG11fB10f:
            return TextureFormat::R11G11B10Ufloat;
        default:
            return TextureFormat::Invalid;
        }
    }

    void OnOpTypeScalar(ParseState& parse_state, std::span<const uint32_t> words, uint32_t result_id)
    {
        // words[2] is the width. words[3] is signedness for OpTypeInt, and an optional encoding for OpTypeFloat
        IdRecord& record = parse_state.Ids[result_id];
        record.ScalarWidth = words[2];
        record.ScalarSigned = (record.OpCode == spv::Op::OpTypeInt) && (words[3] != 0u);
    }

    void OnOpTypeArray(ParseState& parse_state, std::span<const uint32_t> words, uint32_t result_id)
    {
        // words[2] element type, words[3] the id of the length constant (OpTypeArray only)
        IdRecord& record = parse_state.Ids[result_id];
        record.ElementType = words[2];
        record.LengthId = words.size() > 3u ? words[3] : 0u;
    }

    void OnOpTypeComposite(ParseState& parse_state, std::span<const uint32_t> words, uint32_t result_id)
    {
        // OpTypeVector: words[2] component type, words[3] count. OpTypeMatrix: words[2] column type, words[3] count
        IdRecord& record = parse_state.Ids[result_id];
        record.ElementType = words[2];
        record.ComponentCount = words[3];
    }

    void OnOpVariable(ParseState& parse_state, std::span<const uint32_t> words, uint32_t result_id)
    {
        parse_state.Ids[result_id].StorageClass = words[3];
    }

    void OnOpExecutionMode(ParseState& parse_state, std::span<const uint32_t> words)
    {
        const spv::ExecutionMode executionMode = static_cast<spv::ExecutionMode>(words[2]);
        if ((executionMode == spv::ExecutionMode::LocalSize) && (words.size() >= 6))
        {
            parse_state.EntryPoint.LocalSizeX = words[3];
            parse_state.EntryPoint.LocalSizeY = words[4];
            parse_state.EntryPoint.LocalSizeZ = words[5];
        }
    }

    void OnOpExecutionModeId(ParseState& parse_state, std::span<const uint32_t> words)
    {
        if (words.size() < 6u)
        {
            return;
        }

        const spv::ExecutionMode executionMode = static_cast<spv::ExecutionMode>(words[2]);
        if (executionMode == spv::ExecutionMode::LocalSizeId)
        {
            // the spec constants come later, so the ids are resolved after the parse
            parse_state.EntryPoint.LocalSizeFromIds = true;
            parse_state.EntryPoint.LocalSizeX = words[3];
            parse_state.EntryPoint.LocalSizeY = words[4];
            parse_state.EntryPoint.LocalSizeZ = words[5];
        }
    }

    void OnOpMemoryModel(ParseState& parse_state, std::span<const uint32_t> words)
    {
        // Logical is bound, PhysicalStorageBuffer64 is pointers
        parse_state.AddressingModel = static_cast<spv::AddressingModel>(words[1]);
    }

    /** words[3..] is the value, low word first for 64 bits. The True/False forms have no value word. */
    void OnOpConstant(ParseState& parse_state,
                      std::span<const uint32_t> words,
                      uint32_t result_id,
                      ConstantKind kind)
    {
        IdRecord& record = parse_state.Ids[result_id];
        record.Constant = kind;
        switch (record.OpCode)
        {
        case spv::Op::OpConstantTrue:
        case spv::Op::OpSpecConstantTrue:
            record.ConstantBits = 1u;
            return;
        case spv::Op::OpConstantFalse:
        case spv::Op::OpSpecConstantFalse:
            record.ConstantBits = 0u;
            return;
        default:
            break;
        }

        if (words.size() > 3u)
        {
            record.ConstantBits = words[3];
        }

        if (words.size() > 4u)
        {
            record.ConstantBits |= uint64_t{ words[4] } << 32u;
        }
    }

    spv_result_t OnInstruction(void* user_data,
                               const spv_parsed_instruction_t* inst)
    {
        ParseState* parseState = static_cast<ParseState*>(user_data);
        std::span<const uint32_t> words{ inst->words, inst->num_words };
        std::span<const spv_parsed_operand_t> operands{ inst->operands, inst->num_operands };
        const spv::Op opCode = static_cast<spv::Op>(inst->opcode);
        const uint32_t resultId = inst->result_id;
        if (resultId != 0u)
        {
            parseState->Ids[resultId].OpCode = opCode;
            parseState->Ids[resultId].TypeId = inst->type_id;
        }

        switch (opCode)
        {
        case spv::Op::OpCapability:
            OnOpCapability(*parseState, words);
            break;
        case spv::Op::OpExtension:
            OnOpExtension(*parseState, words);
            break;
        case spv::Op::OpEntryPoint:
            OnOpEntryPoint(*parseState, words, operands);
            break;
        case spv::Op::OpName:
            OnOpName(*parseState, words);
            break;
        case spv::Op::OpTypeStruct:
            OnOpTypeStruct(*parseState, words, resultId);
            break;
        case spv::Op::OpDecorate:
            OnOpDecorate(*parseState, words);
            break;
        case spv::Op::OpMemberDecorate:
            OnOpMemberDecorate(*parseState, words);
            break;
        case spv::Op::OpMemberName:
            OnOpMemberName(*parseState, words);
            break;
        case spv::Op::OpTypePointer:
            OnOpTypePointer(*parseState, words, resultId);
            break;
        case spv::Op::OpTypeImage:
            OnOpTypeImage(*parseState, words, resultId);
            break;
        case spv::Op::OpTypeInt:
        case spv::Op::OpTypeFloat:
            OnOpTypeScalar(*parseState, words, resultId);
            break;
        case spv::Op::OpTypeArray:
        case spv::Op::OpTypeRuntimeArray:
            OnOpTypeArray(*parseState, words, resultId);
            break;
        case spv::Op::OpTypeVector:
        case spv::Op::OpTypeMatrix:
            OnOpTypeComposite(*parseState, words, resultId);
            break;
        case spv::Op::OpVariable:
            OnOpVariable(*parseState, words, resultId);
            break;
        case spv::Op::OpExecutionMode:
            OnOpExecutionMode(*parseState, words);
            break;
        case spv::Op::OpExecutionModeId: // for execution modes that reference an ID
            OnOpExecutionModeId(*parseState, words);
            break;
        case spv::Op::OpMemoryModel:
            OnOpMemoryModel(*parseState, words);
            break;
        case spv::Op::OpConstant:
        case spv::Op::OpConstantTrue:
        case spv::Op::OpConstantFalse:
            OnOpConstant(*parseState, words, resultId, ConstantKind::Constant);
            break;
        case spv::Op::OpSpecConstant:
        case spv::Op::OpSpecConstantTrue:
        case spv::Op::OpSpecConstantFalse:
            OnOpConstant(*parseState, words, resultId, ConstantKind::Spec);
            break;
        case spv::Op::OpSpecConstantOp:
        case spv::Op::OpSpecConstantComposite:
            // an expression over constants, with no value and no SpecId
            parseState->Ids[resultId].Constant = ConstantKind::SpecDerived;
            break;
        case spv::Op::OpAccessChain:
        case spv::Op::OpInBoundsAccessChain:
            if (words.size() > 4u)
            {
                parseState->ChainAccesses.push_back(ChainAccess{ .BaseId = words[3], .FirstIndexId = words[4] });
            }
            parseState->Ids[resultId].DerivedFrom = words[3];
            break;
        case spv::Op::OpLoad:
        case spv::Op::OpCopyObject:
            // words[3] is the pointer, or the copied value
            parseState->Ids[resultId].DerivedFrom = words[3];
            break;
        case spv::Op::OpSampledImage:
            // the function bodies are parsed for this: words[3] is the image, and words[4] the sampler
            parseState->SampledImages.push_back(SampledImageUse{ .ImageId = words[3], .SamplerId = words[4] });
            break;
        default:
            return SPV_SUCCESS;
        }

        return SPV_SUCCESS;
    }

    void ApplyPendingMembers(ParseState& parse_state)
    {
        for (const PendingMemberDecoration& pending : parse_state.PendingMembers)
        {
            const IdRecord& structRecord = parse_state.Ids[pending.StructId];
            if (pending.MemberIndex >= structRecord.MemberCount)
            {
                continue;
            }

            MemberRecord& member = parse_state.Members[structRecord.FirstMember + pending.MemberIndex];
            switch (pending.Decoration)
            {
            case spv::Decoration::Max:
                member.Name = pending.Name;
                break;
            case spv::Decoration::Offset:
                member.Offset = pending.Values[0];
                break;
            case spv::Decoration::MatrixStride:
                member.MatrixStride = pending.Values[0];
                break;
            case spv::Decoration::RowMajor:
                member.MatrixMajor = MatrixLayout::RowMajor;
                break;
            case spv::Decoration::ColMajor:
                member.MatrixMajor = MatrixLayout::ColumnMajor;
                break;
            default:
                break;
            }
        }
    }

    /** Classifies a variable through its pointer type. Only variables with a DescriptorSet are bindings. */
    DeclaredBinding ClassifyVariable(const ParseState& parse_state, const IdRecord& variable)
    {
        DeclaredBinding declared{ .Group = variable.Group, .Binding = variable.Binding, .Name = variable.Name };
        const IdRecord& pointer = parse_state.Ids[variable.TypeId];
        const IdRecord& pointee = parse_state.Ids[pointer.PointeeType];

        // a binding array is one OpTypeArray or OpTypeRuntimeArray of the resource type. Vulkan allows no array of arrays
        // here, and spirv-val rejects it before we get this far
        const IdRecord* element = &pointee;
        if (pointee.OpCode == spv::Op::OpTypeRuntimeArray)
        {
            declared.ArrayCount = 0u;
            declared.Array = true;
            element = &parse_state.Ids[pointee.ElementType];
        }
        else if (pointee.OpCode == spv::Op::OpTypeArray)
        {
            const IdRecord& length = parse_state.Ids[pointee.LengthId];
            if (length.Constant != ConstantKind::Constant ||
                length.ConstantBits == 0u ||
                length.ConstantBits > std::numeric_limits<uint32_t>::max())
            {
                declared.Unsupported = "a binding array whose length is not a constant";
                return declared;
            }
            declared.ArrayCount = static_cast<uint32_t>(length.ConstantBits);
            declared.Array = true;
            element = &parse_state.Ids[pointee.ElementType];
        }

        switch (static_cast<spv::StorageClass>(pointer.StorageClass))
        {
        case spv::StorageClass::UniformConstant:
            switch (element->OpCode)
            {
            case spv::Op::OpTypeSampler:
                declared.Kind = BindingKind::Sampler;
                break;
            case spv::Op::OpTypeSampledImage:
                declared.Kind = BindingKind::CombinedTextureSampler;
                break;
            case spv::Op::OpTypeImage:
                declared.Kind = (element->SampledMode == 2u) ? BindingKind::StorageTexture : BindingKind::Texture;
                declared.Shape = element->Shape;
                declared.Access = variable.Access;
                if (declared.Kind == BindingKind::StorageTexture)
                {
                    declared.StorageFormat = TextureFormatFromSpirv(static_cast<spv::ImageFormat>(element->Format));
                }
                break;
            default:
                declared.Unsupported = "a UniformConstant variable of a type this validator does not read";
                break;
            }
            break;
        case spv::StorageClass::StorageBuffer:
            declared.Kind = BindingKind::StorageBuffer;
            declared.Access = variable.Access;
            break;
        case spv::StorageClass::Uniform:
            declared.Kind = BindingKind::UniformBuffer;
            break;
        default:
            declared.Unsupported = "a storage class this validator does not read";
            break;
        }

        return declared;
    }

    /** Returns the variable at the end of a chain of loads, access chains, and copies, or 0. One step per id bounds
     * the walk on a malformed module. */
    uint32_t TraceToVariable(const ParseState& parse_state, uint32_t id)
    {
        for (size_t step = 0u; step < parse_state.Ids.size() && id != 0u && id < parse_state.Ids.size(); ++step)
        {
            const IdRecord& record = parse_state.Ids[id];
            if (record.OpCode == spv::Op::OpVariable)
            {
                return id;
            }
            id = record.DerivedFrom;
        }
        return 0u;
    }

} // namespace

CookError ParseModule(spv_context_t* context,
                      std::span<const std::byte> source_code,
                      bool validate,
                      ParseState& out_state,
                      DiagnosticSink& sink)
{
    if (source_code.empty() || (source_code.size() % sizeof(uint32_t)) != 0u)
    {
        const std::string errStr =
            std::format("the SPIR-V entry point is {} bytes, which is not whole words", source_code.size());
        return ReportError(sink, CookError::TargetValidationEntryPointParseFailed, errStr);
    }

    // a copy: names point into these words, and the source alignment is unknown
    out_state.Words.resize(source_code.size() / sizeof(uint32_t));
    std::memcpy(out_state.Words.data(), source_code.data(), source_code.size());
    const std::vector<uint32_t>& words = out_state.Words;

    spv_diagnostic diagnostic = nullptr;
    if (validate)
    {
        const spv_result_t validResult = spvValidateBinary(context, words.data(), words.size(), &diagnostic);
        if (validResult != SPV_SUCCESS)
        {
            std::string errStr = std::format("spirv-val rejects the entry point: {}",
                                             (diagnostic != nullptr) ? diagnostic->error : "no diagnostic");
            spvDiagnosticDestroy(diagnostic);
            return ReportError(sink, CookError::TargetValidationEntryPointInvalid, std::move(errStr));
        }
        spvDiagnosticDestroy(diagnostic);
        diagnostic = nullptr;
    }

    const spv_result_t parsed = spvBinaryParse(context,
                                               &out_state,
                                               words.data(),
                                               words.size(),
                                               &OnHeader,
                                               &OnInstruction,
                                               &diagnostic);
    if (parsed != SPV_SUCCESS)
    {
        std::string errStr = std::format("the SPIR-V parse failed: {}",
                                         (diagnostic != nullptr) ? diagnostic->error : "no diagnostic");
        spvDiagnosticDestroy(diagnostic);
        return ReportError(sink, CookError::TargetValidationEntryPointParseFailed, std::move(errStr));
    }
    spvDiagnosticDestroy(diagnostic);
    ApplyPendingMembers(out_state);
    return CookError::Success;
}

CookResult<std::vector<SamplerTexturePlacement>> CollectSamplerTexturePairs(const ParseState& parse_state,
                                                                             DiagnosticSink& sink)
{
    std::vector<SamplerTexturePlacement> pairs;
    pairs.reserve(parse_state.SampledImages.size());
    for (const SampledImageUse& use : parse_state.SampledImages)
    {
        const uint32_t imageVariable = TraceToVariable(parse_state, use.ImageId);
        const uint32_t samplerVariable = TraceToVariable(parse_state, use.SamplerId);
        const bool imageBound = imageVariable != 0u && parse_state.Ids[imageVariable].Group != ~0u;
        const bool samplerBound = samplerVariable != 0u && parse_state.Ids[samplerVariable].Group != ~0u;
        if (!imageBound || !samplerBound)
        {
            const std::string errStr =
                std::format("an OpSampledImage of image %{} and sampler %{} does not trace back to two bound "
                            "resource variables, so the pair cannot be recorded",
                            use.ImageId,
                            use.SamplerId);
            return std::unexpected(ReportError(sink, CookError::ReflectionSamplerPairUnresolved, errStr));
        }

        const IdRecord& image = parse_state.Ids[imageVariable];
        const IdRecord& sampler = parse_state.Ids[samplerVariable];
        pairs.push_back(SamplerTexturePlacement{ .TextureGroup = image.Group,
                                                 .TextureBinding = image.Binding,
                                                 .SamplerGroup = sampler.Group,
                                                 .SamplerBinding = sampler.Binding });
    }
    return pairs;
}

std::vector<std::string_view> CollectDynamicIndexingCapabilities(const ParseState& parse_state)
{
    std::vector<std::string_view> names;
    for (const ChainAccess& access : parse_state.ChainAccesses)
    {
        const IdRecord& base = parse_state.Ids[access.BaseId];
        if (base.OpCode != spv::Op::OpVariable || base.Group == ~0u)
        {
            continue;
        }

        const IdRecord& pointee = parse_state.Ids[parse_state.Ids[base.TypeId].PointeeType];
        if (pointee.OpCode != spv::Op::OpTypeArray && pointee.OpCode != spv::Op::OpTypeRuntimeArray)
        {
            continue;
        }

        // an index that is a constant or a specialization constant is fixed when the pipeline is built
        const IdRecord& index = parse_state.Ids[access.FirstIndexId];
        if (index.Constant != ConstantKind::None)
        {
            continue;
        }

        // Slang states none of these capabilities, and Vulkan needs a device feature for each
        const IdRecord& element = parse_state.Ids[pointee.ElementType];
        switch (static_cast<spv::StorageClass>(parse_state.Ids[base.TypeId].StorageClass))
        {
        case spv::StorageClass::UniformConstant:
            if (element.OpCode == spv::Op::OpTypeImage && element.SampledMode == 2u)
            {
                names.emplace_back("StorageImageArrayDynamicIndexing");
            }
            else
            {
                // sampled images, samplers, and combined image samplers
                names.emplace_back("SampledImageArrayDynamicIndexing");
            }
            break;
        case spv::StorageClass::StorageBuffer:
            names.emplace_back("StorageBufferArrayDynamicIndexing");
            break;
        case spv::StorageClass::Uniform:
            names.emplace_back("UniformBufferArrayDynamicIndexing");
            break;
        default:
            break;
        }
    }

    return names;
}

std::vector<DeclaredBinding> CollectDeclaredBindings(const ParseState& parse_state)
{
    std::vector<DeclaredBinding> declared;
    declared.reserve(parse_state.EntryPoint.Interfaces.size());
    for (const uint32_t variableId : parse_state.EntryPoint.Interfaces)
    {
        const IdRecord& variable = parse_state.Ids[variableId];
        if (variable.Group == ~0u)
        {
            continue;
        }

        declared.push_back(ClassifyVariable(parse_state, variable));
    }

    return declared;
}

namespace
{
    // NOLINTBEGIN(misc-no-recursion)
    /** The bytes a value covers, with no trailing padding. SPIR-V `ColMajor` puts a column at each matrix stride. */
    std::optional<uint64_t> TypeExtent(const ParseState& parse_state,
                                       uint32_t type_id,
                                       uint32_t matrix_stride,
                                       MatrixLayout matrix_major)
    {
        const IdRecord& type = parse_state.Ids[type_id];
        switch (type.OpCode)
        {
        case spv::Op::OpTypeInt:
        case spv::Op::OpTypeFloat:
            return type.ScalarWidth / 8u;
        case spv::Op::OpTypeVector:
        {
            const std::optional<uint64_t> component = TypeExtent(parse_state, type.ElementType, 0u, MatrixLayout::Invalid);
            return component ? std::optional{ *component * type.ComponentCount } : std::nullopt;
        }
        case spv::Op::OpTypeMatrix:
        {
            const IdRecord& column = parse_state.Ids[type.ElementType];
            const std::optional<uint64_t> scalar = TypeExtent(parse_state, column.ElementType, 0u, MatrixLayout::Invalid);
            if (!scalar || matrix_stride == 0u)
            {
                return std::nullopt;
            }
            const bool rowMajor = matrix_major == MatrixLayout::RowMajor;
            const uint64_t strided = rowMajor ? column.ComponentCount : type.ComponentCount;
            const uint64_t packed = rowMajor ? type.ComponentCount : column.ComponentCount;
            return (strided - 1u) * matrix_stride + packed * *scalar;
        }
        case spv::Op::OpTypeArray:
        {
            const uint64_t length = parse_state.Ids[type.LengthId].ConstantBits;
            const std::optional<uint64_t> element = TypeExtent(parse_state, type.ElementType, matrix_stride, matrix_major);
            if (!element || length == 0u || type.ArrayStride == 0u)
            {
                return std::nullopt;
            }
            return (length - 1u) * type.ArrayStride + *element;
        }
        case spv::Op::OpTypeStruct:
        {
            uint64_t extent = 0u;
            for (const MemberRecord& member : std::span{ parse_state.Members }.subspan(type.FirstMember, type.MemberCount))
            {
                const std::optional<uint64_t> memberExtent =
                    TypeExtent(parse_state, member.TypeId, member.MatrixStride, member.MatrixMajor);
                if (!memberExtent || member.Offset == ~0u)
                {
                    return std::nullopt;
                }
                extent = std::max(extent, member.Offset + *memberExtent);
            }
            return extent;
        }
        case spv::Op::OpTypePointer:
            // a buffer device address, which the Pointer form of a shim stores
            return type.StorageClass == static_cast<uint32_t>(spv::StorageClass::PhysicalStorageBuffer)
                       ? std::optional<uint64_t>{ 8u }
                       : std::nullopt;
        default:
            return std::nullopt;
        }
    }
    // NOLINTEND(misc-no-recursion)

    struct Std430Layout
    {
        uint64_t Size{ 0u };
        uint64_t Align{ 1u };
    };

    constexpr uint64_t RoundUp(uint64_t alignment, uint64_t value) noexcept
    {
        return (value + alignment - 1u) / alignment * alignment;
    }

    // NOLINTBEGIN(misc-no-recursion)
    /** Std430, for undecorated variables. Matrices are column major, and booleans count as 32 bits, per Vulkan. */
    std::optional<Std430Layout> LayOutStd430(const ParseState& parse_state, uint32_t type_id)
    {
        const IdRecord& type = parse_state.Ids[type_id];
        switch (type.OpCode)
        {
        case spv::Op::OpTypeBool:
            return Std430Layout{ .Size = 4u, .Align = 4u };
        case spv::Op::OpTypeInt:
        case spv::Op::OpTypeFloat:
            return Std430Layout{ .Size = type.ScalarWidth / 8u, .Align = type.ScalarWidth / 8u };
        case spv::Op::OpTypeVector:
        {
            const std::optional<Std430Layout> component = LayOutStd430(parse_state, type.ElementType);
            if (!component)
            {
                return std::nullopt;
            }
            const uint64_t alignedCount = type.ComponentCount == 3u ? 4u : type.ComponentCount;
            return Std430Layout{ .Size = component->Size * type.ComponentCount, .Align = component->Size * alignedCount };
        }
        case spv::Op::OpTypeMatrix:
        case spv::Op::OpTypeArray:
        {
            const std::optional<Std430Layout> element = LayOutStd430(parse_state, type.ElementType);
            const uint64_t length = type.OpCode == spv::Op::OpTypeMatrix ? type.ComponentCount
                                                                          : parse_state.Ids[type.LengthId].ConstantBits;
            if (!element || length == 0u)
            {
                return std::nullopt;
            }
            return Std430Layout{ .Size = length * RoundUp(element->Align, element->Size), .Align = element->Align };
        }
        case spv::Op::OpTypeStruct:
        {
            Std430Layout layout;
            for (const MemberRecord& member : std::span{ parse_state.Members }.subspan(type.FirstMember, type.MemberCount))
            {
                const std::optional<Std430Layout> memberLayout = LayOutStd430(parse_state, member.TypeId);
                if (!memberLayout)
                {
                    return std::nullopt;
                }
                layout.Size = RoundUp(memberLayout->Align, layout.Size) + memberLayout->Size;
                layout.Align = std::max(layout.Align, memberLayout->Align);
            }
            layout.Size = RoundUp(layout.Align, layout.Size);
            return layout;
        }
        default:
            return std::nullopt;
        }
    }
    // NOLINTEND(misc-no-recursion)

} // namespace

CookResult<uint32_t> ReadWorkgroupMemoryBytes(const ParseState& parse_state, DiagnosticSink& sink)
{
    uint64_t bytes = 0u;
    for (const uint32_t variableId : parse_state.EntryPoint.Interfaces)
    {
        const IdRecord& variable = parse_state.Ids[variableId];
        if (variable.OpCode != spv::Op::OpVariable ||
            variable.StorageClass != static_cast<uint32_t>(spv::StorageClass::Workgroup))
        {
            continue;
        }

        const std::optional<Std430Layout> layout =
            LayOutStd430(parse_state, parse_state.Ids[variable.TypeId].PointeeType);
        if (!layout)
        {
            return std::unexpected(ReportError(sink,
                                               CookError::TargetValidationEntryPointInvalid,
                                               std::format("workgroup variable '{}' has a type that the SPIR-V reader "
                                                           "cannot size",
                                                           variable.Name)));
        }
        bytes += RoundUp(16u, RoundUp(layout->Align, layout->Size));
    }
    return static_cast<uint32_t>(bytes);
}

CookResult<std::optional<uint32_t>> ReadPushConstantExtent(const ParseState& parse_state, DiagnosticSink& sink)
{
    for (const IdRecord& variable : parse_state.Ids)
    {
        if (variable.OpCode != spv::Op::OpVariable ||
            variable.StorageClass != static_cast<uint32_t>(spv::StorageClass::PushConstant))
        {
            continue;
        }

        const uint32_t blockType = parse_state.Ids[variable.TypeId].PointeeType;
        const std::optional<uint64_t> extent = TypeExtent(parse_state, blockType, 0u, MatrixLayout::Invalid);
        if (!extent)
        {
            return std::unexpected(ReportError(sink,
                                               CookError::TargetValidationEntryPointInvalid,
                                               std::format("push constant block '{}' contains a type that the SPIR-V "
                                                           "reader cannot size",
                                                           variable.Name)));
        }
        return static_cast<uint32_t>(*extent);
    }

    return std::optional<uint32_t>{};
}

CookResult<EmittedCodeFacts> CollectEmittedCodeFacts(const ParseState& parse_state, DiagnosticSink& sink)
{
    CookResult<std::vector<SamplerTexturePlacement>> pairs = CollectSamplerTexturePairs(parse_state, sink);
    if (!pairs)
    {
        return std::unexpected(pairs.error());
    }

    CookResult<std::optional<uint32_t>> pushExtent = ReadPushConstantExtent(parse_state, sink);
    if (!pushExtent)
    {
        return std::unexpected(pushExtent.error());
    }

    CookResult<uint32_t> workgroupBytes = ReadWorkgroupMemoryBytes(parse_state, sink);
    if (!workgroupBytes)
    {
        return std::unexpected(workgroupBytes.error());
    }

    EmittedCodeFacts facts{ .SamplerTexturePairs = std::move(*pairs), .PushConstantExtent = *pushExtent };
    facts.EntryPointName = parse_state.EntryPoint.Name;
    for (const DeclaredBinding& binding : CollectDeclaredBindings(parse_state))
    {
        facts.DeclaredPlacements.emplace_back(binding.Group, binding.Binding);
    }
    facts.CapabilityNames.reserve(parse_state.Capabilities.size() + parse_state.Extensions.size());
    for (const std::string_view name : CollectDynamicIndexingCapabilities(parse_state))
    {
        facts.CapabilityNames.emplace_back(name);
    }
    // store primary names, which vk.xml uses (SPIRV-Headers may give `StorageUniform16`)
    for (const std::string_view name : parse_state.Capabilities)
    {
        facts.CapabilityNames.emplace_back(vulkan::PrimaryCapabilityName(name));
    }
    for (const std::string_view name : parse_state.Extensions)
    {
        facts.CapabilityNames.emplace_back(name);
    }
    std::ranges::sort(facts.CapabilityNames);
    const auto repeated = std::ranges::unique(facts.CapabilityNames);
    facts.CapabilityNames.erase(repeated.begin(), repeated.end());
    facts.WorkgroupMemoryBytes = *workgroupBytes;
    return facts;
}

} // namespace lodestone::spirv
