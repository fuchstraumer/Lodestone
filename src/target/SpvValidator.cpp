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
#include <expected>
#include <format>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

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
        // Variables attached to the entry point
        std::vector<uint32_t> Interfaces;
    };

    struct ResourceInfo
    {
        std::string_view Name;
        uint32_t Binding{ 0u };
        uint32_t Group{ 0u };
        // assume RW by default, because of SPIR-V's behavior being "opt out" focused
        ResourceAccess Access{ ResourceAccess::ReadWrite };
        bool Block{ false };
        uint32_t OpCode{ 0u };
        uint32_t StorageClass{ 0u };
        uint32_t PointeeType{ 0u };
        uint32_t TypeId{ 0u };
        ResourceShape Shape{ ResourceShape::Invalid };
        uint8_t IsSampled{ 0u };
        uint32_t SampledType{ 0u };
        uint32_t Format{ 0u };
    };

    struct ParseState
    {
        // in our cooker, we only allow one entry point per SPIR-V module
        EntryPointInfo EntryPoints;
        std::vector<ResourceInfo> Resources;
        std::vector<uint32_t> Interfaces;
        std::vector<std::string> Capabilities;
    };

    BindingComparison CompareBindings();

    spv_result_t OnHeader(void* user_data,
                          spv_endianness_t endianness,
                          uint32_t magic,
                          uint32_t version,
                          uint32_t generator,
                          uint32_t id_bound,
                          uint32_t schema);
    
    spv_result_t OnInstruction(void* user_data,
                               const spv_parsed_instruction_t* inst);
                            
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
    // Implementation goes here
    return std::unexpected(CookError::Invalid);
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
        ParseState* parseState = reinterpret_cast<ParseState*>(user_data);
        parseState->Resources.resize(id_bound);
        return SPV_SUCCESS;
    }

    void OnOpEntryPoint(ParseState& parse_state,
                        std::span<const uint32_t> words,
                        std::span<const spv_parsed_operand_t> operands)
    {

        // execution model is in words[1]
        EntryPointInfo entryPoint;
        spv::ExecutionModel execModel = static_cast<spv::ExecutionModel>(words[1]);
        switch (execModel)
        {
        case spv::ExecutionModel::Vertex:
            entryPoint.Stage = ShaderStageKind::Vertex;
            break;
        case spv::ExecutionModel::TessellationControl:
            entryPoint.Stage = ShaderStageKind::TessellationControl;
            break;
        case spv::ExecutionModel::TessellationEvaluation:
            entryPoint.Stage = ShaderStageKind::TessellationEvaluation;
            break;
        case spv::ExecutionModel::Geometry:
            entryPoint.Stage = ShaderStageKind::Geometry;
            break;
        case spv::ExecutionModel::Fragment:
            entryPoint.Stage = ShaderStageKind::Fragment;
            break;
        case spv::ExecutionModel::GLCompute:
            entryPoint.Stage = ShaderStageKind::Compute;
            break;
        case spv::ExecutionModel::TaskNV:
        case spv::ExecutionModel::TaskEXT:
            entryPoint.Stage = ShaderStageKind::Task;
            break;
        case spv::ExecutionModel::MeshNV:
        case spv::ExecutionModel::MeshEXT:
            entryPoint.Stage = ShaderStageKind::Mesh;
            break;
        case spv::ExecutionModel::RayGenerationKHR:
            entryPoint.Stage = ShaderStageKind::RayGeneration;
            break;
        case spv::ExecutionModel::IntersectionKHR:
            entryPoint.Stage = ShaderStageKind::Intersection;
            break;
        case spv::ExecutionModel::AnyHitKHR:
            entryPoint.Stage = ShaderStageKind::AnyHit;
            break;
        case spv::ExecutionModel::ClosestHitKHR:
            entryPoint.Stage = ShaderStageKind::ClosestHit;
            break;
        case spv::ExecutionModel::MissKHR:
            entryPoint.Stage = ShaderStageKind::Miss;
            break;
        case spv::ExecutionModel::CallableKHR:
            entryPoint.Stage = ShaderStageKind::Callable;
            break;
        default:
            entryPoint.Stage = ShaderStageKind::Invalid;
            break;
        }

        for (int16_t i = 0; std::cmp_less(i, operands.size()); ++i)
        {
            parse_state.Interfaces.emplace_back(words[operands[i].offset]);
        }
    }

    void OnOpName(ParseState& parse_state,
                  std::span<const uint32_t> words)
    {
        // strings attached to instruction words are always null-terminated
        const uint32_t* nameWords = words.data() + 2;
        parse_state.Resources[words[1]].Name = reinterpret_cast<const char*>(nameWords);
    }

    void OnOpDecorate(ParseState& parse_state, std::span<const uint32_t> words)
    {
        const spv::Decoration opDec = static_cast<spv::Decoration>(words[2]);
        switch (opDec)
        {
        case spv::Decoration::DescriptorSet:
            parse_state.Resources[words[1]].Group = words[3];
            break;
        case spv::Decoration::Binding:
            parse_state.Resources[words[1]].Binding = words[3];
            break;
        case spv::Decoration::NonWritable:
            parse_state.Resources[words[1]].Access = ResourceAccess::ReadOnly;
            break;
        case spv::Decoration::NonReadable:
            parse_state.Resources[words[1]].Access = ResourceAccess::WriteOnly;
            break;
        case spv::Decoration::Block:
            parse_state.Resources[words[1]].Block = true;
            break;
        default:
            break;
        }
    }

    void OnOpTypePointer(ParseState& parse_state,
                         std::span<const uint32_t> words,
                         uint32_t result_id)
    {
        // Handle OpTypePointer instruction
        // words[2] is the storage class
        // words[3] is the type ID
        parse_state.Resources[result_id].OpCode = static_cast<uint32_t>(spv::Op::OpTypePointer);
        parse_state.Resources[result_id].StorageClass = words[2];
        parse_state.Resources[result_id].PointeeType = words[3];
    }

    void OnOpTypeImage(ParseState& parse_state,
                       std::span<const uint32_t> words,
                       uint32_t result_id)
    {
        parse_state.Resources[result_id].OpCode = static_cast<uint32_t>(spv::Op::OpTypeImage);
        parse_state.Resources[result_id].SampledType = words[2];
        const spv::Dim imageDim = static_cast<spv::Dim>(words[3]);
        switch (imageDim)
        {
        case spv::Dim::Dim1D:
            parse_state.Resources[result_id].Shape |= ResourceShape::Texture1D;
            break;
        case spv::Dim::Dim2D:
            parse_state.Resources[result_id].Shape |= ResourceShape::Texture2D;
            break;
        case spv::Dim::Dim3D:
            parse_state.Resources[result_id].Shape |= ResourceShape::Texture3D;
            break;    
        case spv::Dim::Cube:
            parse_state.Resources[result_id].Shape |= ResourceShape::TextureCube;
            break;
        case spv::Dim::SubpassData:
            parse_state.Resources[result_id].Shape |= ResourceShape::TextureSubpass;
            break;
        case spv::Dim::Rect:
            [[fallthrough]]; // what to do for this one?
        case spv::Dim::Buffer:
            [[fallthrough]];
        default:
            break;
        }

        // 0,2 indicate non depth or unknown, and unknown is quite common
        // 1 is the only guaranteed "this is definitely a depth texture" value
        if (words[4] == 1u)
        {
            parse_state.Resources[result_id].Shape |= ResourceShape::ShadowFlag;
        }

        if (static_cast<bool>(words[5]))
        {
            parse_state.Resources[result_id].Shape |= ResourceShape::ArrayFlag;
        }

        if (static_cast<bool>(words[6]))
        {
            parse_state.Resources[result_id].Shape |= ResourceShape::MultisampleFlag;
        }

        parse_state.Resources[result_id].IsSampled = static_cast<uint8_t>(words[7]);

        parse_state.Resources[result_id].Format = words[8];

        if (words.size() > 9)
        {
            const spv::AccessQualifier accessQualifier = static_cast<spv::AccessQualifier>(words[9]);
            switch (accessQualifier)
            {
            case spv::AccessQualifier::ReadOnly:
                parse_state.Resources[result_id].Access = ResourceAccess::ReadOnly;
                break;
            case spv::AccessQualifier::WriteOnly:
                parse_state.Resources[result_id].Access = ResourceAccess::WriteOnly;
                break;
            case spv::AccessQualifier::ReadWrite:
                parse_state.Resources[result_id].Access = ResourceAccess::ReadWrite;
                break;
            case spv::AccessQualifier::Max:
                break;
            }
        }
    }

    void OnOpVariable(ParseState& parse_state, std::span<const uint32_t> words, uint32_t result_id, uint32_t type_id)
    {
        parse_state.Resources[result_id].TypeId = type_id;
        parse_state.Resources[type_id].StorageClass = words[3];
    }

    spv_result_t OnInstruction(void* user_data,
                               const spv_parsed_instruction_t* inst)
    {
        ParseState* parseState = reinterpret_cast<ParseState*>(user_data);
        std::span<const uint32_t> words{ inst->words, inst->num_words };
        std::span<const spv_parsed_operand_t> operands{ inst->operands, inst->num_operands };
        const spv::Op opCode = static_cast<spv::Op>(inst->opcode);
        switch (opCode)
        {
        case spv::Op::OpCapability:
            parseState->Capabilities.emplace_back(
                spv::CapabilityToString(static_cast<spv::Capability>(words[1])));
            break;
        case spv::Op::OpEntryPoint:
            OnOpEntryPoint(*parseState, words, operands);
            break;
        case spv::Op::OpName:
            OnOpName(*parseState, words);
            break;
        case spv::Op::OpDecorate:
            OnOpDecorate(*parseState, words);
            break;
        case spv::Op::OpTypePointer:
            OnOpTypePointer(*parseState, words, inst->result_id);
            break;
        case spv::Op::OpTypeImage:
            OnOpTypeImage(*parseState, words, inst->result_id);
            break;
        case spv::Op::OpVariable:
            OnOpVariable(*parseState, words, inst->result_id, inst->type_id);
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
}

}
