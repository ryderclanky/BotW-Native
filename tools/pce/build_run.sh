# Build, and launch only if the build succeeded (a running game locks the exe: the link then fails).
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
cmake --build build -j"$JOBS" || { echo "== BUILD FAILED, not launching (is the game still open?) =="; exit 1; }
echo "== build ok, exe $(date -r build/BotWRecomp.exe +%T) =="
. tools/pce/run.sh
