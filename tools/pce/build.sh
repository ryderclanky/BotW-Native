# Incremental build only (no hook changes).
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
cmake --build build -j"$JOBS"; echo "== build exit $? =="
