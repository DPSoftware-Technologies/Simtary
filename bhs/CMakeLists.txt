# BHS - Blackhawk Streaming. Standalone: nothing here includes Simtary headers.
# Build:    pwsh Simtary/bhs/tools/build_bhs.ps1     (configure + build + install into Simtary/deps/bhs-install)
# or by hand: cmake -S Simtary/bhs -B build/bhs -G "Visual Studio 18 2026" -A x64
#             cmake --build build/bhs --config Release
#
# Dependencies live in Simtary/deps (git clones / prebuilt, not committed):
#   libdatachannel  (MPL-2.0)    git clone --recurse-submodules https://github.com/paullouisageneau/libdatachannel
#   openssl-msys    (Apache-2.0) OpenSSL 3.x DLLs from msys2 UCRT64 + import libs: tools/setup_openssl_msys.ps1
cmake_minimum_required(VERSION 3.20)
project(BlackhawkStreaming LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(BHS_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../deps" CACHE PATH "Simtary/deps")

if(WIN32)
    # Milestone 1: test pattern -> hardware H.264 (Media Foundation) -> MP4.
    add_executable(bhs_encode_test tools/encode_test/main.cpp)
    target_compile_definitions(bhs_encode_test PRIVATE UNICODE _UNICODE)
    target_link_libraries(bhs_encode_test PRIVATE d3d11 dxgi d3dcompiler mfplat mfreadwrite mfuuid mf)

    # Which D3D12 resources can D3D11 open through a shared handle (the engine hands frames over this way).
    add_executable(bhs_share_test tools/share_test/main.cpp)
    target_link_libraries(bhs_share_test PRIVATE d3d11 d3d12 dxgi)

    # The node library (bhs.dll, C interface in include/bhs.h) and a test host for it.
    # TLS for DTLS-SRTP is OpenSSL: Mbed TLS cannot complete the browser's DTLS handshake.
    if(EXISTS "${BHS_DEPS}/libdatachannel/CMakeLists.txt" AND EXISTS "${BHS_DEPS}/openssl-msys/lib/libssl-3-x64.lib")
        set(OPENSSL_ROOT_DIR "${BHS_DEPS}/openssl-msys")
        set(OPENSSL_INCLUDE_DIR "${BHS_DEPS}/openssl-msys/include")
        set(USE_MBEDTLS OFF)
        set(NO_EXAMPLES ON)
        set(NO_TESTS ON)
        set(BUILD_SHARED_LIBS OFF)
        add_subdirectory("${BHS_DEPS}/libdatachannel" "${CMAKE_BINARY_DIR}/libdatachannel" EXCLUDE_FROM_ALL)

        add_library(bhs SHARED src/node.cpp)
        target_compile_definitions(bhs PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
        target_include_directories(bhs PUBLIC include)
        target_include_directories(bhs PRIVATE "${BHS_DEPS}/libdatachannel/deps/json/include")
        target_link_libraries(bhs PRIVATE LibDataChannel::LibDataChannelStatic
            d3d11 dxgi mfplat mfuuid mf strmiids ole32 oleaut32)

        add_executable(bhs_stream_test tools/stream_test/main.cpp)
        target_compile_definitions(bhs_stream_test PRIVATE UNICODE _UNICODE)
        target_link_libraries(bhs_stream_test PRIVATE bhs d3d11 dxgi d3dcompiler)
        add_custom_command(TARGET bhs_stream_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" "$<TARGET_FILE_DIR:bhs_stream_test>/index.html")
        foreach(_t bhs bhs_stream_test)
            add_custom_command(TARGET ${_t} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BHS_DEPS}/openssl-msys/bin/libssl-3-x64.dll" "$<TARGET_FILE_DIR:${_t}>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BHS_DEPS}/openssl-msys/bin/libcrypto-3-x64.dll" "$<TARGET_FILE_DIR:${_t}>")
        endforeach()

        # What an engine links: bhs.dll + bhs.lib + bhs.h (+ the two OpenSSL DLLs beside it).
        install(TARGETS bhs RUNTIME DESTINATION bin ARCHIVE DESTINATION lib)
        install(FILES include/bhs.h DESTINATION include)
        install(FILES "${BHS_DEPS}/openssl-msys/bin/libssl-3-x64.dll" "${BHS_DEPS}/openssl-msys/bin/libcrypto-3-x64.dll" DESTINATION bin)
        install(FILES web/index.html DESTINATION web)
    else()
        message(STATUS "bhs.dll skipped: clone libdatachannel and set up deps/openssl-msys in ${BHS_DEPS}")
    endif()
endif()
