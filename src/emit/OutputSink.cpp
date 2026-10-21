#include "emit/OutputSink.hpp"
#include "CookerErrors.hpp"
#include <cstring>
#include <cwchar>
#include <format>
#include <random>
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
    // the throwing overload would end the process here, because the constructor is noexcept. a folder that cannot
    // be made fails the first write instead, with OutputFileOpenFailed
    std::error_code ignored;
    if (!path.empty())
    {
        std::filesystem::create_directories(path, ignored);
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

    // write to a temporary file in the same folder, then rename it into place. a reader sees the old file or the
    // new one, and never a partial write. the suffix is random so that two cooks never share a temporary file
    std::random_device random;
    std::filesystem::path temporaryPath = artifactPath;
    temporaryPath += std::format(".{:08x}{:08x}.tmp", random(), random());
    {
        std::ofstream stream{ temporaryPath, std::ios::binary | std::ios::trunc };
        if (!stream.is_open())
        {
            return CookError::OutputFileOpenFailed;
        }

        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        // close before the check, because the flush on close can fail too
        stream.close();
        if (!stream)
        {
            std::error_code ignored;
            std::filesystem::remove(temporaryPath, ignored);
            return CookError::OutputWriteFailed;
        }
    }

    std::error_code fileError;
    std::filesystem::rename(temporaryPath, artifactPath, fileError);
    if (fileError)
    {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        return CookError::OutputWriteFailed;
    }

    // read the size back from the file system, so that a write that the stream accepted and the disk lost fails
    const std::uintmax_t writtenSize = std::filesystem::file_size(artifactPath, fileError);
    if (fileError || writtenSize != content.size())
    {
        return CookError::OutputWriteFailed;
    }

    return CookError::Success;
}

CookError FileOutputSink::WriteArtifact(std::string_view artifact_name, std::span<const std::byte> content)
{
    // be evil: just cast the span of bytes to a string view and call the other overload
    const std::string_view contentAsStringView{ reinterpret_cast<const char*>(content.data()), content.size() };
    return WriteArtifact(artifact_name, contentAsStringView);
}

CookError FileOutputSink::RemoveArtifact(std::string_view artifact_name)
{
    std::error_code removeError;
    std::filesystem::remove(path / std::filesystem::path{ artifact_name }, removeError);
    return removeError ? CookError::OutputWriteFailed : CookError::Success;
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

CookError MemoryOutputSink::RemoveArtifact(std::string_view artifact_name)
{
    const auto artifact = artifacts.find(std::string{ artifact_name });
    if (artifact != artifacts.end())
    {
        artifacts.erase(artifact);
    }
    return CookError::Success;
}

} // namespace lodestone
