#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
WORKSPACE="${1:-$(CDPATH= cd -- "$ROOT/.." && pwd)}"
REGULAR_SOURCE="${MSYS_CJK_REGULAR_SOURCE:-/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc}"
BOLD_SOURCE="${MSYS_CJK_BOLD_SOURCE:-/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc}"
FACE_INDEX="${MSYS_CJK_FONT_INDEX:-2}"
PIXEL_SIZE=14
OUTPUT_DIR="$ROOT/files/x11display/fonts"
REGULAR_OUTPUT="$OUTPUT_DIR/msys-cjk-14-regular.bdf"
BOLD_OUTPUT="$OUTPUT_DIR/msys-cjk-14-bold.bdf"
BUILD_DIR="${TMPDIR:-/tmp}/msys-bdf-generator-$$"

cleanup()
{
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT INT TERM

mkdir -p "$BUILD_DIR" "$OUTPUT_DIR"
gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Os \
    -o "$BUILD_DIR/bdf_from_ttc" "$SCRIPT_DIR/bdf_from_ttc.c" -ldl

mapfile -d '' catalogs < <(
    find "$WORKSPACE" -type f -path '*/files/share/i18n/*.json' -print0 |
        sort -z
)
rm -f "$REGULAR_OUTPUT.new" "$BOLD_OUTPUT.new" "$OUTPUT_DIR/fonts.dir.new"
"$BUILD_DIR/bdf_from_ttc" "$REGULAR_SOURCE" "$FACE_INDEX" "$PIXEL_SIZE" \
    medium "$REGULAR_OUTPUT.new" "${catalogs[@]}"
"$BUILD_DIR/bdf_from_ttc" "$BOLD_SOURCE" "$FACE_INDEX" "$PIXEL_SIZE" \
    bold "$BOLD_OUTPUT.new" "${catalogs[@]}"

regular_xlfd=$(sed -n 's/^FONT //p' "$REGULAR_OUTPUT.new")
bold_xlfd=$(sed -n 's/^FONT //p' "$BOLD_OUTPUT.new")
test -n "$regular_xlfd"
test -n "$bold_xlfd"
{
    printf '4\n'
    printf '%s %s\n' "$(basename -- "$REGULAR_OUTPUT")" \
        "${regular_xlfd/-msys-msyscjk-/-msys-Noto Sans CJK SC-}"
    printf '%s %s\n' "$(basename -- "$BOLD_OUTPUT")" \
        "${bold_xlfd/-msys-msyscjk-/-msys-Noto Sans CJK SC-}"
    printf '%s %s\n' "$(basename -- "$REGULAR_OUTPUT")" "$regular_xlfd"
    printf '%s %s\n' "$(basename -- "$BOLD_OUTPUT")" "$bold_xlfd"
} > "$OUTPUT_DIR/fonts.dir.new"

mv -f "$REGULAR_OUTPUT.new" "$REGULAR_OUTPUT"
mv -f "$BOLD_OUTPUT.new" "$BOLD_OUTPUT"
mv -f "$OUTPUT_DIR/fonts.dir.new" "$OUTPUT_DIR/fonts.dir"
chmod 0644 "$REGULAR_OUTPUT" "$BOLD_OUTPUT" "$OUTPUT_DIR/fonts.dir"
echo "generated: $REGULAR_OUTPUT"
echo "generated: $BOLD_OUTPUT"
