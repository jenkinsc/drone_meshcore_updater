#!/usr/bin/env bash
# Build the updater for XIAO nRF52840 + Wio-SX1262 with the MeshCore command
# interface. Same as `./build.sh nrf52`, plus the MeshCore overlay/conf and its
# own build directory so it never clobbers a stock nrf52 build.
#
#   ./build-meshcore.sh            lab profile   (verbose logging; first hardware session)
#   MC_PROFILE=prod ./build-meshcore.sh   production-candidate profile (INFO logs compiled out)
#
# See updater/MESHCORE.md.
set -euo pipefail
cd "$(dirname "$0")"
export MC_WIO_SX1262=1
export MC_PROFILE="${MC_PROFILE:-lab}"
export BUILD_DIR_OVERRIDE="updater/build_xiao_ble_nrf52840_meshcore_${MC_PROFILE}"
exec ./build.sh nrf52 "$@"
