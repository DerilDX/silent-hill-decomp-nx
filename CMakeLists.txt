file(GLOB_RECURSE PSYCROSS_SRCS_C
	"*.c"
)

file(GLOB_RECURSE PSYCROSS_SRCS_CPP
	"*.cpp"
)
list(FILTER PSYCROSS_SRCS_CPP EXCLUDE REGEX "/tests/")
list(REMOVE_ITEM PSYCROSS_SRCS_CPP
	"${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PsyX_SPUAL.cpp")

# Exclude incompatible glad source for each platform
if(SH_SWITCH)
    message(STATUS "glad filter: found .c files: ${PSYCROSS_SRCS_C}")
    list(FILTER PSYCROSS_SRCS_C EXCLUDE REGEX ".*glad\\.c$")
    message(STATUS "glad filter: after exclusion: ${PSYCROSS_SRCS_C}")
else()
    message(STATUS "glad_switch filter: found .c files: ${PSYCROSS_SRCS_C}")
    list(FILTER PSYCROSS_SRCS_C EXCLUDE REGEX ".*glad_switch\\.c$")
    message(STATUS "glad_switch filter: after exclusion: ${PSYCROSS_SRCS_C}")
endif()

set_source_files_properties( ${PSYCROSS_SRCS_C} PROPERTIES LANGUAGE C)
set_source_files_properties( ${PSYCROSS_SRCS_CPP} PROPERTIES LANGUAGE CXX)

# C11 uses _Static_assert, but PsyCross headers use C++ style static_assert
# Add compatibility define for C compilation
set_source_files_properties( ${PSYCROSS_SRCS_C} PROPERTIES
    COMPILE_DEFINITIONS "static_assert=_Static_assert"
)

# AppleClang treats an uppercase .C extension as C++ regardless of the LANGUAGE
# property. Force C mode explicitly so -std=gnu11 is accepted.
if(APPLE)
    set_source_files_properties( ${PSYCROSS_SRCS_C} PROPERTIES COMPILE_FLAGS "-x c")
endif()
#set_source_files_properties( "src/gte/PsyX_GTE.cpp" PROPERTIES LANGUAGE C)

add_library(psycross_static STATIC ${PSYCROSS_SRCS_C} ${PSYCROSS_SRCS_CPP})
set_target_properties(psycross_static PROPERTIES OUTPUT_NAME "psycross")
# "include/psx" is deliberately NOT public: it holds a PSX-SDK <strings.h> that
# is a guard-only stub declaring nothing, so putting it on the include path
# shadows the POSIX header for every TU built against this archive (that is why
# strcasecmp used to come up undeclared on glibc). Consumers that need bare
# <libgpu.h>-style PSX includes add the directory themselves.
target_include_directories(psycross_static PUBLIC "include")

# On ELF the host executable that links this archive is built with
# -rdynamic (so map overlay .so's can resolve game symbols at dlopen). That
# also exports every default-visibility global, and two TUs here define
# symbols whose names collide with libc and libopenal:
#   - libapi.c implements PSX file I/O as open/read/close/write/lseek/ioctl/
#     rename. Exporting these interposed libc for ALL loaded libraries —
#     Mesa's read() of the GPU's PCI sysfs `vendor` node during GL context
#     creation was dispatched to the game's read() and hung startup.
#   - PsyX_SPULegacy.cpp includes PsyX_SPUAL.cpp and holds alGenEffects/... as
#     global function-pointer
#     variables, which interposed libopenal's EFX functions.
# -rdynamic cannot export hidden-visibility symbols, so compile just these
# TUs hidden. They're still used normally within the module; nothing
# external needs them (the maps link their own copy of this archive).
if(NOT WIN32)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/psx/libapi.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PsyX_SPULegacy.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PsyX_SPUSoftware.cpp"
        PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden")
endif()

# For Switch (devkitPro), SDL2 is provided without CMake config files
# Use the pre-set variables from the toolchain
if(DEFINED SDL2_LIBRARY)
    target_link_libraries(psycross_static PRIVATE ${SDL2_LIBRARY})
    target_include_directories(psycross_static PRIVATE ${SDL2_INCLUDE_DIR} ${SDL2_INCLUDE_DIR}/SDL2)
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(psycross_static PRIVATE ${SDL2_LIBRARIES})
    target_include_directories(psycross_static PRIVATE ${SDL2_INCLUDE_DIRS})

    find_package(OpenAL REQUIRED)
    if(TARGET OpenAL::OpenAL)
        target_link_libraries(psycross_static PRIVATE OpenAL::OpenAL)
    elseif(TARGET OpenAL)
        target_link_libraries(psycross_static PRIVATE OpenAL)
    else()
        target_link_libraries(psycross_static PRIVATE ${OPENAL_LIBRARY})
        target_include_directories(psycross_static PRIVATE ${OPENAL_INCLUDE_DIR})
    endif()
