#include "emit/OutputSink.hpp"
#include "CookerErrors.hpp"
#include <filesystem>
#include <fstream>
#include <ios>
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
    description = path.string();
    primaryName = path.filename().string();
}

FileOutputSink::~FileOutputSink() = default;

CookError FileOutputSink::WriteArtifact(std::string_view artifact_name, std::string_view content)
{
    const std::filesystem::path artifactPath = path.parent_path() / std::filesystem::path{ artifact_name };
    FileOutputSink companion{ artifactPath };
    return companion.Write(content);
}

std::string_view FileOutputSink::PrimaryName() const noexcept
{
    return primaryName;
}

CookError FileOutputSink::Write(std::string_view content)
{
    const std::filesystem::path parentDirectory = path.parent_path();
    if (!parentDirectory.empty() && !std::filesystem::exists(parentDirectory))
    {
        std::filesystem::create_directories(parentDirectory);
    }

    if (!parentDirectory.empty() && !std::filesystem::is_directory(parentDirectory))
    {
        return CookError::OutputPathInvalid;
    }

    std::ofstream stream{ path, std::ios::binary | std::ios::trunc };
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

std::string_view FileOutputSink::Describe() const noexcept
{
    return description;
}

MemoryOutputSink::MemoryOutputSink() :
    MemoryOutputSink{ "ShaderLibrary.hpp" }
{
}

MemoryOutputSink::MemoryOutputSink(std::string_view primary_name) :
    primaryName{ primary_name }
{
}

MemoryOutputSink::~MemoryOutputSink() = default;

CookError MemoryOutputSink::Write(std::string_view new_content)
{
    content.assign(new_content);
    return CookError::Success;
}

CookError MemoryOutputSink::WriteArtifact(std::string_view artifact_name, std::string_view _content)
{
    auto [iter, inserted] = artifacts.try_emplace(std::string{ artifact_name }, std::string{ _content });
    return inserted ? CookError::Success : CookError::ArtifactAlreadyWritten;
}

std::string_view MemoryOutputSink::Describe() const noexcept
{
    return "<memory>";
}

std::string_view MemoryOutputSink::PrimaryName() const noexcept
{
    return primaryName;
}

const std::map<std::string, std::string>& MemoryOutputSink::GetArtifacts() const noexcept
{
    return artifacts;
}

std::string_view MemoryOutputSink::GetContent() const noexcept
{
    return content;
}

} // namespace lodestone
