cmake_minimum_required(VERSION 3.20)
project(MultiCopySystem LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Multi-Copy-System requires Windows.")
endif()

enable_language(RC)

add_executable(MultiCopySystem WIN32
    src/main.cpp
    src/input.cpp
    src/picker.cpp
    src/caret.cpp
    src/clipboard.cpp
    src/storage.cpp
)

add_executable(MultiCopySettings WIN32
    src/settings_main.cpp
    src/clipboard.cpp
    src/storage.cpp
)

foreach(target IN ITEMS MultiCopySystem MultiCopySettings)
    target_sources(${target} PRIVATE src/app.rc)
    target_include_directories(${target} PRIVATE src)
    set_target_properties(${target} PROPERTIES
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8>")
    endif()
    target_link_libraries(${target} PRIVATE user32 gdi32 shell32 advapi32)
endforeach()

target_link_libraries(MultiCopySystem PRIVATE ole32 oleaut32)
target_link_libraries(MultiCopySettings PRIVATE comctl32)
