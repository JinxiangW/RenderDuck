#include <windows.h>
// Uses the same embedded Python DLL and standard library as the built application.
int wmain(int argc, wchar_t **argv)
{
  HMODULE python = LoadLibraryW(L"python36.dll");
  if(!python)
    return 127;
  auto main = (int (*)(int, wchar_t **))GetProcAddress(python, "Py_Main");
  return main ? main(argc, argv) : 126;
}
