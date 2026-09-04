#!/usr/bin/env bash
# Build EdisonClone (Standalone + VST3 + AU) on macOS. Clones JUCE if missing.
set -euo pipefail
cd "$(dirname "$0")"

JUCE_TAG=8.0.15
CONFIG=${1:-Release}

if [ ! -d JUCE ]; then
    echo "→ cloning JUCE $JUCE_TAG"
    git clone --depth 1 --branch "$JUCE_TAG" https://github.com/juce-framework/JUCE.git
fi

command -v cmake >/dev/null || { echo "cmake not found — see BUILD_ON_MACOS.md"; exit 1; }

cd Plugin
cmake -B build -G Xcode -DCMAKE_POLICY_VERSION_MINIMUM=3.5 .
cmake --build build --config "$CONFIG" \
    --target EdisonCloneSmokeTest EdisonClone_Standalone EdisonClone_VST3 EdisonClone_AU

echo
echo "→ artefacts in Plugin/build/EdisonClone_artefacts/$CONFIG/"
echo "→ run the engine test:"
echo "  ./build/EdisonCloneSmokeTest_artefacts/$CONFIG/EdisonCloneSmokeTest"
