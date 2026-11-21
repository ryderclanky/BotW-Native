# Copy only what is needed to run (programs stripped of debug info, their DLLs, gamecontrollerdb.txt, the PC
# prompt font) into build/bin. Uses the project's own install rules (botw_package).
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
rm -rf build/bin
cmake --install build --prefix build/bin --strip && echo "== bin ready: $(du -sh build/bin | cut -f1) =="
