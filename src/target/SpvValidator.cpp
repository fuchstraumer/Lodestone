#include "target/SpvValidator.hpp"
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
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include <magic_enum/magic_enum.hpp>

#include "spirv-tools/libspirv.h"
#define SPV_ENABLE_UTILITY_CODE
#include "spirv/unified1/spirv.hpp11"

namespace lodestone
{

namespace
{
    struct EntryPointInfo
    {
        ShaderStageKind Stage{ ShaderStageKind::Invalid };
        std::string_view Name;
        // Variables attached to the entry point. From SPIR-V 1.4 this lists every global the entry point
        // uses, in every storage class. It is not sorted.
        std::vector<uint32_t> Interfaces;
        bool LocalSizeFromIds{ false };
        // holds the LocalSize directly, or the IDs of the size constants otherwise
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

    /** How an id holds a value. Only a `Spec` record with a SpecId is a pipeline input. `SpecDerived` is an
     * expression over other constants (OpSpecConstantOp, OpSpecConstantComposite), with no value here. */
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
        // OpTypeArray / OpTypeRuntimeArray. LengthId names a constant, and is 0 for a runtime array.
        uint32_t ElementType{ 0u };
        uint32_t LengthId{ 0u };
        uint32_t ArrayStride{ 0u };
        ConstantKind Constant{ ConstantKind::None };
        // raw bits: the type record says how to read them
        uint64_t ConstantBits{ 0u };
    };

    struct ParseState
    {
        // in our cooker, we only allow one entry point per SPIR-V module
        EntryPointInfo EntryPoint;
        std::vector<IdRecord> Ids;
        std::vector<std::string_view> Capabilities;
        std::vector<std::string_view> Extensions;

        // Members can't be fully associated until parse is completed
        std::vector<PendingMemberDecoration> PendingMembers;
        std::vector<MemberRecord> Members;
        spv::AddressingModel AddressingModel{ spv::AddressingModel::Max };
    };

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

    /** One resource the SPIR-V declares for the entry point, classified from its variable, pointer, and
     * pointee records. `Unsupported` names a form this validator does not read yet. */
    struct DeclaredBinding
    {
        uint32_t Group{ 0u };
        uint32_t Binding{ 0u };
        BindingKind Kind{ BindingKind::Invalid };
        ResourceShape Shape{ ResourceShape::Invalid };
        ResourceAccess Access{ ResourceAccess::Invalid };
        std::string_view Name;
        std::string_view Unsupported;
    };

    std::vector<DeclaredBinding> CollectDeclaredBindings(const ParseState& parse_state);

