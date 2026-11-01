#!/bin/sh
# [UIT 0104] Compile (and link) every non-generated source with the user's real Windows toolchain headers and
# libraries (the builder package's deps/clang64); see check_windows_toolchain.py. Exit 0 = pass.
#   tools/tests/check_windows_toolchain.sh [--builder DIR|ZIP] [--fetch] [--clang LLVM_BIN] [-j N] [--keep]
here=$(dirname "$0")
exec python3 "$here/check_windows_toolchain.py" "$@"
