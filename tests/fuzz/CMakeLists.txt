# Track M reliability: fuzz harnesses, deterministic replay driver, seed corpus
# generator and the corpus contract test.
#
# Every harness is a standalone main() that takes one input path, which is what
# makes the two build flavours below possible from the same source:
#
#   default        hml_fuzz.c provides main(); the binary runs a corpus file or
#                  --selfmutate. No instrumentation needed, so this is what CI
#                  uses and it works with gcc.
#   libFuzzer      -DHML_FUZZ_USE_LIBFUZZER=ON drops hml_fuzz.c's main() and lets
#                  -fsanitize=fuzzer's driver call LLVMFuzzerTestOneInput, so
#                  coverage feedback and the crash/minimize/cov corpus machinery
#                  become available. clang only; the option is rejected elsewhere.
#
# The harnesses deliberately link the real library targets, not a copy of the
# sources: a finding therefore names a file and line the maintainers already own.
#
# Robustness lives in hml_fuzz.c and is not optional: RLIMIT_AS, an allocation
# guard, a per-input SIGALRM watchdog and a fatal-signal handler that turns a
# crash into a named FINDING with the offending input, so a regression is
# reproducible from the CTest log alone.

option(HML_FUZZ_USE_LIBFUZZER
       "Build the fuzz harnesses against libFuzzer (-fsanitize=fuzzer, clang only)" OFF)

set(HML_FUZZ_HARNESSES
    fuzz_frame          # hml_proto_frame_parse: the T0 datagram parser
    fuzz_reasm          # hml_reasm_feed_datagram: the fragment state machine
    fuzz_cfg            # hml_proto_cfg_parse + the transport registry
    fuzz_ctrl           # hml_ctrl_parse + hml_beacon_parse
    fuzz_ctrl_device    # the control server's request dispatcher, over a socket
    fuzz_roce_rx)       # the RoCEv2 receive parser, over a socket

# The shared plumbing. hml_fuzz.c is the whole harness runtime: input loading,
# deterministic mutation, the signal/RLIMIT/alloc guards, the finding reporter
# and the seal_* re-serialisers.
add_library(hml_fuzz_runtime STATIC hml_fuzz.c hml_fuzz.h)
target_include_directories(hml_fuzz_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# hml_control is here rather than per-harness because hml_fuzz.c carries the
# seal_ctrl/seal_beacon re-serialisers, and a static library's object files are only
# pulled in when something references them -- so every harness that links the
# runtime needs the codec too, whether it fuzzes the control plane or not.
target_link_libraries(hml_fuzz_runtime PUBLIC hml_core hml_protocol hml_control hml_port_posix)

# hml_fuzz.c redefines malloc/calloc/realloc/free to add the allocation guard.
# That is an interposition, not a call, so the real allocator has to be reached
# directly; the definition is already inside a __GLIBC__ block, but the link has
# to be told the symbols are ours.
if(HML_FUZZ_USE_LIBFUZZER)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
      "HML_FUZZ_USE_LIBFUZZER=ON needs clang (-fsanitize=fuzzer); "
      "configure with the default replay driver instead.")
  endif()
  target_compile_definitions(hml_fuzz_runtime PUBLIC HML_FUZZ_LIBFUZZER=1)
  target_compile_options(hml_fuzz_runtime PUBLIC -fsanitize=fuzzer)
  target_link_options(hml_fuzz_runtime PUBLIC -fsanitize=fuzzer)
endif()

foreach(_h ${HML_FUZZ_HARNESSES})
  add_executable(${_h} ${_h}.c)
  target_link_libraries(${_h} PRIVATE hml_fuzz_runtime)
  # The private headers the two socket-backed harnesses need: the control
  # server's dispatcher header and the host-side RoCEv2 receiver header. Both are
  # PUBLIC on their own targets, so the include path follows the link.
  if(_h STREQUAL "fuzz_ctrl_device")
    target_link_libraries(${_h} PRIVATE hml_control)
  elseif(_h STREQUAL "fuzz_roce_rx")
    target_link_libraries(${_h} PRIVATE hml_roce_demux)
  endif()
endforeach()

# ---- deterministic replay driver -------------------------------------------
# Replays a corpus through a harness one process per input. The process boundary
# is the point: a crash cannot take the driver down, so the failing corpus file
# is always reported.
add_executable(fuzz_replay fuzz_replay.c)
target_link_libraries(fuzz_replay PRIVATE hml_fuzz_runtime)

# ---- seed corpus generator -------------------------------------------------
# Deterministic: no clock, no rand, no environment. Two runs produce identical
# bytes, so a corpus diff always means a case changed rather than a seed shifted.
add_executable(gen_corpus gen_corpus.c)
target_link_libraries(gen_corpus PRIVATE hml_fuzz_runtime)

# ---- corpus contract test --------------------------------------------------
# Regenerates the corpus into a scratch directory, checks every manifest row's
# bytes against its tags, requires every required case class to be present, and
# runs every seed through its own harness. One CTest entry, because that is the
# unit that fails if the corpus rots.
add_executable(test_fuzz_corpus test_fuzz_corpus.c)
target_link_libraries(test_fuzz_corpus PRIVATE hml_fuzz_runtime)
foreach(_h ${HML_FUZZ_HARNESSES})
  add_dependencies(test_fuzz_corpus ${_h})
  add_dependencies(gen_corpus ${_h})
  add_dependencies(fuzz_replay ${_h})
endforeach()

add_test(NAME test_fuzz_corpus
         COMMAND test_fuzz_corpus $<TARGET_FILE:gen_corpus> ${CMAKE_CURRENT_BINARY_DIR})
# The two socket-backed harnesses bind ports from a private range, so this test
# cannot share them with a concurrently running test.
set_tests_properties(test_fuzz_corpus PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)

# ---- short deterministic mutation run, per harness -------------------------
# A fixed seed and a fixed iteration count, so the same inputs are generated in the
# same order on every machine. The wall-clock backstop exists only so a slow box
# ends the run early instead of hitting the ctest timeout. This is the regression
# net for the harnesses themselves, not a substitute for a libFuzzer campaign; see
# docs/fuzzing.md for how to run one.
foreach(_h ${HML_FUZZ_HARNESSES})
  add_test(NAME ${_h}_selfmutate
           COMMAND fuzz_replay $<TARGET_FILE:${_h}>
                   --mutate 20000 --seed 0x484d4c31 --seconds 45)
  set_tests_properties(${_h}_selfmutate PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
endforeach()

# ---- corpus replay, per harness --------------------------------------------
# The real regression run: every generated seed, through its own harness.
foreach(_h ${HML_FUZZ_HARNESSES})
  add_test(NAME ${_h}_corpus
           COMMAND ${CMAKE_COMMAND}
                   -DHARNESS=$<TARGET_FILE:${_h}>
                   -DREPLAY=$<TARGET_FILE:fuzz_replay>
                   -DGEN=$<TARGET_FILE:gen_corpus>
                   -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/corpus_${_h}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/run_corpus.cmake)
  set_tests_properties(${_h}_corpus PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
endforeach()
