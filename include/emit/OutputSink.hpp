#pragma once
#ifndef LODESTONE_OUTPUT_SINK_HPP
#define LODESTONE_OUTPUT_SINK_HPP
#include "CookerErrors.hpp"
#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <span>

/** Where cooked output goes. Kept behind an interface so a future watch-and-serve process can hand
 * sources to a running engine without going through the filesystem. */
namespace lodestone
{

class OutputSink
{
public:
    explicit OutputSink(std::string _name) noexcept;
    virtual ~OutputSink() noexcept;
    OutputSink(const OutputSink&) = delete;
    OutputSink& operator=(const OutputSink&) = delete;
    OutputSink(OutputSink&&) noexcept = default;
    OutputSink& operator=(OutputSink&&) noexcept = default;

    /** Writes an artifact to the output sink: this never outputs just one item, it outputs
     * multiple named artifacts. */
    [[nodiscard]] virtual CookError WriteArtifact(std::string_view artifact_name,
                                    std::string_view content) = 0;
    [[nodiscard]] virtual CookError WriteArtifact(std::string_view artifact_name,
                                    std::span<const std::byte> content) = 0;
    [[nodiscard]] std::string_view Describe() const noexcept;
protected:
    std::string name;
};

class FileOutputSink final : public OutputSink
{
public:
    explicit FileOutputSink(std::filesystem::path path) noexcept;
    ~FileOutputSink() override;
    FileOutputSink(FileOutputSink&&) noexcept = default;
    FileOutputSink& operator=(FileOutputSink&&) noexcept = default;

    [[nodiscard]] CookError WriteArtifact(std::string_view artifact_name, std::string_view content) override;
    [[nodiscard]] CookError WriteArtifact(std::string_view artifact_name, std::span<const std::byte> content) override;

private:
    std::filesystem::path path;
};

class MemoryOutputSink final : public OutputSink
{
public:
    MemoryOutputSink();
    explicit MemoryOutputSink(std::string _name) noexcept;
    ~MemoryOutputSink() override;

    [[nodiscard]] CookError WriteArtifact(std::string_view artifact_name, std::string_view content) override;
    [[nodiscard]] CookError WriteArtifact(std::string_view artifact_name, std::span<const std::byte> content) override;
    /** Every companion artifact, keyed by name. The determinism check compares two cooks with it. */
    [[nodiscard]] const std::map<std::string, std::string>& GetArtifacts() const noexcept;

private:
    std::map<std::string, std::string> artifacts;
};

} // namespace lodestone

#endif // !LODESTONE_OUTPUT_SINK_HPP
