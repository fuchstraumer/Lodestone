#include "emit/OutputSink.hpp"
#include "CookerErrors.hpp"
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <ios>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace lodestone
{

OutputSink::OutputSink(std::string _name) noexcept : name{ std::move(_name) } {}
OutputSink::~OutputSink() noexcept = default;

std::string_view OutputSink::Describe() const noexcept
{
    return name;
}

FileOutputSink::FileOutputSink(std::filesystem::path _path) noexcept :
    OutputSink{ _path.string() },
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
    // check for length of path
    if (std::wcslen(artifactPath.c_str()) > 255)
    {
        return CookError::OutputPathTooLong;
    }

    // An artifact name can hold a subdirectory, such as the `--dump-sources` folder.
    std::error_code directoryError;
    std::filesystem::create_directories(artifactPath.parent_path(), directoryError);
    if (directoryError)
    {
        return CookError::OutputFileOpenFailed;
    }

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

    stream.close();

    return CookError::Success;
}

CookError FileOutputSink::WriteArtifact(std::string_view artifact_name, std::span<const std::byte> content)
{
    // be evil: just cast the span of bytes to a string view and call the other overload
    const std::string_view contentAsStringView{ reinterpret_cast<const char*>(content.data()), content.size() };
    return WriteArtifact(artifact_name, contentAsStringView);
}

MemoryOutputSink::MemoryOutputSink() : MemoryOutputSink{ "memory_output_sink" }
{
}

MemoryOutputSink::MemoryOutputSink(std::string _name) noexcept : OutputSink{ std::move(_name) }
{
}

MemoryOutputSink::~MemoryOutputSink() = default;

CookError MemoryOutputSink::WriteArtifact(std::string_view artifact_name, std::string_view _content)
{
    auto [iter, inserted] = artifacts.try_emplace(std::string{ artifact_name }, std::string{ _content });
    return inserted ? CookError::Success : CookError::ArtifactAlreadyWritten;
}

CookError MemoryOutputSink::WriteArtifact(std::string_view artifact_name, std::span<const std::byte> content)
{
    const std::string_view contentAsStringView{ reinterpret_cast<const char*>(content.data()), content.size() };
    return WriteArtifact(artifact_name, contentAsStringView);
}

const std::map<std::string, std::string>& MemoryOutputSink::GetArtifacts() const noexcept
{
    return artifacts;
}

} // namespace lodestone
