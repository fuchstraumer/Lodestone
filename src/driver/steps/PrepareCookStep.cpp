#include "driver/steps/PrepareCookStep.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "driver/CookerOptions.hpp"
#include "driver/CookerSteps.hpp"
#include "permute/PolicyDocument.hpp"
#include "target/TargetProfile.hpp"
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace lodestone
{

CookError ErrorCodeToCookError(std::error_code errc)
{
    std::error_condition condition = errc.default_error_condition();
    if (condition == std::errc::no_such_file_or_directory)
    {
        return CookError::DirectoryDoesNotExist;
    }
    else if (condition == std::errc::permission_denied)
    {
        return CookError::PermissionDenied;
    }
    else if (condition == std::errc::file_exists)
    {
        return CookError::FileWriteFailed;
    }
    else if (condition == std::errc::invalid_argument ||
             condition == std::errc::filename_too_long ||
             condition == std::errc::illegal_byte_sequence)
    {
        return CookError::InvalidPath;
    }
    // i have no idea what this is beyond Filesystem Machine Broke bro
    return CookError::SystemError;
}

CookResult<PreparedCook> PrepareCookStep::operator()(CookerOptions&& input) const
{
    SharedCookState result;
    result.Options = std::move(input);
    // build the diagnostics sink
    result.Diagnostics = std::make_unique<StderrDiagnosticSink>();

    // gonna reuse this for a few steps, whenever we touch the filesystem (the third rail)
    std::error_code filesystemError;

    if (result.Options.ModuleCacheDirectory.empty())
    {
        result.Options.ModuleCacheDirectory = DefaultModuleCacheDirectory();
    }

    if (!std::filesystem::exists(result.Options.ModuleCacheDirectory, filesystemError))
    {
        if (filesystemError)
        {
            return std::unexpected(ErrorCodeToCookError(filesystemError));
        }
        std::filesystem::create_directories(result.Options.ModuleCacheDirectory, filesystemError);
        if (filesystemError)
        {
            return std::unexpected(ErrorCodeToCookError(filesystemError));
        }
    }
    
    if (result.Options.PolicyFile)
    {
        const std::filesystem::path& policyFilePath = *result.Options.PolicyFile;
        if (!std::filesystem::exists(policyFilePath, filesystemError))
        {
            return filesystemError ? std::unexpected(ErrorCodeToCookError(filesystemError)) :
                                     std::unexpected(CookError::DirectoryDoesNotExist);
        }

        // convert to a string because TOML++ takes a string path rather than a std::filesystem::path
        std::string policyFilePathStr = policyFilePath.string();
        PolicyDocResult<PolicyDocument> policyDocResult = PolicyDocument::Load(policyFilePathStr);
        if (!policyDocResult)
        {
            // this will have to be cleaned up before too long, what a mess
            const PolicyParseError& policyError = policyDocResult.error();
            Diagnostic policyDiag
            {
                .Severity=DiagnosticSeverity::Fatal,
                .Code="CookError::PolicyDocumentLoadFailed",
                .File=policyFilePath.string(),
                .Range=
                {
                    .StartLine=static_cast<int32_t>(policyError.Line),
                    .StartColumn=static_cast<int32_t>(policyError.Column)
                },
                .Message=policyError.Message,
                .Context="RunCookOnce",
                .Related={}
            };
            result.Diagnostics->Report(std::move(policyDiag));
            return std::unexpected(CookError::PolicyDocumentLoadFailed);
        }
        // remember, to move properly from expected, dereference the result
        result.Policy = std::move(*policyDocResult);
    }

    // sanity check: do all the module paths exist?
    for (const std::filesystem::path& modulePath : result.Options.ModulePaths)
    {
        if (!std::filesystem::exists(modulePath, filesystemError))
        {
            return filesystemError ? std::unexpected(ErrorCodeToCookError(filesystemError)) :
                                     std::unexpected(CookError::DirectoryDoesNotExist);
        }
        else
        {
            result.AllModuleNames.emplace_back(modulePath.stem().string());
        }
    }

    // now get target profiles for all targets in this cook
    for (const std::string& targetName : result.Options.TargetNames)
    {
        CookResult<TargetProfile> targetResult = FindTargetProfile(targetName);
        if (!targetResult)
        {
            return std::unexpected(CookError::TargetProfileNotFound);
        }
        result.TargetProfiles[targetName] = *targetResult;
    }

    return PreparedCook{ std::move(result) };
}

} // namespace lodestone    
