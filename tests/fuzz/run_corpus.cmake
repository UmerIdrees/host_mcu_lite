# run_corpus.cmake -- generate the seed corpus and replay it through one harness.
#
# Invoked by ctest once per harness:
#   cmake -DHARNESS=... -DREPLAY=... -DGEN=... -DWORKDIR=... -P run_corpus.cmake
#
# Two steps in one CTest entry, because ctest has no ordering primitive that is
# both portable and cheap here: the corpus has to exist on disk before the replay
# can enumerate it, and the harness has to be built before either. Doing it in a
# script keeps the ordering inside a single test instead of two tests that can be
# run out of order, or in parallel with each other.
#
# The work directory is per-harness, so the socket-backed harnesses do not
# collide on ports, and none of them replays a corpus another one is still writing.

foreach(_v HARNESS REPLAY GEN WORKDIR)
  if(NOT DEFINED ${_v})
    message(FATAL_ERROR "run_corpus.cmake: -D${_v} is required")
  endif()
endforeach()

# Start from empty: a stale case from an earlier build would be replayed as if it
# were part of the current corpus, which is exactly the kind of drift this exists
# to catch.
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}/corpus")

get_filename_component(_hname "${HARNESS}" NAME)

execute_process(
  COMMAND "${GEN}" "${WORKDIR}/corpus"
  RESULT_VARIABLE _gen_rc
  OUTPUT_VARIABLE _gen_out
  ERROR_VARIABLE _gen_err)
if(NOT _gen_rc EQUAL 0)
  message(FATAL_ERROR "gen_corpus failed (${_gen_rc}):\n${_gen_out}${_gen_err}")
endif()

# fuzz_replay, not the harness directly: it runs one process per input, so a
# crash cannot take the enumeration down with it, and it names the first failing
# corpus file on the way out.
execute_process(
  COMMAND "${REPLAY}" "${HARNESS}" --corpus "${WORKDIR}/corpus"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)

# Always show the transcript: on failure the FINDING line names the corpus file
# and its input, and without the corpus the line is not actionable.
message(STATUS "${_out}")
if(_err)
  message(STATUS "${_err}")
endif()

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "${_hname}: seed corpus replay failed (exit ${_rc})")
endif()
message(STATUS "${_hname}: seed corpus replay clean")
