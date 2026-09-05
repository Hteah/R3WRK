#!/usr/bin/env bash
# Build R3WRK (Standalone + VST3 + AU) on macOS. Clones JUCE if missing.
set -euo pipefail
cd "$(dirname "$0")"

JUCE_TAG=8.0.15
CONFIG=${1:-Release}

if [ ! -d JUCE ]; then
    echo "→ cloning JUCE $JUCE_TAG"
    git clone --depth 1 --branch "$JUCE_TAG" https://github.com/juce-framework/JUCE.git
    echo "→ applying R3WRK patches to JUCE (see patches/README.md)"
    for p in patches/*.patch; do
        (cd JUCE && git apply "../$p") || echo "  ! $p didn't apply -- see patches/README.md"
    done
fi

command -v cmake >/dev/null || { echo "cmake not found — see BUILD_ON_MACOS.md"; exit 1; }

cd Plugin
cmake -B build -G Xcode -DCMAKE_POLICY_VERSION_MINIMUM=3.5 .
cmake --build build --config "$CONFIG" \
    --target R3WRKSmokeTest R3WRK_Standalone R3WRK_VST3 R3WRK_AU

echo
echo "→ artefacts in Plugin/build/R3WRK_artefacts/$CONFIG/"
echo "→ run the engine test:"
echo "  ./build/R3WRKSmokeTest_artefacts/$CONFIG/R3WRKSmokeTest"
