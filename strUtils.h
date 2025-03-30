#pragma once

#include <string>
#include <algorithm>

inline constexpr void toLower(std::string& s)
{
	std::transform(s.begin(), s.end(), s.begin(),
		[](uint8_t c) { return std::tolower(c); });
}
inline constexpr void toUpper(std::string& s)
{
	std::transform(s.begin(), s.end(), s.begin(),
		[](uint8_t c) { return std::toupper(c); });
}
inline constexpr void trimStart(std::string& s)
{
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](uint8_t c) {
		return !std::isspace(c);
		}));
}
inline constexpr void trimEnd(std::string& s)
{
	s.erase(std::find_if(s.rbegin(), s.rend(), [](uint8_t c) {
		return !std::isspace(c);
		}).base(), s.end());
}
inline constexpr void trim(std::string& s)
{
	trimStart(s);
	trimEnd(s);
}
