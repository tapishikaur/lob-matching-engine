#!/usr/bin/env bash
set -e
DEST="tests/doctest.h"
URL="https://raw.githubusercontent.com/doctest/doctest/master/doctest/doctest.h"
echo "Downloading doctest -> $DEST"
curl -fsSL "$URL" -o "$DEST"
echo "Done. Re-run cmake to pick up the test target."
