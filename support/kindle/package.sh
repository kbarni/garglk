#!/bin/sh
# Assembles the output of support/kindle/build.sh into build/package:
#   gargoyle-kindlehf.zip              gargoyle/ and documents/, to extract at the Kindle root
#   gargoyle_<version>_kindlehf.kpkg   the same files as a KPM package

set -e

TOP=$(CDPATH='' cd "$(dirname "$0")/../.." && pwd -P)
KINDLE="${TOP}/support/kindle"
OUT="${TOP}/build/package"
STAGE="${OUT}/stage"
APP="${STAGE}/gargoyle"
VERSION="${VERSION:-$(cat "${TOP}/VERSION")}"
STRIP="${KINDLE_TC_DIR:-${HOME}/x-tools/arm-kindlehf-linux-gnueabihf}/bin/arm-kindlehf-linux-gnueabihf-strip"

rm -rf "${OUT}"
mkdir -p "${APP}/games" "${APP}/saved_games" "${STAGE}/documents"

cp -R "${TOP}/build/dist" "${APP}/dist"
cp "${KINDLE}/gtkrc" "${APP}/dist/"
cp "${KINDLE}/gargoyle.sh" "${KINDLE}/menu.json" "${KINDLE}/config.xml" "${KINDLE}/gargoyle.png" "${APP}/"
mkdir -p "${APP}/config/bocfel"
cp "${KINDLE}/bocfelrc" "${APP}/config/bocfel/"

# Go strips the downloader itself.
if [ -x "${STRIP}" ]; then
    for FILE in "${APP}"/dist/*; do
        if [ -f "${FILE}" ] && [ "$(head -c 4 "${FILE}" | tail -c 3)" = ELF ] && [ "${FILE##*/}" != ifdb-dl ]; then
            "${STRIP}" --strip-unneeded "${FILE}"
        fi
    done
fi

# The Kindle library takes the scriptlet icon from its own header.
awk -v icon="$(base64 -w 0 "${KINDLE}/gargoyle.png")" '{ gsub(/@ICON@/, icon); print }' \
    "${KINDLE}/scriptlet.sh" >"${STAGE}/documents/Gargoyle.sh"
chmod +x "${APP}/gargoyle.sh" "${STAGE}/documents/Gargoyle.sh"

(cd "${STAGE}" && zip -qr "${OUT}/gargoyle-kindlehf.zip" gargoyle documents)

KPKG="${OUT}/kpkg"
mkdir -p "${KPKG}/scriptlets"
cp -R "${APP}" "${KPKG}/"
cp "${STAGE}/documents/Gargoyle.sh" "${KPKG}/scriptlets/"
cp "${KINDLE}/kpm/install.sh" "${KINDLE}/kpm/uninstall.sh" "${KINDLE}/kpm/launch.sh" "${KPKG}/"

# KPM compares three numbers, so a release such as 2026.1-kindle.2 becomes 2026.1.2.
NUMBERS=$(echo "${VERSION}" | sed -E 's/[^0-9]+/./g; s/^[.]+|[.]+$//g')
MAJOR=$(echo "${NUMBERS}" | cut -d. -f1)
MINOR=$(echo "${NUMBERS}" | cut -s -d. -f2)
PATCH=$(echo "${NUMBERS}" | cut -s -d. -f3)
cat >"${KPKG}/manifest.json" <<EOF
{
  "manifest_version": 2,
  "id": "gargoyle",
  "name": "Gargoyle",
  "author": "kbarni",
  "description": "Interactive fiction player for Z-machine, Glulx, TADS, Hugo, ADRIFT, Alan, AGT, AdvSys, JACL, Level 9, Magnetic Scrolls and Scott Adams games",
  "version": [${MAJOR:-0}, ${MINOR:-0}, ${PATCH:-0}],
  "dependencies": [],
  "supported_platforms": ["kindlehf"]
}
EOF

tar -czf "${OUT}/gargoyle_${NUMBERS:-0.0.0}_kindlehf.kpkg" -C "${KPKG}" .

rm -rf "${STAGE}" "${KPKG}"
ls -l "${OUT}"
