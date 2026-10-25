#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/OutputSink.hpp"
#include "target/ShimTable.hpp"
#include "target/StorageFormatTable.hpp"
#include "target/TargetProfile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace lodestone;

constexpr std::array<AccessModel, 3u> k_AccessModels{ AccessModel::Bound, AccessModel::Handle, AccessModel::Pointer };

/** `Name<float4, ...>` with the type arguments of the row, or the bare name. */
std::string Spell(std::string_view name, uint32_t type_parameter_count)
{
    std::string spelled{ name };
    for (uint32_t i = 0u; i < type_parameter_count; ++i)
    {
        spelled += i == 0u ? "<float4" : ", float4";
    }
    return type_parameter_count == 0u ? spelled : spelled + ">";
}

/** One generic struct for each cell, constrained to the interface of its row, and an alias that instantiates it with the
 * implementation of that cell. Slang checks the constraint when it loads the module. */
std::string MakeAgreementSource(std::span<const ShimInterface> table)
{
    std::string source = "module ShimAgreement;\nimport LodestoneShims;\n";
    size_t cell = 0u;
    for (const ShimInterface& row : table)
    {
        for (const AccessModel access : k_AccessModels)
        {
            const std::string_view impl = ShimImplName(row, access);
            if (impl.empty())
            {
                continue;
            }

            if (row.ImplementationTakesFormat)
            {
                // one cell for each accepted format
                for (const StorageFormatInfo& format : GetStorageFormats())
                {
                    source += std::format("struct Check{0}<T : {1}> {{}};\ntypealias Cell{0} = Check{0}<{2}<{3}>>;\n",
                                          cell,
                                          row.Name,
                                          impl,
                                          format.SlangType);
                    ++cell;
                }
                continue;
            }

            source += std::format("struct Check{0}<T : {1}> {{}};\ntypealias Cell{0} = Check{0}<{2}>;\n",
                                  cell,
                                  Spell(row.Name, row.TypeParameterCount),
                                  Spell(impl, row.TypeParameterCount));
            ++cell;
        }
    }
    source += "RWStructuredBuffer<float> Output;\n"
              "[shader(\"compute\")][numthreads(1, 1, 1)]\n"
              "void MainCS(uint3 id : SV_DispatchThreadID) { Output[0] = 1.0f; }\n";
    return source;
}

bool CookAgreementModule(std::span<const ShimInterface> table, const std::filesystem::path& directory)
{
    const std::filesystem::path modulePath = directory / "ShimAgreement.slang";
    {
        std::ofstream out{ modulePath, std::ios::binary };
        out << MakeAgreementSource(table);
    }

    CookerOptions options;
    options.ModulePaths.push_back(modulePath);
    options.TargetNames.emplace_back("spirv");
    options.OutputPath = "ShimTable";
    MemoryOutputSink sink;
    return RunCook(options, sink).has_value();
}

} // namespace

int main()
{
    tests::TestRunner runner{ "ShimTable" };

    runner.BeginSection("the table holds the interfaces of the shim module, and the Bound form is complete");
    const std::span<const ShimInterface> table = GetShimInterfaces();
    runner.CheckEqual(table.size(), 5u, "five interfaces: two buffers, a texture, a sampler, and a write-only storage texture");
    bool boundComplete = true;
    for (const ShimInterface& row : table)
    {
        boundComplete = boundComplete && !ShimImplName(row, AccessModel::Bound).empty() && row.Class != ShimResourceClass::Invalid;
    }
    runner.Check(boundComplete, "every interface has a Bound implementation and a resource class");

    const ShimInterface* readBuffer = FindShimInterface("IReadBuffer");
    if (!runner.Check(readBuffer != nullptr, "IReadBuffer is in the table"))
    {
        return runner.Report();
    }
    runner.Check(readBuffer != nullptr && readBuffer->Class == ShimResourceClass::Buffer && readBuffer->TypeParameterCount == 1u &&
                     ShimImplName(*readBuffer, AccessModel::Bound) == "BoundReadBuffer",
                 "an interface is found by name with its class, arity, and Bound implementation");
    // a buffer device address addresses buffers only, and buffers take no descriptor handle
    for (const ShimInterface& row : table)
    {
        const bool buffer = row.Class == ShimResourceClass::Buffer;
        runner.Check(buffer != ShimImplName(row, AccessModel::Pointer).empty() &&
                         buffer == ShimImplName(row, AccessModel::Handle).empty(),
                     std::format("{}: a buffer has a Pointer cell and no Handle cell, and every other class the reverse", row.Name));
    }
    runner.CheckEqual(ShimImplName(*readBuffer, AccessModel::Pointer), "PointerReadBuffer", "IReadBuffer has the Pointer form");
    const ShimInterface* writeStorage = FindShimInterface("IWriteStorageTexture2D");
    runner.Check(writeStorage != nullptr && writeStorage->Class == ShimResourceClass::StorageTexture &&
                     writeStorage->TypeParameterCount == 0u && writeStorage->ImplementationTakesFormat &&
                     !readBuffer->ImplementationTakesFormat,
                 "the write-only storage texture takes a format as its implementation argument, and no other row does");
    const char* notFoundClaim = "a name that is not a shim interface is not found";
    runner.CheckEqual(FindShimInterface("IWriteBuffer"), nullptr, notFoundClaim);
    runner.CheckEqual(FindShimInterface(""), nullptr, notFoundClaim);
    runner.Check(ShimImplName(*readBuffer, AccessModel::Invalid).empty(), "an invalid access model has no cell");

    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "LodestoneShimTableTest";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    runner.BeginSection("every cell names a type that exists, takes the right arity, and conforms (decision D6)");
    runner.Check(CookAgreementModule(table, directory), "the shim module agrees with the table");

    runner.BeginSection("a table that disagrees with the module fails to load, so the comparison can fail");
    std::vector<ShimInterface> missing(table.begin(), table.end());
    missing[0].Bound = "BoundNoSuchBuffer";
    runner.Check(!CookAgreementModule(missing, directory), "a cell that names a type the module lacks");

    std::vector<ShimInterface> nonConforming(table.begin(), table.end());
    nonConforming[1].Bound = "BoundReadBuffer";
    runner.Check(!CookAgreementModule(nonConforming, directory),
                 "a cell whose type has the right arity and does not implement the interface");

    std::vector<ShimInterface> wrongArity(table.begin(), table.end());
    wrongArity[0].TypeParameterCount = 0u;
    runner.Check(!CookAgreementModule(wrongArity, directory), "a row whose arity differs from the interface");

    std::filesystem::remove_all(directory);
    return runner.Report();
}
