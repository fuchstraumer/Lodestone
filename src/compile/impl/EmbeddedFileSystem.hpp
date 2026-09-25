#pragma once
#ifndef LODESTONE_EMBEDDED_FILE_SYSTEM_HPP
#define LODESTONE_EMBEDDED_FILE_SYSTEM_HPP
#include "slang.h"

#include <string_view>

namespace lodestone
{

struct EmbeddedBuiltinModule;

/** @brief A Slang file system that serves the builtin modules from memory, and every other path from disk.
 *
 * Slang finds a builtin the way it finds any imported file: through a search path. That search path is
 * `k_BuiltinSearchPath`, and no directory of that name exists on disk. The import flow therefore stays the
 * one Slang uses for a file. A builtin loaded with `loadModuleFromSourceString` instead broke every worker
 * session: the worker could not load the serialized root module, and Slang reported nothing.
 *
 * This implements the extended interface on purpose. Slang wraps a plain `ISlangFileSystem` in a cache that
 * reports a content hash in place of a path, and `getDependencyFilePath` then names no readable file.
 *
 * The object holds no state, so one instance serves every session on every thread. It lives for the whole
 * process, so its reference count does not change.
 */
class EmbeddedFileSystem final : public ISlangFileSystemExt
{
public:
    static constexpr std::string_view k_BuiltinSearchPath = "lodestone-builtin";

    SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(SlangUUID const& uuid, void** out_object) override;
    SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override;
    SLANG_NO_THROW uint32_t SLANG_MCALL release() override;
    SLANG_NO_THROW void* SLANG_MCALL castAs(const SlangUUID& guid) override;

    SLANG_NO_THROW SlangResult SLANG_MCALL loadFile(char const* path, ISlangBlob** out_blob) override;

    SLANG_NO_THROW SlangResult SLANG_MCALL getFileUniqueIdentity(const char* path,
                                                                ISlangBlob** out_unique_identity) override;
    SLANG_NO_THROW SlangResult SLANG_MCALL calcCombinedPath(SlangPathType from_path_type,
                                                           const char* from_path,
                                                           const char* path,
                                                           ISlangBlob** path_out) override;
    SLANG_NO_THROW SlangResult SLANG_MCALL getPathType(const char* path, SlangPathType* path_type_out) override;
    SLANG_NO_THROW SlangResult SLANG_MCALL getPath(PathKind kind, const char* path, ISlangBlob** out_path) override;
    SLANG_NO_THROW void SLANG_MCALL clearCache() override;
    SLANG_NO_THROW SlangResult SLANG_MCALL enumeratePathContents(const char* path,
                                                                FileSystemContentsCallBack callback,
                                                                void* user_data) override;
    SLANG_NO_THROW OSPathKind SLANG_MCALL getOSPathKind() override;
};

[[nodiscard]] EmbeddedFileSystem& GetEmbeddedFileSystem() noexcept;

/** @brief The builtin module that `path` names, or null. A path names a builtin when it ends with
 * `k_BuiltinSearchPath/<file name>`, with either separator. */
[[nodiscard]] const EmbeddedBuiltinModule* FindEmbeddedBuiltin(std::string_view path) noexcept;

} // namespace lodestone

#endif // !LODESTONE_EMBEDDED_FILE_SYSTEM_HPP
