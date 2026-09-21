#include <iostream>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
#ifdef _WIN32
  auto handle = LoadLibraryA(argv[1]);
  if (!handle)
    return 3;
  auto test = reinterpret_cast<int (*)(const char *)>(
      GetProcAddress(handle, "ASTRA918_SIM_SELF_TEST"));
#else
  auto handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    std::cerr << dlerror() << "\n";
    return 3;
  }
  auto test = reinterpret_cast<int (*)(const char *)>(
      dlsym(handle, "ASTRA918_SIM_SELF_TEST"));
#endif
  return test ? test(argv[2]) : 4;
}
