# Launch with the user's real saves; log also in build/pce_test.log. Optional env: BOTW_PCE_INPUT_LOG=1,
# BOTW_PCE_MOUSE_LOG=1, BOTW_PCE_NO_PROMPTS=1, BOTW_PCE_FS_LOG=1, BOTW_PCE_SPIN=<deg/frame>.
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
export BOTW_PCE_INPUT_LOG=${BOTW_PCE_INPUT_LOG:-1}
./build/BotWRecomp.exe --game "$GAME" 2>&1 | tee build/pce_test.log
