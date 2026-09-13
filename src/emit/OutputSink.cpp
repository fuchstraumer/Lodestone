#include "emit/OutputSink.hpp"
#include "CookerErrors.hpp"
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace lodestone
{

OutputSink::OutputSink() noexcept = default;
OutputSink::~OutputSink() = default;

FileOutputSink::FileOutputSink(std::filesystem::path _path) :
    path{ std::move(_path) }
{
    if (!path.empty() && !std::filesystem::exists(path))
    {
        std::filesystem::create_directories(path);
    }
}

FileOutputSink::~FileOutputSink() = default;

CookError FileOutputSink::WriteArtifact(std::string_view artifact_name, std::string_view content)
{
    const std::filesystem::path artifactPath = path / std::filesystem::path{ artifact_name };

    std::ofstream stream{ artifactPath, std::ios::binary | std::ios::trunc };
    if (!stream.is_open())
    {
        return CookError::OutputFileOpenFailed;
    }

    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream.good())
    {
        return CookError::OutputWriteFailed;
    }

    return CookError::Success;
}

MemoryOutputSink::MemoryOutputSink() : MemoryOutputSink{ "memory_output_sink" }
{
}

MemoryOutputSink::MemoryOutputSink(std::string_view _name) : name{ _name }
{
}

MemoryOutputSink::~MemoryOutputSink() = default;

CookError MemoryOutputSink::WriteArtifact(std::string_view artifact_name, std::string_view _content)
{
    auto [iter, inserted] = artifacts.try_emplace(std::string{ artifact_name }, std::string{ _content });
    return inserted ? CookError::Success : CookError::ArtifactAlreadyWritten;
}

std::string_view MemoryOutputSink::Describe() const noexcept
{
    return name;
}

const std::map<std::string, std::string>& MemoryOutputSink::GetArtifacts() const noexcept
{
    return artifacts;
}

} // namespace lodestone
