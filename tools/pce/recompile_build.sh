# After adding/removing a BOTW_HOOK: regenerate the guest C++ (~70 s), then build.
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
echo "== recompile =="
python tools/recompile.py "$GAME/code/U-King.rpx" generated 2>&1 | grep --line-buffered -v "Generating C++ shard"
echo "== build =="
cmake --build build -j"$JOBS"; echo "== build exit $? =="
