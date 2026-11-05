#include "stdlib_noniso.h"

char* itoa(int value, char* result, int base) { return ltoa(value, result, base); }
char* utoa(unsigned value, char* result, int base) { return ultoa(value, result, base); }
