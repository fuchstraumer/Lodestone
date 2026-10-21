#include "SpvModuleReader.hpp"
#include "ShaderLibraryTypes.hpp"
#include "target/TargetProfile.hpp"
#include "target/TargetUtils.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "model/ShaderDataSchema.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
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
        // every id is below id_bound, so a flat table indexed by id replaces a map
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
        // The extension name is stored as a null-terminated string starting at words[1]
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
        // execution model is in words[1]. The cook sets VulkanUseEntryPointName, so the name is the source name and
        // not "main"
        EntryPointInfo entryPoint;
        entryPoint.Stage = StageFromExecutionModel(static_cast<spv::ExecutionModel>(words[1]));
        entryPoint.Name = reinterpret_cast<const char*>(words.data() + operands[2].offset);

        // actual operands start at 3: 0-2 are exec model, function id, name string
        for (size_t i = 3u; i < operands.size(); ++i)
        {
            entryPoint.Interfaces.emplace_back(words[operands[i].offset]);
        }

        // LocalSize arrives later (execution modes follow entry points), so nothing is lost here
        parse_state.EntryPoint = std::move(entryPoint);
    }

    void OnOpName(ParseState& parse_state,
                  std::span<const uint32_t> words)
    {
        // strings attached to instruction words are always null-terminated
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
            // we'll use memberTypeId to associate these to the pending records at the end of parse
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
        // OpMemberName is in the debug section, before OpTypeStruct gives the struct its member range.
        // Defer it like a member decoration.
        const char* name = reinterpret_cast<const char*>(words.data() + 3u);
        parse_state.PendingMembers.push_back(PendingMemberDecoration{ .StructId = words[1],
                                                                      .MemberIndex = words[2],
                                                                      .Name = name });
    }

    void OnOpTypePointer(ParseState& parse_state,
                         std::span<const uint32_t> words,
                         uint32_t result_id)
    {
        // words[2] is the storage class
        // words[3] is the type ID
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
            // not valid in Vulkan. The shape stays Invalid, so the comparison reports it.
            [[fallthrough]];
        case spv::Dim::Buffer:
            // a texel buffer (BindingKind::TexelBuffer). No asset uses one yet.
            [[fallthrough]];
        default:
            break;
        }

        // 0,2 indicate non depth or unknown, and unknown is quite common
        // 1 is the only guaranteed "this is definitely a depth texture" value
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
        // words[9], the access qualifier, is Kernel-only. A Vulkan storage image takes its access from
        // NonWritable / NonReadable on the variable instead.
    }

    /** Maps the curated formats that `FromSlangImageFormat` maps, so that the two sides compare. Any other format,
     * `Unknown` included, is Invalid on both sides. */
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
        // OpTypeInt: words[2] width, words[3] signedness. OpTypeFloat: words[2] width (words[3] is an
        // optional encoding, not a sign).
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
        // the prelude already stored TypeId (the pointer type)
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
            // later pass will have to resolve the values using the IDs: this opcode comes before
            // any of the actual spec constants
            parse_state.EntryPoint.LocalSizeFromIds = true;
            parse_state.EntryPoint.LocalSizeX = words[3];
            parse_state.EntryPoint.LocalSizeY = words[4];
            parse_state.EntryPoint.LocalSizeZ = words[5];
        }
    }

    void OnOpMemoryModel(ParseState& parse_state, std::span<const uint32_t> words)
    {
        // logical = bound, physicalstoragebuffer64 = pointers. The memory model (words[2]) is GLSL450 from
        // Slang, which is normal for Vulkan: the Vulkan memory model is opt-in.
        parse_state.AddressingModel = static_cast<spv::AddressingModel>(words[1]);
    }

    /** OpConstant and OpSpecConstant: words[1] type, words[2] result, words[3..] the value. One word up to 32
     * bits, two words (low word first) for 64 bits. The True/False forms have no words[3]: the opcode is
     * the value. */
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
        // id 0 is never valid: it means "no result"
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
            // an expression over other constants: no value and no SpecId. SPIRV-Tools-opt can fold these
            // (CreateFreezeSpecConstantValuePass, CreateFoldSpecConstantOpAndCompositePass) once we need
            // the numbers
            parseState->Ids[resultId].Constant = ConstantKind::SpecDerived;
            break;
        case spv::Op::OpLoad:
        case spv::Op::OpAccessChain:
        case spv::Op::OpInBoundsAccessChain:
        case spv::Op::OpCopyObject:
            // words[3] is the pointer, the base, or the copied value
            parseState->Ids[resultId].DerivedFrom = words[3];
            break;
        case spv::Op::OpSampledImage:
            // the function bodies are parsed for this: words[3] is the image, and words[4] the sampler
            parseState->SampledImages.push_back(SampledImageUse{ .ImageId = words[3], .SamplerId = words[4] });
            break;
        default:
            // for unhandled opcodes, just continue parsing
            return SPV_SUCCESS;
        }

        return SPV_SUCCESS;
    }

    /** Runs once after the parse. Each struct now has its member range, so each pending entry indexes its
     * member directly. */
    void ApplyPendingMembers(ParseState& parse_state)
    {
        for (const PendingMemberDecoration& pending : parse_state.PendingMembers)
        {
            const IdRecord& structRecord = parse_state.Ids[pending.StructId];
            if (pending.MemberIndex >= structRecord.MemberCount)
            {
                // a member of a struct the parse never reached (it stops at the first function)
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

    /** Classifies one interface variable: variable -> pointer type -> pointee type. The storage class sits
     * on the pointer. Only a variable with a DescriptorSet is a binding: builtins, varyings, and Workgroup
     * variables have none. */
    DeclaredBinding ClassifyVariable(const ParseState& parse_state, const IdRecord& variable)
    {
        DeclaredBinding declared{ .Group = variable.Group, .Binding = variable.Binding, .Name = variable.Name };
        const IdRecord& pointer = parse_state.Ids[variable.TypeId];
        const IdRecord& pointee = parse_state.Ids[pointer.PointeeType];

        if ((pointee.OpCode == spv::Op::OpTypeArray) || (pointee.OpCode == spv::Op::OpTypeRuntimeArray))
        {
            declared.Unsupported = "a binding array, which this validator does not read yet";
            return declared;
        }

        switch (static_cast<spv::StorageClass>(pointer.StorageClass))
        {
        case spv::StorageClass::UniformConstant:
            switch (pointee.OpCode)
            {
            case spv::Op::OpTypeSampler:
                declared.Kind = BindingKind::Sampler;
                break;
            case spv::Op::OpTypeSampledImage:
                declared.Kind = BindingKind::CombinedTextureSampler;
                break;
            case spv::Op::OpTypeImage:
                // Sampled 2 is a storage image, and its access comes from the variable's decorations
                declared.Kind = (pointee.SampledMode == 2u) ? BindingKind::StorageTexture : BindingKind::Texture;
                declared.Shape = pointee.Shape;
                declared.Access = variable.Access;
                if (declared.Kind == BindingKind::StorageTexture)
                {
                    declared.StorageFormat = TextureFormatFromSpirv(static_cast<spv::ImageFormat>(pointee.Format));
                }
                break;
            default:
                declared.Unsupported = "a UniformConstant variable of a type this validator does not read";
                break;
            }
            break;
        case spv::StorageClass::StorageBuffer:
            // NonWritable on the variable gives read-only (StructuredBuffer), none gives read-write
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

    /** Follows `id` back through loads, access chains, and copies. Returns the id of the variable, or 0 when
     * the chain ends at something that is not a variable. The chain is at most one step per id, so the step
     * count bounds the walk on a malformed module. */
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

    // A copy, so no alignment of the source bytes is assumed. The state owns it: names point into these words
    out_state.Words.resize(source_code.size() / sizeof(uint32_t));
    std::memcpy(out_state.Words.data(), source_code.data(), source_code.size());
    const std::vector<uint32_t>& words = out_state.Words;

    spv_diagnostic diagnostic = nullptr;
    if (validate)
    {
        // Legality first: spirv-val with the Vulkan 1.2 rules. Tint gives the WGSL side this for free.
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
    /** The bytes one value of `type_id` covers, with no trailing padding. A matrix member reads its stride and its
     * major order from the member decorations. SPIR-V `ColMajor` puts a column at each stride, and `RowMajor` a
     * row. No value for a type that this reader cannot size. */
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
            // ElementType is the column vector, so its component count is the row count
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
        default:
            return std::nullopt;
        }
    }
    // NOLINTEND(misc-no-recursion)

} // namespace

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

    EmittedCodeFacts facts{ .SamplerTexturePairs = std::move(*pairs), .PushConstantExtent = *pushExtent };
    facts.EntryPointName = parse_state.EntryPoint.Name;
    for (const DeclaredBinding& binding : CollectDeclaredBindings(parse_state))
    {
        facts.DeclaredPlacements.emplace_back(binding.Group, binding.Binding);
    }
    return facts;
}

} // namespace lodestone::spirv
