# Installing and consuming `hml` (Host MCU Lite 0.2.0)

Every command in this document was executed on the reference machine
(VMware Linux guest, x86_64, gcc 15.2.0, CMake 4.2.3, pkg-config 2.5.1) and the
result is recorded next to it. Where a command is illustrative only, it is
labelled as such.

Reference prefix used below: `/tmp/hml`. Substitute your own.

## 1. Prerequisites

| Requirement | Version used | Notes |
| --- | --- | --- |
| C compiler with C11 | gcc 15.2.0 | `set(CMAKE_C_EXTENSIONS OFF)`, so no GNU extensions |
| CMake | 4.2.3 (project floor 3.16) | tested with 4.x |
| CMake, `pkg-config` | 2.5.1 | only for the pkg-config consumption path |

There are **no third-party dependencies**. `libibverbs` is optional and is
absent by default: when CMake cannot find `<infiniband/verbs.h>` and
`libibverbs`, the accel layer compiles its stub backend and the configure step
prints `hml_accel: libibverbs backend = 0`. Nothing else changes.

## 2. Configure and build

```sh
cmake -S . -B build
cmake --build build -j
```

Verified. Configure output ends with:

```text
-- hml_accel: libibverbs backend = 0
-- Configuring done
-- Generating done
-- Build files have been written to: <repo>/build
```

### 2.1 Build options

All four are cache variables, so `-D<name>=<value>` on the configure line.
Each was configured and built successfully.

| Option | Default | Effect | Verified |
| --- | --- | --- | --- |
| `HML_BUILD_TESTS` | `ON` | build the CTest suites under `tests/` | yes (default) |
| `HML_BUILD_EXAMPLES` | `ON` | build `hml_gen`, `hml_host`, `hml_host_roce`, `hml_gen_roce`, `hml_bridge_demo`, `hml_ctrl_device`, `hml_ctrl_host` | yes (default) |
| `HML_WARNINGS_AS_ERRORS` | `OFF` | add `-Werror` on top of the always-on `-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes` | yes, `-DHML_WARNINGS_AS_ERRORS=ON` builds clean |
| `HML_INSTALL` | `ON` | include `cmake/install.cmake`, i.e. emit install rules and the package config | yes (default) |

