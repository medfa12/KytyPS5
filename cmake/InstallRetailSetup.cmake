# Explicit runtime-tool allowlist: no research tools, firmware or local stores.
set(_retail_tools
    setup_retail.py local_paths.py extract_agc_register_data.py
    run_shellui_1360.ps1 prepare_shellui_managed_1360.py
    prepare_shellui_first_boot_1360.py prepare_shellui_databases_1360.py
    prepare_shellui_urlconfig.ps1 shellui_native_producer_mounts.ps1
    monitor_shellui_run.ps1 watch_shellui_frames.ps1)
foreach(_tool IN LISTS _retail_tools)
    install(FILES "${CMAKE_CURRENT_LIST_DIR}/../tools/${_tool}" DESTINATION tools)
endforeach()
install(FILES "${CMAKE_CURRENT_LIST_DIR}/../tools/RETAIL_SETUP.md" DESTINATION .)
