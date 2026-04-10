// Link-wrap replacement for std::to_chars(float/double) overloads.
// Redirects floating-point formatting from libstdc++'s Ryu algorithm (~126 KB of
// lookup tables) to snprintf (~200 bytes), saving significant flash space.
// Applied via -Wl,--wrap=<mangled_symbol> in CMakeLists.txt.

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cmath>

struct to_chars_result_t {
    char* ptr;
    int ec; // 0 = success, EOVERFLOW = buffer too small
};

static to_chars_result_t do_to_chars(char* first, char* last, const char* fmt, double value)
{
    if (first >= last)
        return {last, EOVERFLOW};

    if (std::isnan(value)) {
        if (last - first < 3) return {last, EOVERFLOW};
        std::memcpy(first, "nan", 3);
        return {first + 3, 0};
    }

    if (std::isinf(value)) {
        if (last - first < 3) return {last, EOVERFLOW};
        std::memcpy(first, "inf", 3);
        return {first + 3, 0};
    }

    char tmp[80];
    int len = std::snprintf(tmp, sizeof(tmp), fmt, value);
    if (len < 0 || static_cast<size_t>(len) > static_cast<size_t>(last - first))
        return {last, EOVERFLOW};

    std::memcpy(first, tmp, len);
    return {first + len, 0};
}

static to_chars_result_t do_to_chars_prec(char* first, char* last, const char* fmt, int prec, double value)
{
    if (first >= last)
        return {last, EOVERFLOW};

    if (std::isnan(value)) {
        if (last - first < 3) return {last, EOVERFLOW};
        std::memcpy(first, "nan", 3);
        return {first + 3, 0};
    }

    if (std::isinf(value)) {
        if (last - first < 3) return {last, EOVERFLOW};
        std::memcpy(first, "inf", 3);
        return {first + 3, 0};
    }

    char tmp[80];
    int len = std::snprintf(tmp, sizeof(tmp), fmt, prec, value);
    if (len < 0 || static_cast<size_t>(len) > static_cast<size_t>(last - first))
        return {last, EOVERFLOW};

    std::memcpy(first, tmp, len);
    return {first + len, 0};
}

// chars_format values: scientific=1, fixed=2, general=3, hex=4
static const char* fmt_str(int cf)
{
    switch (cf) {
    case 1:  return "%e";
    case 2:  return "%f";
    case 4:  return "%a";
    default: return "%g";
    }
}

static const char* fmt_str_prec(int cf)
{
    switch (cf) {
    case 1:  return "%.*e";
    case 2:  return "%.*f";
    case 4:  return "%.*a";
    default: return "%.*g";
    }
}

extern "C" {

// std::to_chars(char*, char*, float)
to_chars_result_t __wrap__ZSt8to_charsPcS_f(char* first, char* last, float value)
{
    return do_to_chars(first, last, "%g", static_cast<double>(value));
}

// std::to_chars(char*, char*, float, std::chars_format)
to_chars_result_t __wrap__ZSt8to_charsPcS_fSt12chars_format(char* first, char* last, float value, int cf)
{
    return do_to_chars(first, last, fmt_str(cf), static_cast<double>(value));
}

// std::to_chars(char*, char*, float, std::chars_format, int)
to_chars_result_t __wrap__ZSt8to_charsPcS_fSt12chars_formati(char* first, char* last, float value, int cf, int prec)
{
    return do_to_chars_prec(first, last, fmt_str_prec(cf), prec, static_cast<double>(value));
}

// std::to_chars(char*, char*, double)
to_chars_result_t __wrap__ZSt8to_charsPcS_d(char* first, char* last, double value)
{
    return do_to_chars(first, last, "%g", value);
}

// std::to_chars(char*, char*, double, std::chars_format)
to_chars_result_t __wrap__ZSt8to_charsPcS_dSt12chars_format(char* first, char* last, double value, int cf)
{
    return do_to_chars(first, last, fmt_str(cf), value);
}

// std::to_chars(char*, char*, double, std::chars_format, int)
to_chars_result_t __wrap__ZSt8to_charsPcS_dSt12chars_formati(char* first, char* last, double value, int cf, int prec)
{
    return do_to_chars_prec(first, last, fmt_str_prec(cf), prec, value);
}

} // extern "C"