    BindingComparison CompareBindings(std::span<const DeclaredBinding> declared,
                                      std::span<const ReflectedBinding*> reflected);

}

SpvValidator::SpvValidator() : context{ spvContextCreate(SPV_ENV_VULKAN_1_2) }
{
}

SpvValidator::~SpvValidator()
{
    if (context != nullptr)
    {
        spvContextDestroy(context);
        context = nullptr;
    }
}

CookResult<BindingComparison> SpvValidator::validateEntryPoint(std::span<const std::byte> source_code,
                                                               std::span<const ReflectedBinding*> bindings,
                                                               DiagnosticSink& sink) const
{
    if (source_code.empty() || (source_code.size() % sizeof(uint32_t)) != 0u)
    {
        const std::string errStr =
            std::format("the SPIR-V entry point is {} bytes, which is not whole words", source_code.size());
        return std::unexpected(ReportError(sink, CookError::TargetValidationEntryPointParseFailed, errStr));
    }

    // A copy, so no alignment of the source bytes is assumed
    std::vector<uint32_t> words(source_code.size() / sizeof(uint32_t));
    std::memcpy(words.data(), source_code.data(), source_code.size());

    // Legality first: spirv-val with the Vulkan 1.2 rules. Tint gives the WGSL side this for free.
    spv_diagnostic diagnostic = nullptr;
    const spv_result_t validResult = spvValidateBinary(context, words.data(), words.size(), &diagnostic);
    if (validResult != SPV_SUCCESS)
    {
        std::string errStr = std::format("spirv-val rejects the entry point: {}",
                                         (diagnostic != nullptr) ? diagnostic->error : "no diagnostic");
        spvDiagnosticDestroy(diagnostic);
        return std::unexpected(ReportError(sink, CookError::TargetValidationEntryPointInvalid, std::move(errStr)));
    }
    spvDiagnosticDestroy(diagnostic);
    diagnostic = nullptr;

    ParseState state;
    const spv_result_t parsed = spvBinaryParse(context,
                                               &state,
                                               words.data(),
                                               words.size(),
                                               &OnHeader,
                                               &OnInstruction,
                                               &diagnostic);
    // SPV_REQUESTED_TERMINATION is the stop at the first OpFunction: a success
    if ((parsed != SPV_SUCCESS) && (parsed != SPV_REQUESTED_TERMINATION))
    {
        std::string errStr = std::format("the SPIR-V parse failed: {}",
                                         (diagnostic != nullptr) ? diagnostic->error : "no diagnostic");
        spvDiagnosticDestroy(diagnostic);
        return std::unexpected(ReportError(sink, CookError::TargetValidationEntryPointParseFailed, std::move(errStr)));
    }
    spvDiagnosticDestroy(diagnostic);

    ApplyPendingMembers(state);

    std::vector<DeclaredBinding> declared = CollectDeclaredBindings(state);
    // the interface list is not sorted. `bindings` arrives sorted by (group, binding).
    std::ranges::sort(declared, {}, [](const DeclaredBinding& binding)
    {
        return std::make_tuple(binding.Group, binding.Binding);
    });

    BindingComparison comparison = CompareBindings(declared, bindings);
    comparison.Capabilities.reserve(state.Capabilities.size());
    for (const std::string_view capability : state.Capabilities)
    {
        comparison.Capabilities.emplace_back(capability);
    }

    return comparison;
}

namespace
{
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
        // execution model is in words[1]. Slang names every entry point "main": the original name is on
        // the OpName of the function id.
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
            // (CreateFreezeSpecConstantValuePass, CreateFoldSpecConstantOpAndCompositePass) when phase F
            // needs the numbers.
            parseState->Ids[resultId].Constant = ConstantKind::SpecDerived;
            break;
        case spv::Op::OpFunction:
            // Terminate parsing when a function is encountered, bc everything we care about
            // is before the functions.
            return SPV_REQUESTED_TERMINATION;
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

    /** The shape and the access are compared only where both sides state them. SPIR-V states the shape of
     * an image only. It states the access of a storage buffer and a storage image only: reflection writes
     * ReadOnly on every sampled texture, and SPIR-V has no word for that. */
    bool ShapeIsStated(BindingKind kind) noexcept
    {
        return (kind == BindingKind::Texture) || (kind == BindingKind::StorageTexture);
    }

    bool AccessIsStated(BindingKind kind) noexcept
    {
        return (kind == BindingKind::StorageBuffer) || (kind == BindingKind::StorageTexture);
    }

    /** SPIR-V names a scoped variable with dots (`Surface.Material.Albedo`). Reflection joins a scope with
     * underscores, which is the WGSL spelling (`todo.md` holds the scope question). Set and binding already
     * agree when this runs, so the last segment is enough to find a wrong resource at the right slot. */
    std::string_view LastNameSegment(std::string_view spirv_name) noexcept
    {
        const size_t lastDot = spirv_name.rfind('.');
        return (lastDot == std::string_view::npos) ? spirv_name : spirv_name.substr(lastDot + 1u);
    }

