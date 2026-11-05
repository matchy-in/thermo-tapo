#pragma once
#include <cstring>
#define PROGMEM
#define PGM_P const char*
#define PSTR(s) (s)
#define strlen_P strlen
#define strcpy_P strcpy
#define strncpy_P strncpy
#define strcmp_P strcmp
#define memcpy_P memcpy
#define pgm_read_byte(addr) (*reinterpret_cast<const unsigned char*>(addr))
