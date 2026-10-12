cmake_minimum_required(VERSION 3.20.0)

# Boards live under BOARD_ROOT so we can carry a local xiao_nrf54lm20a
# definition until (and after) it ships in the upstream NCS board tree.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# In-tree Zephyr modules. nordic-legacy-dfu is the C++ Legacy DFU client
# ported from the Android DFU Library; it compiles in only when
# CONFIG_NORDIC_LEGACY_DFU=y.
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/modules/nordic-legacy-dfu)

# MeshCore command interface (Seeed Wio-SX1262 on a XIAO nRF52840). Opt-in:
# set MC_WIO_SX1262=1 in the environment (build-meshcore.sh does) or pass
# -DMC_WIO_SX1262=ON. It is chosen here, before find_package(), because it adds
# a devicetree overlay and a Kconfig fragment that must be known by then, and
# because an environment variable reaches the app image through sysbuild
# whereas an unprefixed -D does not reliably.
if(DEFINED ENV{MC_WIO_SX1262} OR MC_WIO_SX1262)
  set(MC_BOARD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/boards/meshcore)
  list(APPEND EXTRA_DTC_OVERLAY_FILE ${MC_BOARD_DIR}/xiao_ble_wio_sx1262.overlay)
  list(APPEND EXTRA_CONF_FILE ${MC_BOARD_DIR}/xiao_ble_wio_sx1262.conf)
  # The operator's key. Git-ignored, so a key is never committed by accident.
  if(EXISTS ${MC_BOARD_DIR}/operator.conf)
    list(APPEND EXTRA_CONF_FILE ${MC_BOARD_DIR}/operator.conf)
  endif()
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(drone_meshcore_updater LANGUAGES C CXX VERSION 0.1.0)

target_sources(app PRIVATE
  src/main.c
  src/crash_record.c
  src/led.c
  src/storage.c
  src/upload_hook.c
  src/fsx_mgmt.c
  src/fsx_stream.c
  src/log_stream.c
  src/selfconfirm.c
  src/config.c
  src/antenna.c
  src/battery.c
  src/battery_curve.c
  src/battery_status.c
  src/ble_scanner.c
  src/ble_pairing.c
  src/survey.c
  src/pin_addr.c
  src/firmware_zip.c
  src/firmware_inspect.c
  src/md5.c
  src/firmware_map.c
  src/dfu_client.cpp
  src/dfu_runner.c
  src/dfu_status.c
  src/dfu_transport.c
  src/transport_ble.c
  src/ble_tx_power.c
)

# The 1200-baud touch that reboots into the Adafruit bootloader. nRF52840 only:
# it needs GPREGRET, a bootloader that reads it, and a CDC ACM the host can set
# a line coding on. The nRF54L board is reached through a debug probe and needs
# no such thing; the ESP32-S3 has no CDC ACM at all (it uses usb_serial), and
# its equivalent is a host-side DTR/RTS sequence with nothing on the device.
if(CONFIG_SOC_NRF52840 AND CONFIG_USBD_CDC_ACM_CLASS)
  target_sources(app PRIVATE src/usb_dfu_touch.c)
endif()

target_include_directories(app PRIVATE src)

# The ElegantOTA transport, and the only file here that is not board-neutral.
# No nRF part has WiFi, so gating on the Kconfig rather than on a board name
# means a future WiFi-capable board picks it up without an edit here.
if(CONFIG_WIFI)
  target_sources(app PRIVATE
    src/transport_wifi_elegantota.c
    # The WiFi half of the scanner. Gated with the transport rather than
    # separately: both need the same radio and the same net_mgmt API, so a
    # board that has one always has the other.
    src/wifi_survey.c
  )
endif()

# MeshCore command interface. Everything under src/mc/ is plain C; only the
# *_zephyr/radio/permit files need Zephyr. The vendored Ed25519 is the one in
# MeshCore (orlp, zlib licence); ED25519_NO_SEED removes its OS-RNG seeding,
# which we do not use (the seed comes from the SoC's entropy source).
if(CONFIG_MC_CMD)
  target_sources(app PRIVATE
    src/mc/mc_zephyr.c
    src/mc/mc_radio.c
    src/mc/mc_sx126x.c
    src/mc/mc_permit.c
    src/mc/mc_crypto.c
    src/mc/mc_store.c
    src/mc/mc_gate.c
    src/mc/mc_cmd.c
    src/mc/mc_progress.c
    src/mc/mc_ctl.c
    src/mc/mc_evlog.c
    src/mc/mc_replay.c
    src/mc/mc_wire.c
    src/mc/mc_id.c
    src/mc/mc_node.c
    src/mc/ed25519/fe.c
    src/mc/ed25519/ge.c
    src/mc/ed25519/sc_small.c
    src/mc/ed25519/sha512.c
    src/mc/ed25519/sign.c
    src/mc/ed25519/keypair.c
    src/mc/ed25519/key_exchange.c
    src/mc/ed25519/ge_small.c
  )
  # MC_ED25519_SMALL drops the 30 KB base-point table (see ge.c / ge_small.c).
  target_compile_definitions(app PRIVATE ED25519_NO_SEED MC_ED25519_SMALL)
endif()
