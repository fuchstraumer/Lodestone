#include "EmbeddedFileSystem.hpp"
#include "compile/EmbeddedBuiltinModules.hpp"
#include "slang-com-helper.h"
#include "slang.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace lodestone
{

namespace
{
    bool IsSeparator(char character) noexcept;
    bool PathNamesBuiltin(std::string_view path, std::string_view file_name) noexcept;
    bool PathNamesBuiltinDirectory(std::string_view path) noexcept;
    std::filesystem::path PathFromUtf8(const char* path);
    SlangResult CreateStringBlob(std::string_view text, ISlangBlob** out_blob);
    SlangResult CreatePathBlob(const std::filesystem::path& path, ISlangBlob** out_blob);
    SlangResult LoadDiskFile(const char* path, ISlangBlob** out_blob);
    SlangResult GetCanonicalPath(const char* path, ISlangBlob** out_path);
}

SlangResult EmbeddedFileSystem::queryInterface(SlangUUID const& uuid, void** out_object)
{
    if ((uuid == ISlangUnknown::getTypeGuid()) || (uuid == ISlangCastable::getTypeGuid()) ||
        (uuid == ISlangFileSystem::getTypeGuid()) || (uuid == ISlangFileSystemExt::getTypeGuid()))
    {
        *out_object = static_cast<ISlangFileSystemExt*>(this);
        return SLANG_OK;
    }

    *out_object = nullptr;
    return SLANG_E_NO_INTERFACE;
}

uint32_t EmbeddedFileSystem::addRef()
{
    return 1u;
}

uint32_t EmbeddedFileSystem::release()
{
    return 1u;
}

void* EmbeddedFileSystem::castAs(const SlangUUID& guid)
{
    void* object = nullptr;
    return SLANG_SUCCEEDED(queryInterface(guid, &object)) ? object : nullptr;
}

SlangResult EmbeddedFileSystem::loadFile(char const* path, ISlangBlob** out_blob)
{
    const EmbeddedBuiltinModule* builtin = FindEmbeddedBuiltin(path);
    if (builtin != nullptr)
    {
        *out_blob = slang_createBlob(builtin->Source, builtin->SourceSize);
        return SLANG_OK;
    }

    return LoadDiskFile(path, out_blob);
}

SlangResult EmbeddedFileSystem::getFileUniqueIdentity(const char* path, ISlangBlob** out_unique_identity)
{
    return getPath(PathKind::Canonical, path, out_unique_identity);
}

SlangResult EmbeddedFileSystem::calcCombinedPath(SlangPathType from_path_type,
                                                 const char* from_path,
                                                 const char* path,
                                                 ISlangBlob** path_out)
{
    const std::filesystem::path fromPath = PathFromUtf8(from_path);
    const std::filesystem::path directory =
        (from_path_type == SLANG_PATH_TYPE_FILE) ? fromPath.parent_path() : fromPath;
    return CreatePathBlob(directory / PathFromUtf8(path), path_out);
}

SlangResult EmbeddedFileSystem::getPathType(const char* path, SlangPathType* path_type_out)
{
    if (FindEmbeddedBuiltin(path) != nullptr)
    {
        *path_type_out = SLANG_PATH_TYPE_FILE;
        return SLANG_OK;
    }

    if (PathNamesBuiltinDirectory(path))
    {
        *path_type_out = SLANG_PATH_TYPE_DIRECTORY;
        return SLANG_OK;
    }

    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(PathFromUtf8(path), error);
    if (error || !std::filesystem::exists(status))
    {
        return SLANG_E_NOT_FOUND;
    }

    *path_type_out = std::filesystem::is_directory(status) ? SLANG_PATH_TYPE_DIRECTORY : SLANG_PATH_TYPE_FILE;
    return SLANG_OK;
}

SlangResult EmbeddedFileSystem::getPath(PathKind kind, const char* path, ISlangBlob** out_path)
{
    const EmbeddedBuiltinModule* builtin = FindEmbeddedBuiltin(path);
    if (builtin != nullptr)
    {
        std::string builtinPath{ k_BuiltinSearchPath };
        builtinPath += '/';
        builtinPath += builtin->FileName;
        return CreateStringBlob(builtinPath, out_path);
    }

    switch (kind)
    {
    case PathKind::Simplified:
        return CreatePathBlob(PathFromUtf8(path).lexically_normal(), out_path);
    case PathKind::Canonical:
        return GetCanonicalPath(path, out_path);
    case PathKind::Display:
    case PathKind::OperatingSystem:
        return SLANG_SUCCEEDED(GetCanonicalPath(path, out_path))
                   ? SLANG_OK
                   : CreatePathBlob(PathFromUtf8(path).lexically_normal(), out_path);
    default:
        return SLANG_E_NOT_AVAILABLE;
    }
}

void EmbeddedFileSystem::clearCache()
{
}

SlangResult EmbeddedFileSystem::enumeratePathContents(const char* /*path*/,
                                                      FileSystemContentsCallBack /*callback*/,
                                                      void* /*user_data*/)
{
    return SLANG_E_NOT_IMPLEMENTED;
}

OSPathKind EmbeddedFileSystem::getOSPathKind()
{
    return OSPathKind::Direct;
}

EmbeddedFileSystem& GetEmbeddedFileSystem() noexcept
{
    static EmbeddedFileSystem fileSystem;
    return fileSystem;
}

const EmbeddedBuiltinModule* FindEmbeddedBuiltin(std::string_view path) noexcept
{
    for (const EmbeddedBuiltinModule& builtin : k_EmbeddedBuiltinModules)
    {
        if (PathNamesBuiltin(path, builtin.FileName))
        {
            return &builtin;
        }
    }

    return nullptr;
}

namespace
{
    bool IsSeparator(char character) noexcept
    {
        return (character == '/') || (character == '\\');
    }

    bool PathNamesBuiltin(std::string_view path, std::string_view file_name) noexcept
    {
        if (!path.ends_with(file_name))
        {
            return false;
        }

        const std::string_view rest = path.substr(0u, path.size() - file_name.size());
        return !rest.empty() && IsSeparator(rest.back()) && PathNamesBuiltinDirectory(rest.substr(0u, rest.size() - 1u));
    }

    bool PathNamesBuiltinDirectory(std::string_view path) noexcept
    {
        const std::string_view directory = EmbeddedFileSystem::k_BuiltinSearchPath;
        if (!path.ends_with(directory))
        {
            return false;
        }

        const size_t start = path.size() - directory.size();
        return (start == 0u) || IsSeparator(path[start - 1u]);
    }

    std::filesystem::path PathFromUtf8(const char* path)
    {
        return std::filesystem::path{ std::u8string_view{ reinterpret_cast<const char8_t*>(path) } };
    }

    // A string blob holds its terminating zero. Slang reads the text up to that zero.
    SlangResult CreateStringBlob(std::string_view text, ISlangBlob** out_blob)
    {
        std::string terminated{ text };
        terminated.push_back('\0');
        *out_blob = slang_createBlob(terminated.data(), terminated.size());
        return (*out_blob != nullptr) ? SLANG_OK : SLANG_FAIL;
    }

    SlangResult CreatePathBlob(const std::filesystem::path& path, ISlangBlob** out_blob)
    {
        const std::u8string text = path.u8string();
        return CreateStringBlob(std::string_view{ reinterpret_cast<const char*>(text.data()), text.size() }, out_blob);
    }

    SlangResult LoadDiskFile(const char* path, ISlangBlob** out_blob)
    {
        std::ifstream file(PathFromUtf8(path), std::ios::binary);
        if (!file)
        {
            *out_blob = nullptr;
            return SLANG_E_NOT_FOUND;
        }

        // `slang_createBlob` refuses a size of zero, so an empty file is a failed load, not a null blob.
        const std::vector<char> contents{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        *out_blob = contents.empty() ? nullptr : slang_createBlob(contents.data(), contents.size());
        return (*out_blob != nullptr) ? SLANG_OK : SLANG_FAIL;
    }

    SlangResult GetCanonicalPath(const char* path, ISlangBlob** out_path)
    {
        std::error_code error;
        const std::filesystem::path canonical = std::filesystem::canonical(PathFromUtf8(path), error);
        if (error)
        {
            return SLANG_E_NOT_FOUND;
        }

        return CreatePathBlob(canonical, out_path);
    }
}

} // namespace lodestone
