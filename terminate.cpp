#include <cstdio>
#include <cstdlib>

// GCC's default fatal-exception diagnostic pulls in a large C++ name demangler.
// Use a small fatal handler instead. Normal errors are caught by the program.
// With -static-libstdc++, this definition prevents the archive's verbose handler
// and demangler from being linked. This relies on GCC/libstdc++ symbol layout.
namespace __gnu_cxx {
[[noreturn]] void __verbose_terminate_handler() {
    std::fputs("clip-bridge: fatal unhandled exception\n", stderr);
    std::abort();
}
}
