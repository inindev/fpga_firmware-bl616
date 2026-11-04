# Write the firmware version shown in the menu footer to OUT, as FW_VERSION:
# "<short sha>[+] <commit date>", e.g. "a1b2c3d 2026-11-03" (+ when there are uncommitted
# changes). Run on every build, with -DSRC=<source dir> -DOUT=<header>.
#
# The VERSION environment variable (make VERSION=...) is used instead when set: a build
# host that gets the tree by rsync has no matching git history.

if(DEFINED ENV{VERSION} AND NOT "$ENV{VERSION}" STREQUAL "")
    set(version "$ENV{VERSION}")
else()
    execute_process(COMMAND git rev-parse --short=7 HEAD
        WORKING_DIRECTORY ${SRC} OUTPUT_VARIABLE sha
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE r)
    if(NOT r EQUAL 0)
        set(sha "unknown")
    else()
        execute_process(COMMAND git update-index -q --refresh WORKING_DIRECTORY ${SRC}
            OUTPUT_QUIET ERROR_QUIET)
        execute_process(COMMAND git diff-index --quiet HEAD -- WORKING_DIRECTORY ${SRC}
            RESULT_VARIABLE dirty)
        if(NOT dirty EQUAL 0)
            set(sha "${sha}+")
        endif()
    endif()
    execute_process(COMMAND git log -1 --format=%cs
        WORKING_DIRECTORY ${SRC} OUTPUT_VARIABLE date
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    set(version "${sha} ${date}")
endif()

# rewrite only when changed, so that an unchanged version does not rebuild main.cpp
file(WRITE ${OUT}.tmp "#define FW_VERSION \"${version}\"\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OUT}.tmp ${OUT})
file(REMOVE ${OUT}.tmp)