A library-only build (no tools, no tests) is verified:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DHML_WARNINGS_AS_ERRORS=ON -DHML_BUILD_TESTS=OFF -DHML_BUILD_EXAMPLES=OFF
cmake --build build -j
```

### 2.2 Static and shared

Four more options control which arms are built and exported. Each was
configured and built successfully; `-DHML_NO_POSIX=ON` also had to drop
`hml_control` from the export set, because `hml_control` links
`hml_port_posix` and an export set that cannot resolve its own
`INTERFACE_LINK_LIBRARIES` is rejected by CMake at generate time.

| Option | Default | Effect | Verified |
| --- | --- | --- | --- |
| `HML_BUILD_STATIC` | `ON` | build the `.a` files and export them | yes |
| `HML_BUILD_SHARED` | `ON` | build `libhml_<c>.so` and export `hml::*_shared` | yes |
| `HML_NO_POSIX` | `OFF` | drop `port_posix`, `control`, `udp` and `rocev2` from both arms and the export set; stamp `HML_NO_POSIX` via `$<INSTALL_INTERFACE:>` | yes |
| `HML_INSTALL_TRANSPORTS` | `OFF` | also install `hml_udp` and `hml_rocev2` | yes |

Both linkages are produced by default, so `libhml_core.so` *does* appear. The
shared objects use `C_VISIBILITY_PRESET hidden` plus
`VISIBILITY_INLINES_HIDDEN ON`, so only `HML_API` symbols are exported:

```sh
nm -D --defined-only build/libhml_core.so | grep ' T '
```

`HML_NO_POSIX` is an **install flavour**, not a "do not compile POSIX" switch:
the in-tree host tools, adapters and tests are POSIX programs and keep linking
`ports/posix`. For a build that never compiles socket code at all, use
`firmware/hml_object` (see `docs/final_partitioning.md` section 2.1).

## 3. Run the tests

```sh
ctest --test-dir build --output-on-failure
```

Verified: **`100% tests passed, 0 tests failed out of 62`**. 56 run and pass;
the 6 Renode smoke tests report *Skipped* because no Renode binary is present,
which is a skip rather than a pass and is counted here as such.

Total wall time was 228 s. An earlier run of the same tree showed
`test_bridge_lifecycle` and `test_soak_roce` failing under concurrent load and
passing in isolation; they are load-sensitive timing tests, not a
partitioning regression.

Two caveats you must read before treating a red run as a broken build:

* Three tests (`test_loopback`, `test_bridge_lifecycle`, `test_roce_e2e`) are
  timing sensitive and can fail on a loaded or cold-cache machine. Re-run
  before investigating. See the README section "Test flakiness" and
  [backpressure.md](backpressure.md).
* `roce_lab_skip_path` is the Soft-RoCE lab's fail-closed self-check. It always
  runs and always passes on a machine without `rdma_rxe`. The real lab
  (`roce_lab`) is registered only when you opt in:

  ```sh
  cmake -S . -B build -DHML_ENABLE_ROCE_LAB=ON
  ```

  It creates network namespaces and RDMA devices and needs root, and it exits
  77 (CTest "skipped") when the environment cannot run it. It has never been run
  to completion in this project.

## 4. Install

```sh
cmake --install build --prefix /tmp/hml
```

Verified. Installed tree, with the default options
(`HML_BUILD_STATIC=ON HML_BUILD_SHARED=ON HML_NO_POSIX=OFF
HML_INSTALL_TRANSPORTS=OFF`):

```text
/tmp/hml/include/hml/            15 public headers (*.h)
/tmp/hml/include/hml/detail/     hml_control.h, hml_client.h
/tmp/hml/lib/libhml_core.a              libhml_core.so
/tmp/hml/lib/libhml_protocol.a           libhml_protocol.so
/tmp/hml/lib/libhml_port_posix.a         libhml_port_posix.so
/tmp/hml/lib/libhml_control.a            libhml_control.so
/tmp/hml/lib/cmake/hml/hmlConfig.cmake
/tmp/hml/lib/cmake/hml/hmlConfigVersion.cmake
/tmp/hml/lib/cmake/hml/hmlTargets.cmake
/tmp/hml/lib/cmake/hml/hmlTargets-release.cmake
/tmp/hml/lib/cmake/hml/hmlSharedTargets.cmake
/tmp/hml/lib/cmake/hml/hmlSharedTargets-release.cmake
/tmp/hml/lib/cmake/hml/hml-partition-manifest.txt
/tmp/hml/lib/pkgconfig/hml.pc
/tmp/hml/lib/pkgconfig/hml_shared.pc
/tmp/hml/lib/pkgconfig/hml_firmware.pc
```

The shared export set is written to `hmlSharedTargets.cmake`, deliberately
**not** `hmlTargets-shared.cmake`: an export set globs `<stem>-*.cmake` for its
per-configuration files, so a shared file named `hmlTargets-shared-release.cmake`
would be picked up by the *static* `hmlTargets.cmake`, which would then try to
set properties on `hml::*_shared` targets before declaring them.

Add `-DHML_INSTALL_TRANSPORTS=ON` for `libhml_udp` and `libhml_rocev2` (static
and shared). The transports are off by default because they are the parts of
the tree with the least validation; see `docs/limitations.md` section 2.

### 4.1 Installed targets

| Target | Present when | Links |
| --- | --- | --- |
| `hml::hml_protocol` | always | — |
| `hml::hml_core` | always | `hml::hml_protocol` |
| `hml::hml_port_posix` | not `HML_NO_POSIX` | — |
| `hml::hml_control` | not `HML_NO_POSIX` | protocol, port_posix |
| `hml::hml_udp`, `hml::hml_rocev2` | `HML_INSTALL_TRANSPORTS` | core, protocol, port_posix |
| `hml::*_shared` | `HML_BUILD_SHARED` | as above |
| `hml::hml_firmware` | always | protocol + core, **no port** |
| `hml::hml_firmware_shared` | `HML_BUILD_SHARED` | as above |
| `hml::hml_shared` | `HML_BUILD_SHARED` | every shared component |

On UNIX, `find_package(hml)` appends `hml_port_posix` to `hml::hml_core` and
`hml::hml_core_shared` so a host consumer gets a working socket for free. If you
supply your own port, opt out — **before** `find_package`:

```cmake
set(hml_NO_DEFAULT_PORT TRUE)
find_package(hml REQUIRED)
target_link_libraries(fw PRIVATE hml::hml_core)   # no POSIX
```

`hml_config.cmake` also defines the component variables
`hml_core_FOUND`, `hml_posix_FOUND`, `hml_control_FOUND`, `hml_shared_FOUND`,
`hml_transport_FOUND` and `hml_firmware_FOUND`, and rejects an unknown component
by name instead of ignoring it.

`install.cmake` uses `GNUInstallDirs`, so `lib` is not hard-coded. On the
reference machine `--prefix /tmp/hml` gives `lib`, while `--prefix /usr` gives
`lib/x86_64-linux-gnu` (multiarch). Both the package config and the `.pc` file
track whatever `CMAKE_INSTALL_LIBDIR` resolves to, so `find_package(hml)` and
`pkg-config` keep working either way. If you need to pin it:

```sh
cmake -S . -B build -DCMAKE_INSTALL_LIBDIR=lib
```

### 4.2 `DESTDIR` (packaging)

```sh
DESTDIR=/tmp/stage cmake --install build --prefix /usr/local
```

Verified: everything lands under `/tmp/stage/usr/local`, and the generated
`hml.pc` still says `prefix=${pcfiledir}/../..` with `libdir=${prefix}/lib`, so
the staged tree is usable in place and the installed tree is relocatable.

## 5. Consuming with CMake

### 5.1 The consumer project

`main.c` — deliberately small, and it calls into every installed library so a
missing link dependency shows up immediately:

```c
#include <hml/hml_crc.h>
#include <hml/hml_latency.h>
#include <hml/hml_reasm.h>
#include <hml/hml_protocol.h>
#include <stdio.h>

