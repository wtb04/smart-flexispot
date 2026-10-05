#!/bin/sh
# Takes what a picture carries besides itself out of every one in the
# repository, as the website does with its own: when and where it was made,
# with what, and a screenshot's note that it is one. The colour profile stays,
# or the colours would change.
#
#   tools/strip_metadata.sh          strips them
#   tools/strip_metadata.sh --check  fails if any still has some, for CI
#
# Needs exiftool: brew install exiftool, or apt install libimage-exiftool-perl.
cd "$(dirname "$0")/.." || exit 1
command -v exiftool >/dev/null || { echo "exiftool is needed" >&2; exit 2; }

pictures=$(git ls-files '*.png' '*.jpg' '*.jpeg' '*.webp' '*.gif' | grep -v -e '^managed_components/' -e '^components/esp_hosted/')
[ -n "$pictures" ] || exit 0

if [ "${1:-}" = "--check" ]; then
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    echo "$pictures" | while read -r f; do mkdir -p "$work/$(dirname "$f")" && cp "$f" "$work/$f"; done
    result=$(cd "$work" && exiftool -r -overwrite_original -all= --icc_profile:all . 2>&1)
else
    result=$(echo "$pictures" | xargs exiftool -overwrite_original -all= --icc_profile:all 2>&1)
fi

changed=$(echo "$result" | sed -n 's/^ *\([0-9][0-9]*\) image files updated.*/\1/p')
changed=${changed:-0}
if [ "${1:-}" = "--check" ] && [ "$changed" -gt 0 ]; then
    echo "$changed picture(s) still carry metadata: run tools/strip_metadata.sh" >&2
    exit 1
fi
echo "$changed picture(s) had metadata taken out"
