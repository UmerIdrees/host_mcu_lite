# Host MCU Lite (`hml`) — Fuzzing and Corpus

Fuzzing for the protocol parsers, the CPU-side reassembly path, the control
dispatcher and the RoCEv2 receive demultiplexer. Everything here runs on the
host with **no RoCE NIC, no GPU, no RDMA, no root and no network**: the
transports are socket-based and the harness inputs are ordinary files.

| Guide | Scope |
| --- | --- |
| This file | Harnesses, corpus, replay, mutation, coverage, CI |
| [soak.md](soak.md) | Bounded long-running soak tests (leaks, accounting) |
| [testing.md](testing.md) | The unit and integration suite |
| [protocol_v1.md](protocol_v1.md) | Byte-level wire layout and the rejected-on-parse tables |

---

## 1. What is fuzzed, and why these six

Six harnesses, one per trust boundary. Each one drives the same code the product
drives, with a bounded input that cannot allocate more than a fixed arena and
cannot block on I/O.

| Harness | Target | Input shape |
| --- | --- | --- |
| `fuzz_frame` | `hml_frame_parse` + `hml_frame_seal` | One 28-byte header, optional payload, trailing CRC |
| `fuzz_reasm` | `hml_reasm_*` CPU fragment path | A concatenation of frames, fed fragment by fragment |
| `fuzz_cfg` | `hml_proto_cfg_parse` + the port registry | One 40-byte configuration record |
| `fuzz_ctrl` | `hml_ctrl_fill` / `hml_ctrl_parse` / `hml_beacon_*` | One control or discovery datagram |
| `fuzz_ctrl_device` | `hml_control_poll` dispatch | One datagram into the real dispatcher with real callbacks |
| `fuzz_roce_rx` | `hml_roce_demux` receive path | One `IP｜UDP｜BTH｜RETH｜payload｜ICRC` datagram |

The split matters. `fuzz_ctrl` and `fuzz_ctrl_device` look redundant and are
not: the first proves the codec cannot be made to lie, the second proves a
*server* cannot be made to change state on input it should have rejected. Only
the second one can catch a parser that is correct in isolation but reachable
from a socket.

---

## 2. Build

```sh
cmake -S . -B /tmp/trkm -G Ninja -DCMAKE_BUILD_TYPE=Release -DHML_WARNINGS_AS_ERRORS=ON
cmake --build /tmp/trkm -j8
```

Two build flavours, selected by one option:

| Option | Harness `main()` | Fits |
| --- | --- | --- |
| `HML_FUZZ_USE_LIBFUZZER=OFF` (default) | `hml_fuzz.c` provides it | GCC, CI, coverage-free regression |
| `HML_FUZZ_USE_LIBFUZZER=ON` | omitted, libFuzzer's | clang `-fsanitize=fuzzer`, coverage-guided search |

The default flavour is the one CI uses, because it needs no special compiler
and its verdicts are reproducible. `-DHML_FUZZ_USE_LIBFUZZER=ON` with a
non-clang compiler is a hard configure error rather than a link error, so the
mistake is caught at configure time.

Both flavours get the same runtime (`hml_fuzz.c`): input loading, the
deterministic mutator, the signal and allocation guards, and the seal helpers.
libFuzzer drives the loop; the default flavour drives it from `--selfmutate`.

---

## 3. Seed corpus

`gen_corpus` writes a deterministic corpus and a manifest. Deterministic means
byte-identical across runs, so a corpus diff always means a case changed rather
than a seed shifted.

```sh
/tmp/trkm/tests/fuzz/gen_corpus /tmp/corp
```

```text
gen_corpus: 281 seed inputs in /tmp/corp
```

Layout: one directory per harness plus `manifest.txt`, whose rows are
`harness<TAB>file<TAB>bytes<TAB>tag[,tag...]`. The tags are the point — they name
*which* property each case is for, so a missing case is visible as a missing tag
rather than as a coverage number that happened not to move.

```text
fuzz_frame	valid_single.bin	46	valid
fuzz_frame	payload_len_over_max.bin	94	len_over_max v_0x2000 max_len
fuzz_frame	bad_hdr_crc.bin	46	bad_icrc
fuzz_frame	truncated_one.bin	93	truncated
```

Current corpus: **281 inputs, 71227 bytes**, 360 contract checks. Case names
encode the property (`bad_hdr_crc`, `len_over_max`, `no_nul`, `bad_cfg_kind`,
`rkey_zero_disables_check`), and boundary widths are swept rather than sampled
(`field_12_ffff`, `field_12_7fffffff`, `field_12_ffffff00`,
`field_12_ffffffff`, `field_12_sizemax`).

