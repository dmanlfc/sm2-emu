#!/bin/sh

# ██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
# ██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
# █████╔╝ █████╗  ██║███████╗███████║██║   ██║
# ██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
# ██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
# ╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
#            継 承  ( k e i s h ō )
#
# A Sega Model 1, 2 & 3 arcade emulator.
# Copyright (c) 2025+ Daniel Martin (dmanlfc)
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This header must not be removed. Keisho is licensed under the GNU General
# Public License, version 3 or later, with additional terms under section 7 of
# that licence. See LICENSE and NOTICE.
#
# Make an installed Keisho.app self-contained: copy its non-system dylibs and
# MoltenVK into Contents/Frameworks, register MoltenVK with the bundled Vulkan
# loader, and re-sign the bundle ad hoc. Needs dylibbundler (brew install
# dylibbundler) and MoltenVK from Homebrew or the Vulkan SDK.
#
#   cmake --install build --prefix install
#   cmake/macos-bundle.sh install/Keisho.app
#
set -eu

app=${1:?usage: macos-bundle.sh path/to/Keisho.app}
exe="$app/Contents/MacOS/$(basename "$app" .app)"
frameworks="$app/Contents/Frameworks"
icd_dir="$app/Contents/Resources/vulkan/icd.d"

dylibbundler -of -b -cd -x "$exe" -d "$frameworks" -p @executable_path/../Frameworks/

# MoltenVK is loaded by the Vulkan loader through its ICD manifest rather than
# linked, so dylibbundler does not see it.
moltenvk_dir=${MOLTENVK_DIR:-}
if [ -z "$moltenvk_dir" ]; then
    if command -v brew >/dev/null 2>&1 && brew --prefix molten-vk >/dev/null 2>&1; then
        moltenvk_dir=$(brew --prefix molten-vk)
    elif [ -n "${VULKAN_SDK:-}" ]; then
        moltenvk_dir=$VULKAN_SDK
    fi
fi
if [ -z "$moltenvk_dir" ] || [ ! -f "$moltenvk_dir/lib/libMoltenVK.dylib" ]; then
    echo "macos-bundle.sh: MoltenVK not found; set MOLTENVK_DIR" >&2
    exit 1
fi

cp -f "$moltenvk_dir/lib/libMoltenVK.dylib" "$frameworks/"
chmod u+w "$frameworks/libMoltenVK.dylib"
install_name_tool -id @executable_path/../Frameworks/libMoltenVK.dylib \
    "$frameworks/libMoltenVK.dylib"

# The loader searches Contents/Resources/vulkan/icd.d of the main bundle; the
# manifest's library path is relative to the manifest itself.
mkdir -p "$icd_dir"
manifest=$(find "$moltenvk_dir/share/vulkan/icd.d" "$moltenvk_dir/etc/vulkan/icd.d" \
               -name 'MoltenVK_icd.json' 2>/dev/null | head -n 1 || true)
if [ -n "$manifest" ]; then
    sed 's|"library_path"[^,]*|"library_path": "../../../Frameworks/libMoltenVK.dylib"|' \
        "$manifest" > "$icd_dir/MoltenVK_icd.json"
else
    cat > "$icd_dir/MoltenVK_icd.json" <<'EOF'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../../../Frameworks/libMoltenVK.dylib",
        "api_version": "1.2.0",
        "is_portability_driver": true
    }
}
EOF
fi

codesign --force --deep --sign - "$app"

echo "bundled into $frameworks:"
ls "$frameworks"
