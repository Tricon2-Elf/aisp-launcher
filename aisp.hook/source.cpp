// The source table; see source.h.
#include "source.h"

#include <cwchar>

namespace aisp
{
namespace
{
const ScreenSource kSources[] = {
    {L"streamlink:", RunFfmpegSource},
    {L"stream:", RunFfmpegSource},
    {L"pattern:", RunPatternSource},
};
} // namespace

const ScreenSource* FindSource(const wchar_t* source)
{
    if (!source)
        return nullptr;
    for (const ScreenSource& candidate : kSources)
        if (_wcsnicmp(source, candidate.prefix, std::wcslen(candidate.prefix)) == 0)
            return &candidate;
    return nullptr;
}

bool IsKnownSource(const wchar_t* source)
{
    return FindSource(source) != nullptr;
}
} // namespace aisp
