# Copyright (c) 2026 Advanced Micro Devices, Inc.
# SPDX-License-Identifier: MIT

if(NOT DEFINED TEST_EXECUTABLE)
    message(FATAL_ERROR "TEST_EXECUTABLE is required")
endif()
if(NOT DEFINED TEST_RUNTIME_DIR)
    message(FATAL_ERROR "TEST_RUNTIME_DIR is required")
endif()
if(NOT DEFINED TEST_OUTPUT_DIR)
    message(FATAL_ERROR "TEST_OUTPUT_DIR is required")
endif()

file(REMOVE_RECURSE "${TEST_OUTPUT_DIR}")
file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}")

set(_rank_script "${TEST_OUTPUT_DIR}/rank.sh")
file(
    WRITE "${_rank_script}"
    [=[
#!/bin/sh
if [ "$RANK" != 0 ]; then
    sleep 60 &
    printf "%s\n" "$!" > "$RJ_GRID_OUT/worker-$RANK"
    trap 'printf "term\n" > "$RJ_GRID_OUT/term-$RANK"; exit 0' TERM INT
fi
printf "%s\n" "$$" > "$RJ_GRID_OUT/pid-$RANK"
if [ "$RANK" = 0 ]; then
    i=0
    while [ "$i" -lt 100 ]; do
        [ -s "$RJ_GRID_OUT/pid-1" ] && break
        i=$((i + 1))
        sleep 0.1
    done
    exit 42
fi
while :; do
    sleep 1
done
]=]
)

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "ROCJITSU_RUNTIME_DIR=${TEST_RUNTIME_DIR}/peer-failure"
        "${TEST_EXECUTABLE}" --profile mi350x --num-nodes 1 --nproc-per-node 2
        --env "RJ_GRID_OUT=${TEST_OUTPUT_DIR}" -- /bin/sh "${_rank_script}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    TIMEOUT 15
)

if(NOT "${_result}" STREQUAL "42")
    message(
        FATAL_ERROR
        "expected process-grid exit 42, got ${_result}\nstdout:\n${_stdout}\nstderr:\n${_stderr}"
    )
endif()

if(NOT EXISTS "${TEST_OUTPUT_DIR}/term-1")
    message(
        FATAL_ERROR
        "rank 1 did not observe termination after rank 0 failed\nstdout:\n${_stdout}\nstderr:\n${_stderr}"
    )
endif()
if(NOT EXISTS "${TEST_OUTPUT_DIR}/worker-1")
    message(FATAL_ERROR "rank 1 did not record its worker pid")
endif()

file(READ "${TEST_OUTPUT_DIR}/worker-1" _worker_pid)
string(STRIP "${_worker_pid}" _worker_pid)
set(_worker_alive 1)
foreach(_attempt RANGE 0 50)
    execute_process(
        COMMAND /bin/sh -c "kill -0 ${_worker_pid} 2>/dev/null"
        RESULT_VARIABLE _worker_alive
    )
    if(NOT _worker_alive EQUAL 0)
        break()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
endforeach()

if(_worker_alive EQUAL 0)
    message(
        FATAL_ERROR
        "rank 1 background worker survived process-grid termination"
    )
endif()

set(_leader_exit_dir "${TEST_OUTPUT_DIR}/leader-exit")
file(MAKE_DIRECTORY "${_leader_exit_dir}")
set(_leader_exit_script "${_leader_exit_dir}/rank.sh")
file(
    WRITE "${_leader_exit_script}"
    [=[
#!/bin/sh
if [ "$RANK" = 0 ]; then
    sleep 60 &
    printf "%s\n" "$!" > "$RJ_GRID_OUT/worker-0"
    printf "%s\n" "$$" > "$RJ_GRID_OUT/pid-0"
    exit 0
fi
i=0
while [ "$i" -lt 100 ]; do
    [ -s "$RJ_GRID_OUT/worker-0" ] && break
    i=$((i + 1))
    sleep 0.1
done
printf "%s\n" "$$" > "$RJ_GRID_OUT/pid-1"
sleep 0.5
exit 42
]=]
)

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "ROCJITSU_RUNTIME_DIR=${TEST_RUNTIME_DIR}/leader-exit"
        "${TEST_EXECUTABLE}" --profile mi350x --num-nodes 1 --nproc-per-node 2
        --env "RJ_GRID_OUT=${_leader_exit_dir}" -- /bin/sh
        "${_leader_exit_script}"
    RESULT_VARIABLE _leader_result
    OUTPUT_VARIABLE _leader_stdout
    ERROR_VARIABLE _leader_stderr
    TIMEOUT 15
)

if(NOT "${_leader_result}" STREQUAL "42")
    message(
        FATAL_ERROR
        "expected leader-exit grid to report peer failure 42, got ${_leader_result}\nstdout:\n${_leader_stdout}\nstderr:\n${_leader_stderr}"
    )
endif()
if(NOT EXISTS "${_leader_exit_dir}/worker-0")
    message(FATAL_ERROR "rank 0 did not record its worker pid")
endif()
file(READ "${_leader_exit_dir}/worker-0" _leader_worker_pid)
string(STRIP "${_leader_worker_pid}" _leader_worker_pid)
set(_leader_worker_alive 1)
foreach(_attempt RANGE 0 50)
    execute_process(
        COMMAND /bin/sh -c "kill -0 ${_leader_worker_pid} 2>/dev/null"
        RESULT_VARIABLE _leader_worker_alive
    )
    if(NOT _leader_worker_alive EQUAL 0)
        break()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
endforeach()
if(_leader_worker_alive EQUAL 0)
    message(FATAL_ERROR "rank 0 worker survived after its leader exited")
endif()

set(_sigchld_driver "${TEST_OUTPUT_DIR}/sigchld-ignored.sh")
file(
    WRITE "${_sigchld_driver}"
    [=[
#!/bin/sh
trap '' CHLD
exec "$RJ_EXECUTABLE" --profile mi350x --num-nodes 1 --nproc-per-node 2 -- /bin/sh -c 'exit 42'
]=]
)

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "ROCJITSU_RUNTIME_DIR=${TEST_RUNTIME_DIR}/sigchld"
        "RJ_EXECUTABLE=${TEST_EXECUTABLE}" /bin/sh "${_sigchld_driver}"
    RESULT_VARIABLE _sigchld_result
    OUTPUT_VARIABLE _sigchld_stdout
    ERROR_VARIABLE _sigchld_stderr
    TIMEOUT 15
)

if(NOT "${_sigchld_result}" STREQUAL "42")
    message(
        FATAL_ERROR
        "expected inherited ignored SIGCHLD launch to report rank failure 42, got ${_sigchld_result}\nstdout:\n${_sigchld_stdout}\nstderr:\n${_sigchld_stderr}"
    )
endif()