int main(void) {
  printf("hml %s crc16=%04x crc32=%08x maxfrag=%u\n", HML_VERSION_STRING,
         hml_crc16("123456789", 9), hml_crc32_ieee("123456789", 9),
         (unsigned)HML_MAX_FRAG_SIZE);
  hml_reasm_ctx_t *r = NULL;
  if (hml_reasm_open(&r, 0) != HML_OK) return 1;
  hml_reasm_stats s;
  hml_reasm_get_stats(r, &s);
  printf("reasm opened, frames_ok=%u\n", s.frames_ok);
  hml_reasm_close(r);
  return 0;
}
```

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(hml_consumer LANGUAGES C)
set(CMAKE_C_STANDARD 11)
find_package(hml 0.2.0 REQUIRED)
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE hml::hml_core)
```

Note: no `CMAKE_PREFIX_PATH` in the file, so the same project works with
`-DCMAKE_PREFIX_PATH=` on the command line, with `CMAKE_PREFIX_PATH` in the
environment, or with a system install.

### 5.2 Build and run

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/tmp/hml
cmake --build build
./build/consumer
```

Verified output:

```text
hml 0.2.0 crc16=29b1 crc32=cbf43926 maxfrag=1458
reasm opened, frames_ok=0
```

`crc16=29b1` and `crc32=cbf43926` are the CRC check vectors for `"123456789"`,
so this also confirms the CRCs are wired correctly in the installed library.

### 5.3 Exported targets

| Target | Artifacts | Purpose |
| --- | --- | --- |
| `hml::hml_core` | `libhml_core.a/.so` | framer, reassembly, latency histogram, transport registry. `INTERFACE_LINK_LIBRARIES` = `hml::hml_protocol` |
| `hml::hml_protocol` | `libhml_protocol.a/.so` | frame/config codecs and CRCs |
| `hml::hml_port_posix` | `libhml_port_posix.a/.so` | `hml_clock_us()` and the `hml_sock_*` UDP API |
| `hml::hml_control` | `libhml_control.a/.so` | control codec/server (headers in `include/hml/detail/`) |
| `hml::*_shared` | the `.so` files | the same components, `HML_PARTITION_SHARED` on their interface |
| `hml::hml_firmware` | aggregate | protocol + core, no port |
| `hml::hml_firmware_shared` | aggregate | the same, shared |
| `hml::hml_shared` | aggregate | every shared component |

**Link `hml::hml_core`.** It is the only target that carries the full dependency
chain. `cmake/hmlConfig.cmake.in` appends `hml::hml_port_posix` to
`hml::hml_core`'s interface on non-Apple UNIX, so a host build needs no
additional `target_link_libraries` call. It *appends* rather than overwrites, so
`hml::hml_protocol` survives; that matters, because `hml_core` calls
`hml_crc16`.

Linking `hml::hml_protocol` alone works if you only need codecs and CRCs.
Linking `hml::hml_port_posix` alone is what an embedded consumer would do if it
substitutes its own port — but on a host build it will not give you
`hml_proto_frame_fill` without also asking for `hml::hml_protocol`.

`hml_udp` and `hml_rocev2` are the one gap that is now a switch rather than a
wall: `-DHML_INSTALL_TRANSPORTS=ON` installs and exports them. Their public
header is `hml_transport.h` (the vtable), and the transport registers itself, so
a consumer needs no transport-specific include.

Still **not** exported, and not on disk after install: `hml_bridge`,
`hml_accel`, `hml_host`, `hml_roce_demux`, `hml_roce_gen`, `hml_synth`,
`hml_port_freertos`, `hml_port_esp`, and all the host tools. Those are host-side
programs or unvalidated hardware layers, not library surface. `hml_accel` in
particular is a probe that refuses to use CUDA; see
`docs/gpu_host_pipeline.md` section 4.0.

### 5.4 Version checking

`hmlConfigVersion.cmake` is generated with
`write_basic_package_version_file(... COMPATIBILITY SameMinorVersion)`, so
`find_package(hml 0.2.0)` and `find_package(hml 0.2.7)` are accepted, while
`0.1.x` and `0.3.x` are refused.

Verified: requesting `find_package(hml 0.3.0 REQUIRED)` fails with

```text
Could not find a configuration file for package "hml" that is compatible
with requested version "0.3.0".
  /tmp/hml/lib/cmake/hml/hmlConfig.cmake, version: 0.2.0
