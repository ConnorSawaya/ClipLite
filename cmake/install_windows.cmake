if(NOT WIN32)
    return()
endif()

set(_start_menu_dir "$ENV{APPDATA}/Microsoft/Windows/Start Menu/Programs")
set(_shortcut_path "${_start_menu_dir}/ClipLite.lnk")
set(_target_path "${CMAKE_INSTALL_PREFIX}/ClipLite.exe")
set(_icon_path "${CMAKE_INSTALL_PREFIX}/cliplite.ico")

file(MAKE_DIRECTORY "${_start_menu_dir}")

execute_process(
    COMMAND powershell.exe
        -NoProfile
        -NonInteractive
        -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_LIST_DIR}/create_start_menu_shortcut.ps1"
        -ShortcutPath "${_shortcut_path}"
        -TargetPath "${_target_path}"
        -WorkingDirectory "${CMAKE_INSTALL_PREFIX}"
        -IconPath "${_icon_path}"
    RESULT_VARIABLE _shortcut_result
    OUTPUT_VARIABLE _shortcut_output
    ERROR_VARIABLE _shortcut_error
)

if(NOT _shortcut_result EQUAL 0)
    message(WARNING "Could not create the ClipLite Start Menu shortcut: ${_shortcut_error}")
endif()
