#pragma once
#include <string>
#include <string_view>

namespace me {
/// Case-insensitive lowercasing: ASCII fast-path + CoreFoundation Unicode fallback.
std::string toLower(std::string_view s);
}
