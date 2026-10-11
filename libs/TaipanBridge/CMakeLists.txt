# SimtaryScript: Taipan (libs/Taipan) + its exception-free bridge (StTaipan.cpp).
#
# A SUBDIRECTORY on purpose, added from the top-level CMakeLists.txt BEFORE the
# project-wide flags. add_compile_definitions() reaches every target in its own
# directory whenever that target was declared, so declaring this library next to the
# flags - even above them - would still compile it with _HAS_EXCEPTIONS=0. A
# subdirectory takes a copy of the parent's directory properties at add_subdirectory()
# time, which is the only placement that keeps them out (the same reason sentry and
# libzmq come in through add_subdirectory).
#
# Only this directory is PUBLIC, so framework code sees StTaipan.h - a POD,
# non-throwing surface - and cannot reach taipan.h by accident.
set(_tp "${CMAKE_CURRENT_SOURCE_DIR}/../Taipan")

add_library(SimtaryScript STATIC
    ${_tp}/src/analysis.cpp
    ${_tp}/src/api.cpp
    ${_tp}/src/bindings.cpp
    ${_tp}/src/compiler.cpp
    ${_tp}/src/diag.cpp
    ${_tp}/src/gc.cpp
    ${_tp}/src/lexer.cpp
    ${_tp}/src/module.cpp
    ${_tp}/src/object.cpp
    ${_tp}/src/parser.cpp
    ${_tp}/src/stdlib.cpp
    ${_tp}/src/vm.cpp
    StTaipan.cpp
    StTaipan.h
    StTaipanInternal.h
    StTaipanMath.cpp
)
target_include_directories(SimtaryScript
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${_tp}/include ${_tp}/src
)
set_target_properties(SimtaryScript PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Simtary/ThirdParty"
)
if (MSVC)
    # /utf-8: Taipan's diagnostics draw with box characters. Warnings off as for the
    # other vendored code.
    target_compile_options(SimtaryScript PRIVATE /EHsc /utf-8 /w)
else()
    target_compile_options(SimtaryScript PRIVATE -fexceptions -w)
endif()

unset(_tp)
