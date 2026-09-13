cmake_minimum_required(VERSION 3.21)
project(ClipLite VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Avoid the min/max macros from <windows.h> clashing with std::min/std::max.
add_compile_definitions(NOMINMAX)

option(CLIPLITE_BUILD_TESTS "Build ClipLite tests" ON)
option(CLIPLITE_ASAN "Instrument media lib + library test with AddressSanitizer" OFF)
option(CLIPLITE_ASAN "Instrument with AddressSanitizer" OFF)

include(GNUInstallDirs)

# Core library: pure C++20 logic, no Win32/GPU dependency so it is unit-testable
# on any toolchain. The native Win32 app target links against this later.
add_library(cliplite_core STATIC
    src/log.cpp
    src/config.cpp
    src/util/string.cpp
    src/hotkeys.cpp
    src/replay/ring_buffer.cpp
    src/library/timeline.cpp
    src/audio/stem_mixer.cpp
    src/audio/stem_writer.cpp
    src/audio/stem_track.cpp
    src/audio/wav_peaks.cpp
    src/audio/mix_limiter.cpp
)

target_include_directories(cliplite_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(cliplite_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(cliplite_core PRIVATE -Wall -Wextra)
endif()

# Media library: graphics (D3D11), encoder (Media Foundation), capture/audio.
add_library(cliplite_media STATIC
    src/graphics/d3d11_device.cpp
    src/graphics/frame_converter.cpp
    src/encoder/media_foundation.cpp
    src/audio/wasapi_loopback.cpp
    src/audio/wasapi_mic.cpp
    src/audio/pcm_decode.cpp
    src/audio/session_monitor.cpp
    src/audio/process_loopback.cpp
    src/capture/dxgi_capture.cpp
    src/replay/clip_remux.cpp
    src/replay/replay_recorder.cpp
    src/replay/recovery.cpp
    src/detection/game_namer.cpp
    src/detection/window_info.cpp
    src/detection/process_classifier.cpp
    src/detection/game_detector.cpp
    src/detection/capture_selector.cpp
    src/library/clip_library.cpp
    src/library/thumbnails.cpp
    src/library/export_clip.cpp
    src/library/text_overlay.cpp
    src/library/audio_peaks.cpp
    src/library/stem_mixdown.cpp
    src/library/video_edit.cpp
    src/player/video_player.cpp
)
target_link_libraries(cliplite_media PUBLIC
    cliplite_core
    d3d11 dxgi mf mfplat mfreadwrite mfuuid ole32 mmdevapi version
)
target_include_directories(cliplite_media PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(MSVC)
    target_compile_options(cliplite_media PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(cliplite_media PRIVATE -Wall -Wextra)
endif()

# Native Win32 application: hidden message window, system tray, global hotkey.
add_executable(cliplite_app WIN32
    src/app/main.cpp
    src/app/tray.cpp
    src/app/startup.cpp
    src/ui/webview_window.cpp
    src/ui/settings_window.cpp
    assets/cliplite.rc
)
set_target_properties(cliplite_app PROPERTIES OUTPUT_NAME "ClipLite")
target_include_directories(cliplite_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2
)
target_link_libraries(cliplite_app PRIVATE cliplite_core cliplite_media shell32 ole32 comctl32 dwmapi advapi32 uxtheme
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/x64/WebView2LoaderStatic.lib)
target_compile_definitions(cliplite_app PRIVATE UNICODE _UNICODE)
if(MSVC)
    target_compile_options(cliplite_app PRIVATE /W4 /permissive- /EHsc)
    # Our .rc embeds the manifest; disable the linker's auto-generated one.
    target_link_options(cliplite_app PRIVATE "/MANIFEST:NO")
else()
    target_compile_options(cliplite_app PRIVATE -Wall -Wextra)
endif()

# Ship the web UI next to the exe for dev runs and installs.
add_custom_command(TARGET cliplite_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/web $<TARGET_FILE_DIR:cliplite_app>/web
)
install(DIRECTORY assets/web/ DESTINATION web)

# A per-user install places the executable in a stable location and creates a
# Start Menu shortcut so Windows Search can discover ClipLite like a normal app.
install(TARGETS cliplite_app
    RUNTIME DESTINATION .
)
install(FILES assets/cliplite.ico DESTINATION .)
if(WIN32)
    install(SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_windows.cmake")
endif()

if(CLIPLITE_BUILD_TESTS)
    enable_testing()
    add_executable(cliplite_tests
        tests/test_main.cpp
        tests/test_ring_buffer.cpp
        tests/test_config.cpp
        tests/test_string.cpp
        tests/test_hotkeys.cpp
        tests/test_timeline.cpp
        tests/test_stem_mixer.cpp
        tests/test_stem_writer.cpp
        tests/test_stem_track.cpp
        tests/test_wav_peaks.cpp
        tests/test_mix_limiter.cpp
    )
    target_link_libraries(cliplite_tests PRIVATE cliplite_core)
    target_include_directories(cliplite_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    if(MSVC)
        target_compile_options(cliplite_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(cliplite_tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME cliplite_tests COMMAND cliplite_tests)

    add_executable(cliplite_media_test
        tests/media_selftest.cpp
    )
    target_link_libraries(cliplite_media_test PRIVATE cliplite_media)
    target_include_directories(cliplite_media_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    if(MSVC)
        target_compile_options(cliplite_media_test PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(cliplite_media_test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME cliplite_media_test COMMAND cliplite_media_test)

    add_executable(cliplite_audio_test
        tests/audio_selftest.cpp
    )
    target_link_libraries(cliplite_audio_test PRIVATE cliplite_media)
    if(MSVC)
        target_compile_options(cliplite_audio_test PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME cliplite_audio_test COMMAND cliplite_audio_test)

    add_executable(cliplite_dxgi_test
        tests/dxgi_selftest.cpp
    )
    target_link_libraries(cliplite_dxgi_test PRIVATE cliplite_media)
    if(MSVC)
        target_compile_options(cliplite_dxgi_test PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME cliplite_dxgi_test COMMAND cliplite_dxgi_test)

    add_executable(cliplite_replay_test
        tests/replay_selftest.cpp
    )
    target_link_libraries(cliplite_replay_test PRIVATE cliplite_media)
    if(MSVC)
        target_compile_options(cliplite_replay_test PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME cliplite_replay_test COMMAND cliplite_replay_test)

    add_executable(cliplite_detection_test
        tests/detection_selftest.cpp
    )
    target_link_libraries(cliplite_detection_test PRIVATE cliplite_media)
    if(MSVC)
        target_compile_options(cliplite_detection_test PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME cliplite_detection_test COMMAND cliplite_detection_test)

    add_executable(cliplite_library_test
        tests/library_selftest.cpp
    )
    target_link_libraries(cliplite_library_test PRIVATE cliplite_media)
    if(CLIPLITE_ASAN AND MSVC)
        # ASan must also instrument cliplite_media (where video_edit lives).
        target_compile_options(cliplite_media PRIVATE /fsanitize=address)
        target_link_options(cliplite_library_test PRIVATE /INCREMENTAL:NO)
    endif()
    if(MSVC)
        target_compile_options(cliplite_library_test PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME cliplite_library_test COMMAND cliplite_library_test)

    add_executable(cliplite_player_test
        tests/player_selftest.cpp
    )
    target_link_libraries(cliplite_player_test PRIVATE cliplite_media)
    if(MSVC)
        target_compile_options(cliplite_player_test PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME cliplite_player_test COMMAND cliplite_player_test)

    add_executable(cliplite_recovery_test
        tests/recovery_selftest.cpp
    )
    target_link_libraries(cliplite_recovery_test PRIVATE cliplite_media)
    if(MSVC)
        target_compile_options(cliplite_recovery_test PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME cliplite_recovery_test COMMAND cliplite_recovery_test)

    add_executable(cliplite_session_test
        tests/session_selftest.cpp
    )
    target_link_libraries(cliplite_session_test PRIVATE cliplite_media)
    if(MSVC)
        target_compile_options(cliplite_session_test PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME cliplite_session_test COMMAND cliplite_session_test)

    add_executable(cliplite_process_loopback_test
        tests/process_loopback_selftest.cpp
    )
    target_link_libraries(cliplite_process_loopback_test PRIVATE cliplite_media)
    if(MSVC)
        target_compile_options(cliplite_process_loopback_test PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME cliplite_process_loopback_test COMMAND cliplite_process_loopback_test)

    add_executable(cliplite_stem_mixdown_test
        tests/stem_mixdown_selftest.cpp
    )
    target_link_libraries(cliplite_stem_mixdown_test PRIVATE cliplite_media)
    if(MSVC)
        target_compile_options(cliplite_stem_mixdown_test PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME cliplite_stem_mixdown_test COMMAND cliplite_stem_mixdown_test)

    add_executable(cliplite_peaks_test
        tests/peaks_selftest.cpp
    )
    target_link_libraries(cliplite_peaks_test PRIVATE cliplite_media)
    if(MSVC)
        target_compile_options(cliplite_peaks_test PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME cliplite_peaks_test COMMAND cliplite_peaks_test)
endif()
