#pragma once
#include <Windows.h>
#include <stdexcept>  // ← ЭТОГО НЕ ХВАТАЛО!
#include <string>
#include <cstdio>     // для sprintf_s

inline void ThrowIfFailed(HRESULT hr, const char* msg = "")
{
    if (FAILED(hr))
    {
        char buf[256];
        sprintf_s(buf, "HRESULT=0x%08X  %s", (unsigned)hr, msg);
        throw std::runtime_error(buf);
    }
}