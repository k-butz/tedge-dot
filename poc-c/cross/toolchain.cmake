# CMake toolchain for cross-compiling the C PoC with `zig cc` against Debian's
# multiarch libraries. Driven by three environment variables, all set by
# build.sh: ZIG_TARGET, DEB_MULTIARCH and TDOT_SYSTEM_PROCESSOR.
#
# Only used for cross builds — a native `cmake -B build -S poc-c` is untouched.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR $ENV{TDOT_SYSTEM_PROCESSOR})

set(CMAKE_C_COMPILER   /usr/local/bin/zig-cc)
set(CMAKE_CXX_COMPILER /usr/local/bin/zig-cxx)
set(CMAKE_AR           /usr/local/bin/zig-ar)
set(CMAKE_RANLIB       /usr/local/bin/zig-ranlib)

# zig does not put /usr/include on the implicit include path the way a native
# gcc does, so Debian's layout (cJSON.h in /usr/include/cjson/, mosquitto.h
# straight in /usr/include) would not resolve. -idirafter appends it *after*
# zig's own libc headers, so the bundled glibc headers still win any conflict —
# plain -I would put Debian's glibc headers first and mix the two.
add_compile_options(-idirafter /usr/include)

# Foreign-architecture libraries live here; headers are shared in /usr/include.
set(CMAKE_LIBRARY_PATH /usr/lib/$ENV{DEB_MULTIARCH})
set(CMAKE_FIND_ROOT_PATH /usr/lib/$ENV{DEB_MULTIARCH})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
# Without this, find_package() happily returns a *host* package and hands it to
# the target linker (open62541 is the one that bites).
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
