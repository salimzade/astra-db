add_executable(astra_tests
    test_main.c
    test_types.c
    test_error.c
    test_allocator.c
    test_log.c
    test_config.c
)

target_link_libraries(astra_tests PRIVATE astradb)

# The tests link the static library directly and additionally include one private
# header, src/core/log_internal.h, which redirects the log sink so that output
# can be asserted on. Nothing else in src/ is reachable this way.
target_include_directories(astra_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

set_target_properties(astra_tests PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

astra_target_warnings(astra_tests)
astra_target_sanitize(astra_tests)

# One registration, because there is only one binary: whether it is sanitized is
# decided at configure time by ASTRA_SANITIZE, not by the test itself. Running
# it twice would only ever execute identical code twice.
add_test(NAME astra_tests COMMAND astra_tests)

if(DEFINED ASTRA_SANITIZE AND NOT ASTRA_SANITIZE STREQUAL "" AND NOT ASTRA_SANITIZE STREQUAL "none")
    set_tests_properties(astra_tests PROPERTIES LABELS "sanitizer")

    # LeakSanitizer ships with AddressSanitizer on Linux and macOS only. Asking
    # for it on Windows makes ASan exit non-zero with "detect_leaks is not
    # supported on this platform", which would fail an otherwise clean run.
    if(NOT WIN32)
        set_tests_properties(astra_tests PROPERTIES
            ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:strict_string_checks=1")
    endif()
endif()