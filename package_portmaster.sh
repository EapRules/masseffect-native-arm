#!/usr/bin/env bash
#
# Build a game-data-free PortMaster package for the gamepad-build port.
# `tools/` is a frozen historical scaffold, so current packaging lives here.
set -euo pipefail

cd "$(dirname "$0")"

OUT="build/masseffect-portmaster.zip"
STAGE="build/pkg-portmaster"

[ -x build/masseffect ] \
    || { echo "build/masseffect missing - run make first" >&2; exit 1; }
[ -f build/libs.armhf/MANIFEST.txt ] \
    || { echo "build/libs.armhf missing - run make libs first" >&2; exit 1; }
[ -d build/libs.armhf/licenses ] \
    || { echo "build/libs.armhf/licenses missing - run make libs again" >&2; exit 1; }

rm -rf "$STAGE"
rm -f "$OUT"
mkdir -p "$STAGE/masseffect"

cp "ports/Mass Effect Infiltrator.sh"            "$STAGE/"
cp build/masseffect                  "$STAGE/masseffect/"
cp ports/masseffect/masseffect.gptk   "$STAGE/masseffect/"
cp ports/masseffect/masseffect.eapx.json "$STAGE/masseffect/"
cp ports/masseffect/PUT_MASS_EFFECT_DATA_HERE.txt "$STAGE/masseffect/"
cp tools/eapx.py                    "$STAGE/masseffect/"
cp ports/masseffect/port.json        "$STAGE/masseffect/"
cp ports/masseffect/gameinfo.xml     "$STAGE/masseffect/"
cp ports/masseffect/cover.png        "$STAGE/masseffect/"
cp ports/masseffect/screenshot.png   "$STAGE/masseffect/"
cp ports/masseffect/README.md        "$STAGE/masseffect/"
cp ports/masseffect/CREDITS.md       "$STAGE/masseffect/"
cp ports/masseffect/grab_screen.sh   "$STAGE/masseffect/"
cp -R build/libs.armhf              "$STAGE/masseffect/"

mkdir -p "$STAGE/masseffect/licenses/libraries"
cp LICENSE "$STAGE/masseffect/licenses/LICENSE-portmaster-port.txt"
cp NOTICE.md "$STAGE/masseffect/licenses/NOTICE.md"
cp third_party/gmloader/LICENSE.md "$STAGE/masseffect/licenses/LICENSE-gmloader.md"
cp third_party/masseffect-vita/LICENSE "$STAGE/masseffect/licenses/LICENSE-masseffect-vita.txt"
cp third_party/powervr/LICENSE.md "$STAGE/masseffect/licenses/LICENSE-powervr.txt"
cp third_party/vfpvector/LICENSE "$STAGE/masseffect/licenses/LICENSE-vfpvector.txt"
mv "$STAGE/masseffect/libs.armhf/licenses/"* \
   "$STAGE/masseffect/licenses/libraries/"
rmdir "$STAGE/masseffect/libs.armhf/licenses"

chmod +x "$STAGE/Mass Effect Infiltrator.sh" "$STAGE/masseffect/masseffect" \
         "$STAGE/masseffect/grab_screen.sh" "$STAGE/masseffect/eapx.py"

find "$STAGE" \( -name '._*' -o -name '.DS_Store' \) -delete
(cd "$STAGE" && zip -qr "../../$OUT" .)

# PortMaster rewrites an unsigned root launcher in place to add this line. Its
# implementation opens the file with mode "w" before writing, so an interrupted
# install can leave a zero-byte launcher on exFAT. Ship the canonical signature
# ourselves: with the release filename below, PortMaster recognizes it and does
# not touch the launcher after extraction.
EXPECTED_SIGNATURE="# PORTMASTER: masseffect-portmaster.zip, Mass Effect Infiltrator.sh"
ACTUAL_SIGNATURE="$(unzip -p "$OUT" "Mass Effect Infiltrator.sh" | sed -n '2p')"
[ "$ACTUAL_SIGNATURE" = "$EXPECTED_SIGNATURE" ] || {
    echo "package has wrong PortMaster signature: $ACTUAL_SIGNATURE" >&2
    exit 1
}
[ "$(unzip -p "$OUT" "Mass Effect Infiltrator.sh" | wc -c | tr -d ' ')" -gt 0 ] || {
    echo "package has an empty launcher" >&2
    exit 1
}
unzip -tq "$OUT" >/dev/null

# The eapx in tools/ is a copy of the canonical one and drifts silently: this
# port shipped 0.4.1 while the canonical tree was at 0.4.2, because nobody
# compared them. Refuse to package on a mismatch instead of trusting memory.
canonical="${EAPX_CANONICAL:-$HOME/Projects/Others/handheld/eapx/eapx.py}"
if [ -f "$canonical" ]; then
  if ! cmp -s tools/eapx.py "$canonical"; then
    echo "refusing package: tools/eapx.py differs from the canonical $canonical" >&2
    echo "  packaged:  $(sed -n 's/^VERSION = "\(.*\)"/\1/p' tools/eapx.py)" >&2
    echo "  canonical: $(sed -n 's/^VERSION = "\(.*\)"/\1/p' "$canonical")" >&2
    exit 1
  fi
else
  echo "note: canonical eapx not found at $canonical; packaged copy not verified" >&2
fi

listing="$(unzip -Z1 "$OUT")"
for required in "Mass Effect Infiltrator.sh" "masseffect/masseffect" \
                "masseffect/masseffect.gptk" "masseffect/port.json" \
                "masseffect/gameinfo.xml" "masseffect/README.md" \
                "masseffect/CREDITS.md" \
                "masseffect/cover.png" "masseffect/screenshot.png" \
                "masseffect/eapx.py" \
                "masseffect/masseffect.eapx.json" \
                "masseffect/PUT_MASS_EFFECT_DATA_HERE.txt" \
                "masseffect/licenses/LICENSE-portmaster-port.txt" \
                "masseffect/licenses/LICENSE-gmloader.md" \
                "masseffect/licenses/LICENSE-masseffect-vita.txt" \
                "masseffect/licenses/LICENSE-powervr.txt" \
                "masseffect/licenses/LICENSE-vfpvector.txt" \
                "masseffect/licenses/libraries/libstdc++.so.6.copyright" \
                "masseffect/libs.armhf/MANIFEST.txt"; do
    case "$listing" in
        *"$required"*) ;;
        *) echo "package missing $required" >&2; exit 1 ;;
    esac
done

case "$listing" in
    *libMassEffect.so*|*assets/published/*)
        echo "refusing package: proprietary game data found" >&2
        exit 1
        ;;
esac

# A stale zip with an old binary passes every check above - they all pass on an
# old binary. Comparing the hashes is the only check that catches it; twice a
# release was nearly published with a binary older than the one just verified.
built_sha="$(shasum -a 256 build/masseffect | cut -d' ' -f1)"
packed_sha="$(unzip -p "$OUT" masseffect/masseffect | shasum -a 256 | cut -d' ' -f1)"
[ "$built_sha" = "$packed_sha" ] || {
    echo "refusing package: the zipped binary is not the one just built" >&2
    echo "  built:  $built_sha" >&2
    echo "  packed: $packed_sha" >&2
    exit 1
}

echo "$OUT"
# Single source: src/port_version.h. The binary answers --version with the same
# string, but it is armhf and the packaging host is not, so read the header.
echo "port version: $(sed -n 's/^#define MASSEFFECT_PORT_VERSION "\(.*\)"$/\1/p' src/port_version.h)"
echo "binary sha256: $built_sha"
du -h "$OUT"