Two things the generator is careful about, both of which were bugs first:

- **Corrupting a covered field means recomputing its checksum.** A mutation of the
  opcode, the RETH length, the VA or the rkey invalidates the ICRC, so a receiver
  must reject it as a link error rather than as a protocol error. The generator
  recomputes the ICRC, so the case tests the *protocol* check. Cases that
  deliberately break the checksum are tagged `bad_icrc` instead.
- **`rkey == 0` is valid.** It disables the key check. There is a dedicated
  `rkey_zero_disables_check` case; the rejection case is `bad_rkey`, so the two
  cannot be confused by a later edit.

---

## 4. The corpus contract

`test_fuzz_corpus` is not a smoke test. It regenerates the corpus, then for every
manifest row asserts the *specific* outcome that row's name claims, and it fails
if a required tag has no case. That is what stops the corpus from rotting: a
harness that starts accepting `bad_magic` fails here even though nothing crashed.

```sh
/tmp/trkm/tests/fuzz/test_fuzz_corpus /tmp/trkm/tests/fuzz/gen_corpus /tmp/trkm/tests/fuzz
```

```text
test_fuzz_corpus: rows=281 bytes=71227 harnessed=281 checks=360 failed=0
```

`harnessed=281` means every row was additionally executed by its real harness
binary, so the contract and the runtime cannot disagree about what a case does.
`failed=0` covers the 360 property assertions.

The exit status is the whole point: non-zero on any mismatch, so it is usable
unchanged as a CI gate.

---

## 5. Replay

`fuzz_replay` runs a corpus through a harness **one process per input**. The
process boundary is deliberate: a crashing input must not take the driver down
with it, so one bad case still gets reported and the rest of the corpus still
runs.

```sh
# a whole directory
/tmp/trkm/tests/fuzz/fuzz_replay /tmp/trkm/tests/fuzz/fuzz_frame --corpus /tmp/corp/fuzz_frame

# specific files
/tmp/trkm/tests/fuzz/fuzz_replay /tmp/trkm/tests/fuzz/fuzz_ctrl /tmp/corp/fuzz_ctrl/bad_crc.bin
```

```text
fuzz_replay: harness=/tmp/trkm/tests/fuzz/fuzz_frame inputs=35 clean=35 findings=0 errors=0
```

A finding is reported with the path of the first failing input, which is the
artifact to attach to a bug report:

```text
fuzz_replay: FIRST FAILING INPUT /tmp/corp/fuzz_frame/bad_hdr_crc.bin (exit 3)
fuzz_replay: harness=fuzz_frame inputs=35 clean=0 findings=1 errors=0
```

The exit status distinguishes the kinds of failure, so a report can say what
kind without re-running anything:

| Exit | Meaning |
| --- | --- |
| 0 | The input was processed and nothing was found |
| 2 | Usage error (bad arguments, or a harness that does not exist) |
| 3 | A finding: a signal, the allocation guard, the input watchdog, or an assertion |
| 4 | The input could not be read, or the harness itself failed |

A bare 3 is a harness-reported finding; anything else non-zero is a crash in
whatever sits below it, and the exit code is the signal number (e.g. 11 for
`SIGSEGV`, 139 if the driver reports `128 + signal`).

Pass the binary's path, not its bare name: the driver `exec`s it, so a name that
is not on `PATH` is an immediate and confusing `exited 127`. With the full path
it works from any working directory.

---

## 6. Mutation

Two entry points to the same deterministic mutator, so a CI run and a local
reproduction do the same work.

```sh
# via the driver, across the whole corpus as a starting population
/tmp/trkm/tests/fuzz/fuzz_replay /tmp/trkm/tests/fuzz/fuzz_frame --mutate 2000 --seed 7

# or from a harness directly
/tmp/trkm/tests/fuzz/fuzz_frame --selfmutate --iters 2000 --seed 7
```

```text
harness fuzz_frame: selfmutate iters=2000 seed=0x00000007 elapsed=0.46s findings=0
```

`--seed` makes a run reproducible; the seed is printed with the result, so a
number in a CI log can be replayed exactly. `--seconds` bounds by wall clock
instead, for an open-ended local run, and is checked every 64 inputs rather than
once per batch — the socket-backed harnesses spend milliseconds per input, so a
coarse check would overshoot the budget badly.

The mutator is xorshift32 seeded from `--seed` (`hml_fuzz_rng`), and the input
guards are:

- a 64 KiB cap on input length (`HML_FUZZ_MAX_INPUT`), so a huge file cannot make
  a harness walk more than it is meant to;
