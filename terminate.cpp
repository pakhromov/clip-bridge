#include <cstdio>
#include <cstdlib>

// GCC's default fatal-exception diagnostic pulls in a large C++ name demangler.
// Use a small fatal handler instead. Normal errors are caught by the program.
namespace __gnu_cxx {
[[noreturn]] void __verbose_terminate_handler() {
    std::fputs("clip-bridge: fatal unhandled exception\n", stderr);
    std::abort();
}
}
