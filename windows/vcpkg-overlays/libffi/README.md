# MSVC assembly build fix

Based on microsoft/vcpkg commit 434307da09bc05b2c86996dccc8b2351fc0d5d37, ports/libffi (MIT license included).
Only change: skip the generic helper's redundant ASM/LLVM acquisition on MSVC. This port already selects its own upstream msvcc.sh assembler wrapper, using the installed Visual Studio assembler. Library source, version, hash and runtime behavior are unchanged.
