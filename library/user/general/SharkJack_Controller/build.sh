#!/bin/bash
# Reproducible cross-build of the Shark Jack Controller for the Pager.
# Requires Go (tested with go1.27). Produces ./sharkjack-controller (mipsle, soft-float).
set -e
cd "$(dirname "$0")"
GOOS=linux GOARCH=mipsle GOMIPS=softfloat CGO_ENABLED=0 \
    go build -trimpath -ldflags="-s -w" -o sharkjack-controller .
file sharkjack-controller
echo "size: $(ls -l sharkjack-controller | awk '{print $5}') bytes"
shasum -a 256 sharkjack-controller