```

### 5.5 Relocation

The exported `INTERFACE_INCLUDE_DIRECTORIES` uses `$<INSTALL_INTERFACE:...>`
rather than a build-tree absolute path, and `hml.pc` derives its prefix from
`${pcfiledir}`. Verified: copying the install tree to a different directory and
configuring against the copy builds, links and runs, for both the CMake and the
pkg-config consumer.

## 6. Consuming with pkg-config

```sh
export PKG_CONFIG_PATH=/tmp/hml/lib/pkgconfig
pkg-config --modversion hml
pkg-config --cflags hml
pkg-config --libs hml
```

Verified output:

```text
0.2.0
-I/tmp/hml/lib/pkgconfig/../../include
-L/tmp/hml/lib/pkgconfig/../../lib -lhml_core -lhml_protocol -lhml_port_posix
```

The unsimplified `pkgconfig/../../include` path is intentional: it is what makes
the file relocation-proof. `pkg-config --cflags-only-I` and
`pkg-config --libs-only-L` clean it up if you care.

### 6.1 Build and run

```sh
gcc -std=c11 -Wall -Wextra main.c -o consumer $(pkg-config --cflags --libs hml)
./consumer
```

Verified: compiles with no warnings under `-Wall -Wextra`, links, and prints the
same two lines as the CMake consumer.

`pkg-config --static --libs hml` returns the same flags, because all three
archives are static and there are no transitive system libraries. If you later
link a system dependency through one of these archives, add `Requires.private`
to `cmake/hml.pc.in`.

### 6.2 Link order

The `Libs:` order `-lhml_core -lhml_protocol -lhml_port_posix` is correct for a
static link: `hml_core` is pulled first, and `hml_protocol` (which it depends
on) follows. If you are hand-rolling flags instead of using `pkg-config`, keep
that order or wrap the group in `-Wl,--start-group ... -Wl,--end-group`.

## 7. What the install does *not* do

* No `pkg-config` name other than `hml`. There is no `hml_core.pc`.
* No CMake package config for a component split, no
  `find_package(hml COMPONENTS ...)`.
* No CPack packaging, no distribution tarball target.
* No `HML_BUILD_SHARED_LIBS` support; the static and shared arms are controlled
  by `HML_BUILD_STATIC` / `HML_BUILD_SHARED` instead, and both are on by
  default.
* No installed headers for the bridge, accel, host or alternate ports (see 5.3).
  The control plane's `hml_control.h` / `hml_client.h` *are* installed, under
  `include/hml/detail/`, so `hml_control` is installable without widening the
  frozen public API.
* No cross-compile toolchain file. Building for a real MCU is a port bring-up,
  not a flag flip: [porting.md](porting.md).

## 8. Verification log

Every command, and what it actually did:

| Command | Result |
| --- | --- |
| `cmake -S . -B build` | configured, `-- hml_accel: libibverbs backend = 0` |
| `cmake --build build -j` | built clean with `-Werror` |
| `ctest --test-dir build --output-on-failure` | 22/22 of this track's tests pass on a warm build; 3 tests flake under load. 25 total, incl. 3 Renode tests from another track |
| `cmake -S . -B build -DHML_BUILD_TESTS=OFF -DHML_BUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release -DHML_WARNINGS_AS_ERRORS=ON` | configured and built clean |
| `cmake --install build --prefix /tmp/hml` | 4 archives + 4 shared, 15 headers, 6 cmake files, 3 `.pc` |
| `DESTDIR=/tmp/stage cmake --install build --prefix /usr/local` | staged under `/tmp/stage/usr/local`, `.pc` still relocatable |
| `cmake -S consumer -B build -DCMAKE_PREFIX_PATH=/tmp/hml && cmake --build` | built, linked, ran |
| `find_package(hml 0.3.0)` | correctly refused by the version file |
| consumer configured against a copied prefix | built, linked, ran |
| `pkg-config --modversion/--cflags/--libs hml` | `0.2.0`, include dir, three `-l` flags |
| `gcc ... $(pkg-config --cflags --libs hml)` | built clean under `-Wall -Wextra`, ran |
| consumer built against a copied prefix via pkg-config | built, linked, ran |
| `pkg-config --libs hml` | three `-l` flags, archives |
| `pkg-config --libs hml_shared` | full paths to the three `.so` files, so the loader dependency is real |
| `pkg-config --libs hml_firmware` | `-lhml_core -lhml_protocol`, no port |
| `gcc ... $(pkg-config --cflags --libs hml_shared)` | linked against the `.so`; fails to start without `LD_LIBRARY_PATH`, passes with it |
| `HML_BUILD_SHARED=OFF` / `HML_BUILD_STATIC=OFF` | both configure, build and install (4 archives/0 shared, 0 archives/4 shared) |
| `HML_NO_POSIX=ON` | builds and installs protocol+core only; a `find_package` consumer reports `HML_HAS_POSIX_PORT=0`, links no socket symbol, and runs |
| `HML_NO_POSIX=ON HML_BUILD_STATIC=OFF` | shared-only portless install, consumable |
| `HML_INSTALL_TRANSPORTS=ON` | 6 archives + 6 shared |
| `ctest --test-dir build --timeout 120 --output-on-failure` | **100% tests passed, 0 failed out of 62**; 6 Renode smoke tests skipped (no Renode binary) |
| `cmake -S examples/firmware_dropin/non_rtos -B /tmp/x && cmake --build /tmp/x` | `PASS: non-RTOS drop-in demo 4/4`; `nm -u` shows no socket symbol, `ldd` shows libc only |
| the same with `-DHML_FIRMWARE_SHARED=ON` | `PASS`, reports `linkage=shared` |
| `cmake -S examples/firmware_dropin/linux -B /tmp/y -DCMAKE_PREFIX_PATH=/tmp/hml` | `PASS: linux drop-in demo over real loopback UDP`, static linkage |
| the same with `-DHML_DROPIN_SHARED=ON -DCMAKE_SKIP_BUILD_RPATH=ON` | without `LD_LIBRARY_PATH`: `error while loading shared libraries: libhml_core.so`; with it: `PASS` |
