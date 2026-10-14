#pragma once

// Writes the two-letter ISO country code of the region set in the operating system into r_code,
// null-terminated. False, with r_code left empty, where the region is unknown.
bool get_home_country_code(char r_code[3]);
