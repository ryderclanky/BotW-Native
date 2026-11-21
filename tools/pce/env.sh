# Shared settings for the tools/pce scripts (sourced). Machine-specific values go in tools/pce/local.env
# (not committed; see local.env.example) or in the environment.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
[ -f tools/pce/local.env ] && . tools/pce/local.env
# Game folder (the one holding code/U-King.rpx): BOTW_GAME_DIR, or ./game next to the repo
GAME="${BOTW_GAME_DIR:-$ROOT/game}"
[ -f "$GAME/code/U-King.rpx" ] || { echo "== game not found at \"$GAME\": set BOTW_GAME_DIR (tools/pce/local.env) =="; exit 1; }
# The user folder must be explicit: an MSYS login shell started from Git Bash has no APPDATA, and the exe
# then falls back to .\BotWRecomp (no saves -> the game starts a new game). Default: %APPDATA%\BotWRecomp.
if [ -z "$BOTW_USER_ROOT" ]; then
    APPDIR="${APPDATA:-$(cygpath -u "$USERPROFILE" 2>/dev/null)/AppData/Roaming}"
    [ -z "$APPDATA" ] && [ -z "$USERPROFILE" ] && APPDIR="/c/Users/$USERNAME/AppData/Roaming"
    export BOTW_USER_ROOT="$(cygpath -m "$APPDIR/BotWRecomp" 2>/dev/null || echo "$APPDIR/BotWRecomp")"
fi
JOBS="${BOTW_BUILD_JOBS:-$(( $(nproc) > 2 ? $(nproc) - 2 : 1 ))}"