- a per-input `SIGALRM` watchdog (20 s, or `HML_FUZZ_INPUT_TIMEOUT` when set), so
  a hang is a bounded failure rather than a stuck CI job;
- a `malloc`/`calloc`/`realloc` interposer that reports any single allocation
  above 32 MiB as a finding, so a length taken from a parsed field is caught at
  the allocation it drives rather than three inputs later when the heap is already
  corrupted. Every caller in the library is expected to clamp a caller-supplied
  `size_t` before allocating, so nothing legitimate comes near that;
- a 256 MiB `RLIMIT_AS` ceiling, which nothing legitimate reaches and which stops
  a runaway allocation before the OOM killer does. It is skipped under a
  sanitizer, whose shadow mapping would not fit under it.

---

## 7. CTest

```sh
ctest --test-dir /tmp/trkm -R 'fuzz|corpus' --output-on-failure
```

| Test | What it does |
| --- | --- |
| `test_fuzz_corpus` | Regenerates the corpus and checks all 360 property assertions |
| `fuzz_<harness>_selfmutate` | Runs the deterministic mutator in-process |
| `fuzz_<harness>_corpus` | `gen_corpus` into a scratch dir, then `fuzz_replay` it |

All 13 passed on the tree as of **2026-09-28**:

```text
100% tests passed, 0 tests failed out of 13
Total Test time (real) =  96.43 sec
```

The `_corpus` entries go through `run_corpus.cmake`, which is where the generate
and replay halves are chained. Doing it in CMake rather than in a shell script
keeps the paths generator-relative, so the test works from any build directory.

Each entry is `RUN_SERIAL` and time-bounded (180 s for self-mutation, 600 s for
the corpus replays). `fuzz_reasm_selfmutate` is the slowest of the self-mutation
entries at 17 s, and `fuzz_ctrl_device_selfmutate` at 45 s, because both feed
every mutated input through a full dispatcher or reassembly round rather than a
single parse.

---

## 8. Coverage-guided runs

The default flavour cannot tell you whether the mutator is finding anything new.
For that, build with clang and libFuzzer:

```sh
cmake -S . -B /tmp/trkfz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DHML_FUZZ_USE_LIBFUZZER=ON -DCMAKE_C_COMPILER=clang
cmake --build /tmp/trkfz -j8

/tmp/trkfz/tests/fuzz/fuzz_frame /tmp/corp/fuzz_frame -max_total_time=60 -max_len=2048
```

`-max_len` matters: the reassembly harness concatenates frames, so its interesting
inputs are larger than one datagram, and a small cap would never reach the
multi-fragment paths. The harness argument is a corpus *directory* for the
libFuzzer flavour, not a file.

These runs are deliberately **not** in CTest. Coverage-guided search is
nondeterministic and unbounded, so it belongs in a scheduled job rather than in a
gate whose result has to be the same twice.

---

## 9. CI

Three tiers, cheapest first, and a red build stops at the first one:

```sh
# 1. build and unit tests
cmake -S . -B build -DHML_WARNINGS_AS_ERRORS=ON && cmake --build build -j
ctest --test-dir build --output-on-failure

# 2. corpus contract -- the anti-rot gate
ctest --test-dir build -R test_fuzz_corpus --output-on-failure

# 3. mutation, fixed seeds so the numbers are comparable between runs
ctest --test-dir build -R selfmutate --output-on-failure
```

The corpus is generated into the build tree and never committed, so there is no
corpus to fall out of date in review. A corpus *change* therefore shows up as a
`test_fuzz_corpus` diff — the new case, its tag, and the assertion — which is
the reviewable artifact. Committing generated binaries would make every seed
edit a binary diff and hide exactly the part worth reading.

For a scheduled coverage-guided job, run the §8 commands under clang and archive
libFuzzer's `crash-*` and `leak-*` directories as build artifacts. A crash there
replays through `fuzz_replay` (tier-independent, since the harness binaries are
the same).

---

## 10. Adding a case

1. Add the case to `gen_corpus.c` with a name that states the property and a tag
   from the existing vocabulary.
2. Add the matching assertion to `test_fuzz_corpus.c`. A case with no assertion
   only proves the harness does not crash, which the mutation run already covers.
3. Run `test_fuzz_corpus`. It fails if the tag is required and unrepresented, so
   this is also how a new tag is registered.
4. Run the harness over the regenerated corpus and then `--selfmutate` for a
   while, to check the new case did not break the runtime's own invariants.

A case that only crashes is worth less than a case that asserts a specific
rejection. The contract test is the one that keeps a parser honest over years;
the crash tests only speak when they are spoken to.
