#!/bin/sh
# check_isolation.sh [BUILD_DIR] - see check_isolation.py (the rules) and docs/abi_recomp_runtime.md, section 8.
exec python3 "$(dirname "$0")/check_isolation.py" "$@"
