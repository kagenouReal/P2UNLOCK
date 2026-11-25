cmake_minimum_required(VERSION 3.20)

project(P2UNLOCK VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(pseriescore
    src/core/types.cpp
    src/core/recovery.cpp
    src/core/mcfg.cpp
    src/core/pcie_capability.cpp
    src/core/device_selection.cpp
    src/core/pci_identity.cpp
    src/core/p104_plan.cpp
    src/core/profile.cpp
)

target_include_directories(pseriescore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(MSVC)
    target_compile_options(pseriescore PRIVATE /W4 /permissive-)
else()
    target_compile_options(pseriescore PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    add_library(pserieswindows STATIC
        platform/windows/cuda_verifier.cpp
        platform/windows/seavo_backend.cpp
        platform/windows/target_topology.cpp
        platform/windows/firmware_table.cpp
        platform/windows/pci_config.cpp
        platform/windows/pci_discovery.cpp
        platform/windows/pnp_device.cpp
        platform/windows/p104_executor.cpp
        platform/windows/profile_store.cpp
        platform/windows/service_install.cpp
        platform/windows/service_ipc.cpp
        platform/windows/service_host.cpp
        platform/windows/transaction_journal_store.cpp
    )
    target_include_directories(pserieswindows PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(pserieswindows PRIVATE pseriescore bcrypt advapi32 setupapi cfgmgr32)
    if(MSVC)
        target_compile_options(pserieswindows PRIVATE /W4 /permissive-)
    else()
        target_compile_options(pserieswindows PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_subdirectory(apps/cli)
endif()
