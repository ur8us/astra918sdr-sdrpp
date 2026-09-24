#!/bin/sh
# Launch the matching SDR++ build with the Astra918 CMX918 receiver module.
set -eu

script_dir=$(CDPATH= cd -P -- "$(dirname -- "$0")" && pwd)
sdrpp_source=${SDRPP_SOURCE:-"$script_dir/../cmx918_sdrpp_upstream"}
sdrpp_build=${SDRPP_BUILD:-"$sdrpp_source/build"}

# Hardware by default; offline work needs --module pointing to a Debug build
# together with --simulator HOST:PORT.
unset ASTRA918_SIMULATOR
exec python3 "$script_dir/scripts/run.py" \
    --sdrpp-source "$sdrpp_source" \
    --sdrpp-build "$sdrpp_build" \
    "$@"
