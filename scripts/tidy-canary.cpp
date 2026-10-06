// The canary tidy.sh runs before the real files: two violations the
// configured checks must report (cppcoreguidelines-no-malloc and
// cppcoreguidelines-owning-memory). A run that reports nothing here is not
// analysing — a missing SDK, a check set that did not load, a clang-tidy
// that fails to parse — and an empty log would otherwise read as "clean".
#include <cstdlib>
int* canary() {
    int* p = static_cast<int*>(malloc(sizeof(int)));
    return p;
}
