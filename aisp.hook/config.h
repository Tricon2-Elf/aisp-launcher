// aisp.hook.ini next to the game executable, read with the classic profile API. Every setting
// also has an environment variable of the same meaning, which wins when set, so a one-off
// override never needs the file edited. See aisp.hook.ini in the repository for the keys.
#pragma once

#include <cstddef>

namespace aisp
{
// The value of `variable` from the environment if set and non-empty, else [section] key from
// the ini if present and non-empty. Returns false when neither has a value; `out` is then
// empty. `variable` may be nullptr for settings without an environment form.
bool ConfigString(const wchar_t* variable, const wchar_t* section, const wchar_t* key, wchar_t* out, size_t outCount);

// A three-way switch: 1 / on / yes / true, 0 / off / no / false, or anything else (auto, or
// no value at all).
enum class Switch
{
    Auto,
    Off,
    On,
};
Switch ConfigSwitch(const wchar_t* variable, const wchar_t* section, const wchar_t* key);
} // namespace aisp
