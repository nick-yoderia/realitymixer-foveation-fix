#!/bin/sh
# Build and zip a release (Git Bash on Windows):  sh tools/make-release.sh v0.1.0-alpha
set -eu
VER=${1:?version, e.g. v0.1.0-alpha}
cd "$(dirname "$0")/.."
OUT=dist/realitymixer-foveation-fix-$VER
rm -rf "$OUT" build/openvr_api.dll && mkdir -p "$OUT/bin"
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 | grep -vE '^\s*\S+\.cpp\s*$|Creating library' || true
test -f build/openvr_api.dll
cp build/openvr_api.dll "$OUT/bin/"
cp install.ps1 tools/release/*.cmd tools/release/README.txt LICENSE "$OUT/"
(cd dist && rm -f "realitymixer-foveation-fix-$VER.zip" && python -I -m zipfile -c "realitymixer-foveation-fix-$VER.zip" "realitymixer-foveation-fix-$VER")
echo "dist/realitymixer-foveation-fix-$VER.zip"
