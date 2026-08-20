set(cov_srcs
    "main.c"
    "adapter/adapter.c"
    "adapter/config.c"
    "adapter/gameid.c"
    "adapter/hid_parser.c"
    "adapter/kb_monitor.c"
    "adapter/macro.c"
    "adapter/mapping_quirks.c"
    "adapter/memory_card.c"
    "adapter/wired/npiso.c"
    "adapter/wired/cdi.c"
    "adapter/wired/saturn.c"
    "adapter/wired/pce.c"
    "adapter/wired/genesis.c"
    "adapter/wired/real.c"
    "adapter/wired/jag.c"
    "adapter/wired/pcfx.c"
    "adapter/wired/jvs.c"
    "adapter/wired/n64.c"
    "adapter/wired/dc.c"
    "adapter/wired/gc.c"
    "adapter/wired/ps.c"
    "adapter/wired/sea.c"
    "adapter/wired/wii.c"
    "adapter/wired/parallel_1p.c"
    "adapter/wired/parallel_2p.c"
    "adapter/wired/wired.c"
    "adapter/wireless/hid_generic.c"
    "adapter/wireless/ps3.c"
    "adapter/wireless/wii.c"
    "adapter/wireless/ps.c"
    "adapter/wireless/sw.c"
    "adapter/wireless/sw2.c"
    "adapter/wireless/wireless.c"
    "bluetooth/host.c"
    "bluetooth/hci.c"
    "bluetooth/l2cap.c"
    "bluetooth/sdp.c"
    "bluetooth/att.c"
    "bluetooth/att_cfg.c"
    "bluetooth/att_hid.c"
    "bluetooth/smp.c"
    "bluetooth/mon.c"
    "bluetooth/hidp/hidp.c"
    "bluetooth/hidp/generic.c"
    "bluetooth/hidp/ps3.c"
    "bluetooth/hidp/wii.c"
    "bluetooth/hidp/ps.c"
    "bluetooth/hidp/sw.c"
    "bluetooth/hidp/sw2.c"
    "system/manager.c"
    "tools/devcrypto.c"
    "tools/ps1_gameid.c"
)

set(srcs
    "adapter/adapter_debug.c"
    "bluetooth/debug.c"
    "system/delay.c"
    "system/fs.c"
    "system/gpio.c"
    "system/intr.c"
    "system/led.c"
    "system/bare_metal_app_cpu.c"
    "system/core0_stall.c"
    "system/core0_stall_highint_hdl.S"
    "system/startup.c"
    "tools/stats.c"
    "wired/wired_bare.c"
    "wired/wired_rtos.c"
)

# Wired drivers, per the system choice. See the BLUERETRO_DRV_* block in
# Kconfig.projbuild -- a UNIVERSAL build turns all of these on, which is the
# old behaviour, while a single-system build compiles only what it uses.
foreach(drv maple nsi npiso_io snes_spi pce_io sega_io cdi_uart real_spi
            jag_io ps_spi pcfx_spi jvs_uart wii_i2c parallel detect)
    string(TOUPPER "${drv}" drv_sym)
    if(CONFIG_BLUERETRO_DRV_${drv_sym})
        list(APPEND srcs "wired/${drv}.c")
    endif()
endforeach()

# The SEA Board is the only user of the FPGA bitstream loader.
if(CONFIG_BLUERETRO_DRV_SEA_IO)
    list(APPEND srcs
        "wired/sea_io.c"
        "system/fpga_config.c"
    )
endif()

if(CONFIG_BLUERETRO_WS_CMDS)
    list(APPEND srcs
        "tests/ws_srv.c"
        "tests/cmds.c"
    )
endif()

if(CONFIG_BLUERETRO_GC_OTA)
    list(APPEND srcs
        "system/gc_ota.c"
    )
endif()

if(CONFIG_BLUERETRO_GC_APP)
    list(APPEND srcs
        "system/gc_app.c"
    )
endif()

if(CONFIG_BLUERETRO_GC_LOG)
    list(APPEND srcs
        "system/gc_log.c"
    )
endif()

if(CONFIG_BLUERETRO_GC_BOOT)
    list(APPEND srcs
        "system/gc_boot.c"
    )
endif()

if(CONFIG_BLUERETRO_GC_CFG)
    list(APPEND srcs
        "system/gc_cfg.c"
    )
endif()

if(CONFIG_BLUERETRO_GC_PAIR)
    list(APPEND srcs
        "system/gc_pair.c"
    )
endif()

if(CONFIG_BLUERETRO_COVERAGE)
    list(APPEND srcs
        "tests/coverage.c"
    )
endif()

idf_component_register(
    SRCS "${srcs}" "${cov_srcs}"
    PRIV_INCLUDE_DIRS "."
    LDFRAGMENTS linker.lf
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-address-of-packed-member)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-builtin-memset)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-builtin-memcpy)

if(CONFIG_BLUERETRO_COVERAGE)
    target_compile_options(${COMPONENT_LIB} PRIVATE -fprofile-info-section)

    set_source_files_properties(
        ${cov_srcs}
        PROPERTIES COMPILE_FLAGS --coverage
    )

    target_link_libraries(${COMPONENT_TARGET} gcov)
endif()

idf_build_set_property(COMPILE_DEFINITIONS "-DBLUERETRO" APPEND)
# S31 is RV32IMAFCP, so the A extension gives us real lr.w/sc.w/amo* across both
# HP cores. Use the GCC __atomic_* builtin path in zephyr/atomic.h rather than
# the Xtensa asm implementation that used to live in zephyr/atomic.S.
idf_build_set_property(COMPILE_DEFINITIONS "-DCONFIG_ATOMIC_OPERATIONS_BUILTIN" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "-DXT_INTEXC_HOOKS" APPEND)

target_link_libraries(${COMPONENT_TARGET} "-u ld_include_my_startup_file")
target_link_libraries(${COMPONENT_TARGET} "-u ld_include_core0_stall_highint_hdl")
