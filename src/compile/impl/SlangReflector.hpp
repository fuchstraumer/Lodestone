#pragma once
#ifndef LODESTONE_SLANG_REFLECTOR_HPP
#define LODESTONE_SLANG_REFLECTOR_HPP
#include "CookerErrors.hpp"
#include "compile/RawLibrary.hpp"

namespace lodestone
{

class SlangReflector
{
public:
    // Since we need to propagate errors/results, ctor is just defaulted
    SlangReflector() noexcept = default;
    ~SlangReflector() noexcept = default;
    // this thing works like a local one-shot invocation of reflection, so copying 
    // shouldn't happen, but better safe than sorry
    SlangReflector(const SlangReflector&) noexcept = delete;
    SlangReflector& operator=(const SlangReflector&) noexcept = delete;

    CookResult<RawVariant> Reflect();
private:

};

}

#endif // !LODESTONE_SLANG_REFLECTOR_HPP