endif()

# For Switch (devkitPro), OpenAL is provided without CMake config files
# Use the pre-set variables from the toolchain
if(DEFINED OPENAL_LIBRARY)
    target_link_libraries(psycross_static PRIVATE ${OPENAL_LIBRARY})
    target_include_directories(psycross_static PRIVATE ${OPENAL_INCLUDE_DIR})
else()
    find_package(OpenAL REQUIRED)
    # find_package(OpenAL) exposes different handles depending on how OpenAL was
    # found: the imported target OpenAL::OpenAL (openal-soft's CMake config, or
    # CMake's FindOpenAL in 3.x), a bare `OpenAL` target (some MSYS2 configs), or
    # just the OPENAL_LIBRARY / OPENAL_INCLUDE_DIR variables. Prefer the imported
    # target; fall back so this links on both MinGW and Linux.
    if(TARGET OpenAL::OpenAL)
        target_link_libraries(psycross_static PRIVATE OpenAL::OpenAL)
    elseif(TARGET OpenAL)
        target_link_libraries(psycross_static PRIVATE OpenAL)
    else()
        target_link_libraries(psycross_static PRIVATE ${OPENAL_LIBRARY})
        target_include_directories(psycross_static PRIVATE ${OPENAL_INCLUDE_DIR})
    endif()
endif()
if(OpenAL_SOURCE_DIR)
    target_include_directories(psycross_static PRIVATE ${OpenAL_BINARY_DIR} ${OpenAL_SOURCE_DIR}/include)
endif()
if(OPENAL_INCLUDE_DIR)
    target_include_directories(psycross_static PRIVATE ${OPENAL_INCLUDE_DIR})
endif()

if(WIN32)
    target_link_libraries(psycross_static ole32 ksguid avrt propsys)
endif()

if(BUILD_TESTING)
    # Without this add_test() registers nothing and ctest reports "No tests
    # were found" — PsyCross has no project() of its own.
    enable_testing()

    # Each test file carries its own main(), so each builds as its own binary
    # against just the DSP translation units it exercises — none of them link
    # psycross_static, so they stay free of SDL/OpenAL/GL.
    set(PSYCROSS_SPU_CORE_SRCS
        src/audio/PsyX_SPUCore.cpp
        src/audio/PsyX_SPUReverb.cpp
        src/audio/PsyX_IdealReverb.cpp
        src/audio/PsyX_ReferenceReverb.cpp
        src/audio/PsyX_ReferenceResampler.cpp)

    function(psycross_add_audio_test name)
        add_executable(${name} ${ARGN})
        target_include_directories(${name} PRIVATE
            "include" "include/psx" "src" "src/audio")
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    psycross_add_audio_test(psycross_spu_dispatch_test
        tests/audio/PsyX_SPUDispatch_test.cpp
        src/audio/PsyX_SPUDispatch.cpp)
    psycross_add_audio_test(psycross_spu_core_test
        tests/audio/PsyX_SPUCore_test.cpp ${PSYCROSS_SPU_CORE_SRCS})
    psycross_add_audio_test(psycross_spu_enhanced_test
        tests/audio/PsyX_SPUEnhanced_test.cpp ${PSYCROSS_SPU_CORE_SRCS})
    psycross_add_audio_test(psycross_ideal_reverb_test
        tests/audio/PsyX_IdealReverb_test.cpp src/audio/PsyX_IdealReverb.cpp)
    psycross_add_audio_test(psycross_reference_reverb_test
        tests/audio/PsyX_ReferenceReverb_test.cpp src/audio/PsyX_ReferenceReverb.cpp)
    psycross_add_audio_test(psycross_reference_resampler_test
        tests/audio/PsyX_ReferenceResampler_test.cpp src/audio/PsyX_ReferenceResampler.cpp)
    psycross_add_audio_test(psycross_reference_xa_test
        tests/audio/PsyX_ReferenceXA_test.cpp
        src/audio/PsyX_ReferenceXA.cpp src/audio/PsyX_ReferenceResampler.cpp)
    psycross_add_audio_test(psycross_xastream_fidelity_test
        tests/audio/PsyX_XAStream_fidelity_test.cpp src/audio/PsyX_XAStream.cpp)
endif()
