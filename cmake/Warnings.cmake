# Compiler configuration shared by every AstraDB target.
#
# Two functions live here:
#
#   astra_target_warnings(target)   strict warning set, optionally as errors
#   astra_target_sanitize(target)   AddressSanitizer / UndefinedBehaviorSanitizer
#
# The warning list must stay identical for the library, the CLI and the tests so
# that no translation unit is ever compiled with a weaker configuration than its
# neighbours. A warning that is fixed for one target must be fixed for all.

# Warning groups that are enabled on every target.
set(ASTRA_WARNINGS_GCC
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wcast-qual
    -Wwrite-strings
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wmissing-declarations
    -Wold-style-definition
    -Wredundant-decls
    -Wnested-externs
    -Wpointer-arith
    -Wundef
    -Wvla
    -Wformat=2
    -Wswitch-default
    -Winit-self
    -Wstrict-overflow=2
    -Wnull-dereference
    -Wdouble-promotion
    -Wimplicit-fallthrough
    -Wcast-align
    -Wformat-nonliteral
    -Wswitch-enum
    CACHE INTERNAL "AstraDB strict warning set"
)

function(astra_target_warnings target)
    if(MSVC)
        set(_astra_opts
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /utf-8
            /wd4996   # target: we deliberately do not use the CRT's "secure" variants
            /wd5105   # unreferenced formal parameter is fine for documented stubs
        )
        # C11 <stdatomic.h> sits behind an opt-in switch on MSVC. Without it the
        # header is a hard #error, so this is a requirement rather than a nicety.
        # Probed once per configure and cached; ASTRA_MSAN_C11_ATOMICS is set by
        # the probe below.
        if(DEFINED ASTRA_MSAN_HAS_C11_ATOMICS AND ASTRA_MSAN_HAS_C11_ATOMICS)
            list(APPEND _astra_opts /experimental:c11atomics)
        endif()
        if(ASTRA_WERROR)
            list(APPEND _astra_opts /WX)
        endif()
        target_compile_options(${target} PRIVATE ${_astra_opts})
    else()
        set(_astra_opts ${ASTRA_WARNINGS_GCC})
        if(ASTRA_WERROR)
            list(APPEND _astra_opts -Werror)
        endif()
        target_compile_options(${target} PRIVATE ${_astra_opts})
    endif()
endfunction()

# Applies the sanitizers requested through the ASTRA_SANITIZE cache variable.
#
# Accepted values: none, address, undefined, thread, and the combinations
# address+undefined and address+thread. Anything else is a configuration error
# rather than a silently ignored typo.
#
# ThreadSanitizer is a separate instrument from the other two and cannot be combined
# with AddressSanitizer, so `address+thread` is deliberately not offered: asking for both
# produces binaries that report neither. Use one or the other, in two build directories.
function(astra_target_sanitize target)
    if(NOT DEFINED ASTRA_SANITIZE OR ASTRA_SANITIZE STREQUAL "" OR ASTRA_SANITIZE STREQUAL "none")
        return()
    endif()

    string(TOLOWER "${ASTRA_SANITIZE}" _astra_sanitize)

    if(_astra_sanitize STREQUAL "address" OR _astra_sanitize STREQUAL "address+undefined")
        set(_astra_want_address TRUE)
    endif()
    if(_astra_sanitize STREQUAL "undefined" OR _astra_sanitize STREQUAL "address+undefined")
        set(_astra_want_ub TRUE)
    endif()
    if(_astra_sanitize STREQUAL "thread")
        set(_astra_want_tsan TRUE)
    endif()

    if(NOT _astra_want_address AND NOT _astra_want_ub AND NOT _astra_want_tsan)
        message(FATAL_ERROR
            "ASTRA_SANITIZE='${ASTRA_SANITIZE}' is not recognised. Use one of: "
            "none, address, undefined, address+undefined, thread.")
    endif()

    if(MSVC)
        # MSVC offers AddressSanitizer only; UBSan and TSan are not available for it.
        if(_astra_want_ub AND NOT _astra_want_address)
            message(FATAL_ERROR "MSVC supports only ASTRA_SANITIZE=address, not 'undefined'.")
        endif()
        if(_astra_want_tsan)
            message(FATAL_ERROR "MSVC does not provide ThreadSanitizer; use GCC or Clang.")
        endif()
        if(_astra_want_address)
            # /Zi as well as /fsanitize=address: MSVC emits C5072 when ASan is
            # enabled without debug information, because without symbols an
            # ASan report is close to useless. That is a warning, and warnings
            # are errors here.
            target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
            target_link_options(${target} PRIVATE /fsanitize=address)
        endif()
    else()
        if(_astra_want_tsan)
            # TSan stands alone. Combining it with ASan is not a supported pairing on
            # any of the compilers this project targets, so a request for both is refused
            # at configure time rather than producing a binary that detects neither.
            if(_astra_want_address OR _astra_want_ub)
                message(FATAL_ERROR
                    "ASTRA_SANITIZE='${ASTRA_SANITIZE}' asks for ThreadSanitizer together with "
                    "another sanitizer, which cannot be instrumented together. Build them in "
                    "separate build directories: -DASTRA_SANITIZE=thread and "
                    "-DASTRA_SANITIZE=address+undefined.")
            endif()
            target_compile_options(${target} PRIVATE -fsanitize=thread)
            target_link_options(${target} PRIVATE -fsanitize=thread)
            return()
        endif()

        if(_astra_want_address)
            list(APPEND _astra_sanitize_opts -fsanitize=address)
        endif()
        if(_astra_want_ub)
            list(APPEND _astra_sanitize_opts
                -fsanitize=undefined
                -fno-sanitize-recover=all)
        endif()
        # Frame pointers keep sanitizer stack traces readable; performance is
        # irrelevant for instrumented builds.
        list(APPEND _astra_sanitize_opts -fno-omit-frame-pointer -g)

        target_compile_options(${target} PRIVATE ${_astra_sanitize_opts})
        target_link_options(${target} PRIVATE ${_astra_sanitize_opts})
    endif()
endfunction()
