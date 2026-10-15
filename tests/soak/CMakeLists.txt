# Track M soak tests. Bounded by --seconds so the same binary serves both the
# short CTest entry and the long run the plan asks for ("1h continuous" in M1,
# "24-72h" in M7):
#
#   test_soak_reasm --seconds 3600     the 1 h M1 soak
#   ctest -R soak                      the ~5 s version CI runs
#
# hml_soak_util.h holds the shared parts: the wall-clock budget, the resident-set
# watch, the check macros and the loopback port picker. The two rules it exists to
# enforce are worth stating once here:
#
#   - No threads, no sleeps, no libc rand(). The workload is fixed, so the same
#     duration does the same work every run and a change in the numbers means
#     something changed in the library rather than in the schedule.
#   - Invariants are checked at samples throughout the run, not only at the end.
#     A leak that would only be visible after an hour has to show up as a growth
#     rate while the run is still going, or a long soak tells you nothing until it
#     is over.
#
# The tests bind loopback UDP ports from private per-test ranges, so they are
# RUN_SERIAL like the socket-backed fuzz harnesses.

add_executable(test_soak_reasm test_soak_reasm.c)
target_include_directories(test_soak_reasm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_soak_reasm PRIVATE hml_core hml_protocol hml_port_posix)

add_test(NAME test_soak_reasm COMMAND test_soak_reasm --seconds 5)
set_tests_properties(test_soak_reasm PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

# A longer entry, because the plan asks for a 1 h soak and a 5 s entry cannot
# tell a slow leak from noise. Same binary, different --seconds.
add_test(NAME test_soak_reasm_long COMMAND test_soak_reasm --seconds 30)
set_tests_properties(test_soak_reasm_long PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

# The control plane soak needs the private control server and client headers, and
# runs the server on its own thread because that is how hml_control is documented
# to be driven ("not internally locked, one poller per hml_control_t").
find_package(Threads REQUIRED)

add_executable(test_soak_ctrl test_soak_ctrl.c)
target_include_directories(test_soak_ctrl PRIVATE
                           ${CMAKE_SOURCE_DIR}/src/control
                           ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_soak_ctrl PRIVATE
                      hml_control hml_core hml_protocol hml_port_posix Threads::Threads)

add_test(NAME test_soak_ctrl COMMAND test_soak_ctrl --seconds 5)
set_tests_properties(test_soak_ctrl PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

# The RoCE soak drives the real transmitter against the real receiver over
# loopback, with the M3 fault hook dropping and corrupting datagrams, so it needs
# the private rocev2 wire header and the demultiplexer.
add_executable(test_soak_roce test_soak_roce.c)
target_include_directories(test_soak_roce PRIVATE
                           ${CMAKE_SOURCE_DIR}/host/roce_demux
                           ${CMAKE_SOURCE_DIR}/src/transports/rocev2
                           ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_soak_roce PRIVATE
                      hml_roce_demux hml_rocev2 hml_core hml_protocol hml_port_posix
                      Threads::Threads)

add_test(NAME test_soak_roce COMMAND test_soak_roce --seconds 5)
set_tests_properties(test_soak_roce PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
