// Acore::StringFormat over std::format, for the golden check's build of MlpPolicy.cpp alone.
#ifndef ANIMUS_GOLDEN_STUB_STRING_FORMAT_H
#define ANIMUS_GOLDEN_STUB_STRING_FORMAT_H

#include <format>
#include <string>
#include <utility>

namespace Acore
{
    template <typename... Args>
    std::string StringFormat(std::format_string<Args...> fmt, Args&&... args)
    {
        return std::format(fmt, std::forward<Args>(args)...);
    }
}

#endif
