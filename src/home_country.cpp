#include "home_country.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Kept apart from the Godot headers, which clash with windows.h macros
bool get_home_country_code(char r_code[3]) {
	r_code[0] = '\0';
#ifdef _WIN32
	// the "Country or region" the user picked in Windows settings, not the display language
	wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
	// regions without a two-letter code come back as a three-digit UN code, which isn't a country
	if (GetUserDefaultGeoName(name, LOCALE_NAME_MAX_LENGTH) != 3) { return false; }
	r_code[0] = static_cast<char>(name[0]);
	r_code[1] = static_cast<char>(name[1]);
	r_code[2] = '\0';
	return true;
#else
	return false;
#endif
}
