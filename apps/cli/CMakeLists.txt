add_executable(P2UNLOCK main.cpp)

target_link_libraries(P2UNLOCK PRIVATE pseriescore pserieswindows)
target_compile_definitions(P2UNLOCK PRIVATE UNICODE _UNICODE)

if(MSVC)
    target_compile_options(P2UNLOCK PRIVATE /W4 /permissive-)
else()
    target_compile_options(P2UNLOCK PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MINGW)
    target_link_options(P2UNLOCK PRIVATE -municode)
    get_filename_component(compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(runtime_dll libc++.dll libunwind.dll libwinpthread-1.dll)
        if(EXISTS "${compiler_bin}/${runtime_dll}")
            add_custom_command(TARGET P2UNLOCK POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${compiler_bin}/${runtime_dll}"
                    "$<TARGET_FILE_DIR:P2UNLOCK>/${runtime_dll}")
        endif()
    endforeach()
endif()
