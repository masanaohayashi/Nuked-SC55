#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sc55debug
{
inline bool enabled() noexcept
{
    static const bool value = []() noexcept
    {
#if defined (DEBUG) || defined (_DEBUG)
        constexpr bool debugBuild = true;
#else
        constexpr bool debugBuild = false;
#endif

        const auto* environmentValue = std::getenv ("NUKED_SC55_DEBUG");
        return debugBuild
            || (environmentValue != nullptr
            && *environmentValue != '\0'
            && std::strcmp (environmentValue, "0") != 0
            && std::strcmp (environmentValue, "false") != 0);
    }();

    return value;
}

inline void log (const char* format, ...)
{
    if (! enabled())
        return;

    std::fputs ("[DEBUG-SC55] ", stderr);

    va_list arguments;
    va_start (arguments, format);
    std::vfprintf (stderr, format, arguments);
    va_end (arguments);

    std::fputc ('\n', stderr);
    std::fflush (stderr);
}
}