    void CompareOneBinding(const DeclaredBinding& declared,
                           const ReflectedBinding& reflected,
                           BindingComparison& comparison)
    {
        const auto report = [&declared, &comparison](std::string_view what)
        {
            comparison.Matches = false;
            comparison.Report += std::format("  spir-v declares set {} binding {} {} : {}\n",
                                             declared.Group,
                                             declared.Binding,
                                             declared.Name,
                                             what);
        };

        if (!declared.Unsupported.empty())
        {
            report(declared.Unsupported);
            return;
        }

        const std::string_view declaredName = StripSlangNameMangling(LastNameSegment(declared.Name));
        if (declaredName != reflected.Name)
        {
            report(std::format("reflection has mismatched name \"{}\"", reflected.Name));
        }

        if (declared.Kind != reflected.Kind)
        {
            report(std::format("declared as {}, reflection has kind {}", ToString(declared.Kind), ToString(reflected.Kind)));
            return;
        }

        if (ShapeIsStated(declared.Kind) && (declared.Shape != reflected.Shape))
        {
            report(std::format("declared with shape {}, reflection has shape {}",
                               ToString(declared.Shape),
                               ToString(reflected.Shape)));
        }

        if (AccessIsStated(declared.Kind) && (declared.Access != reflected.Access))
        {
            report(std::format("declared with access {}, reflection has access {}",
                               magic_enum::enum_name(declared.Access),
                               magic_enum::enum_name(reflected.Access)));
        }
    }

    /** Both lists are sorted by (set, binding), so one pass of two iterators compares them: O(N + M), the
     * same walk as the WGSL validator. */
    BindingComparison CompareBindings(std::span<const DeclaredBinding> declared,
                                      std::span<const ReflectedBinding*> reflected)
    {
        BindingComparison comparison;
        comparison.Matches = true;
        auto iterDeclared = declared.begin();
        auto iterReflected = reflected.begin();

        while ((iterDeclared != declared.end()) && (iterReflected != reflected.end()))
        {
            const DeclaredBinding& declaredBinding = *iterDeclared;
            const ReflectedBinding& reflectedBinding = **iterReflected;
            const auto declaredTuple = std::make_tuple(declaredBinding.Group, declaredBinding.Binding);
            const auto reflectedTuple = std::make_tuple(GroupOf(reflectedBinding), BindingOf(reflectedBinding));

            if (declaredTuple == reflectedTuple)
            {
                CompareOneBinding(declaredBinding, reflectedBinding, comparison);
                ++iterDeclared;
                ++iterReflected;
            }
            else if (declaredTuple < reflectedTuple)
            {
                comparison.Matches = false;
                comparison.Report += std::format("  spir-v declares set {} binding {} {} : reflection has no binding "
                                                 "at that location\n",
                                                 declaredBinding.Group,
                                                 declaredBinding.Binding,
                                                 declaredBinding.Name);
                ++iterDeclared;
            }
            else
            {
                comparison.Matches = false;
                comparison.Report += std::format("  reflection has set {} binding {} {} : spir-v has no binding "
                                                 "at that location\n",
                                                 std::get<0>(reflectedTuple),
                                                 std::get<1>(reflectedTuple),
                                                 reflectedBinding.Name);
                ++iterReflected;
            }
        }

        for (; iterDeclared != declared.end(); ++iterDeclared)
        {
            comparison.Matches = false;
            comparison.Report += std::format("  spir-v declares set {} binding {} {} : reflection has no binding "
                                             "at that location\n",
                                             iterDeclared->Group,
                                             iterDeclared->Binding,
                                             iterDeclared->Name);
        }

        for (; iterReflected != reflected.end(); ++iterReflected)
        {
            const ReflectedBinding& reflectedBinding = **iterReflected;
            comparison.Matches = false;
            comparison.Report += std::format("  reflection has set {} binding {} {} : spir-v has no binding at that "
                                             "location\n",
                                             GroupOf(reflectedBinding),
                                             BindingOf(reflectedBinding),
                                             reflectedBinding.Name);
        }

        return comparison;
    }
}

}
