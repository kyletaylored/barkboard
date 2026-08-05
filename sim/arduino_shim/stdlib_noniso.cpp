// Sim-only desktop implementation of the handful of non-ISO conversion
// functions the vendored WString.cpp calls (declared in stdlib_noniso.h,
// copied verbatim from the ESP32 Arduino core). macOS/BSD libc doesn't ship
// itoa/ultoa/dtostrf etc., so these are reimplemented here in plain C++ —
// simulator-only, never linked into the real ESP32 firmware.
#include "stdlib_noniso.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {

char* ltoa(long val, char* s, int radix) {
    if (radix == 10) {
        sprintf(s, "%ld", val);
    } else {
        unsigned long uval = (val < 0) ? -(unsigned long)val : (unsigned long)val;
        char tmp[34];
        int i = 33;
        tmp[i--] = 0;
        if (uval == 0) tmp[i--] = '0';
        while (uval) {
            int d = uval % radix;
            tmp[i--] = (d < 10) ? ('0' + d) : ('a' + d - 10);
            uval /= radix;
        }
        if (val < 0 && radix == 10) tmp[i--] = '-';
        strcpy(s, tmp + i + 1);
    }
    return s;
}

char* utoa(unsigned int val, char* s, int radix) {
    return ultoa((unsigned long)val, s, radix);
}

char* ultoa(unsigned long val, char* s, int radix) {
    if (radix == 10) {
        sprintf(s, "%lu", val);
        return s;
    }
    char tmp[34];
    int i = 33;
    tmp[i--] = 0;
    if (val == 0) tmp[i--] = '0';
    while (val) {
        int d = val % radix;
        tmp[i--] = (d < 10) ? ('0' + d) : ('a' + d - 10);
        val /= radix;
    }
    strcpy(s, tmp + i + 1);
    return s;
}

char* itoa(int val, char* s, int radix) {
    return ltoa((long)val, s, radix);
}

char* lltoa(long long val, char* s, int radix) {
    if (radix == 10) {
        sprintf(s, "%lld", val);
        return s;
    }
    unsigned long long uval = (val < 0) ? -(unsigned long long)val : (unsigned long long)val;
    char tmp[70];
    int i = 69;
    tmp[i--] = 0;
    if (uval == 0) tmp[i--] = '0';
    while (uval) {
        int d = uval % radix;
        tmp[i--] = (d < 10) ? ('0' + d) : ('a' + d - 10);
        uval /= radix;
    }
    strcpy(s, tmp + i + 1);
    return s;
}

char* ulltoa(unsigned long long val, char* s, int radix) {
    if (radix == 10) {
        sprintf(s, "%llu", val);
        return s;
    }
    char tmp[70];
    int i = 69;
    tmp[i--] = 0;
    if (val == 0) tmp[i--] = '0';
    while (val) {
        int d = val % radix;
        tmp[i--] = (d < 10) ? ('0' + d) : ('a' + d - 10);
        val /= radix;
    }
    strcpy(s, tmp + i + 1);
    return s;
}

char* dtostrf(double val, signed int width, unsigned int prec, char* s) {
    char fmt[24];
    snprintf(fmt, sizeof(fmt), "%%%d.%uf", width, prec);
    sprintf(s, fmt, val);
    return s;
}

} // extern "C"
