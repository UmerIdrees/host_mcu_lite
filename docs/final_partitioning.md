# Partitioning `hml` for firmware, package and host consumers

**Status: build/install contract for this tree. Every claim below is checked by
`examples/firmware_dropin/` and by the commands in `docs/install.md`.**

## 1. What the library is split into

`hml` is one library with six independently buildable components. The split is
not a source-directory convention: each component is its own CMake target, its
own static archive, its own shared object, and its own entry in the installed
export set. A consumer links the ones it needs and nothing else.

| Component | Target | Source | Needs a port? | What it is |
|---|---|---|---|---|
| protocol | `hml_protocol` | `src/protocol/` | no | CRCs, the HML wire header, `hml_dest_cfg` codec |
| core | `hml_core` | `src/core/` | **yes** | framer, reassembly, latency histogram, transport registry |
| port_posix | `hml_port_posix` | `ports/posix/` | is the port | the host socket and clock implementation |
| control | `hml_control` | `src/control/` | **yes** | discovery beacon + control codec/server |
| udp | `hml_udp` | `src/transports/udp/` | **yes** | T0 UDP data plane |
| rocev2 | `hml_rocev2` | `src/transports/rocev2/` | **yes** | T1a socket-crafted RoCEv2, experimental |

"Needs a port" means the component calls `hml_clock_us()` and/or the
`hml_sock_*` family. It does not mean it needs a *specific* port: any
implementation of those seven symbols will do, which is the entire point of
section 3.

The dependency graph is fixed and declared in one place,
`cmake/hml_component.cmake`:

```
protocol          (no platform symbols at all)
  ^
core              (clock only)
  ^
  +-- udp         (clock + sockets)
  +-- rocev2      (clock + sockets)

port_posix        (the one real port: Linux/macOS sockets + CLOCK_MONOTONIC)
  ^
  +-- control     (beacon/server speak the control protocol over UDP)
  +-- udp
  +-- rocev2
```

`core` deliberately does **not** link `port_posix`. That is the property that
makes bare-metal possible: an RTOS build links `hml_core` and its own six
symbols, and the POSIX socket code is never compiled, never archived, never in
the link line.

## 2. The two build systems

There are two ways to consume the library, for two different situations.

### 2.1 `add_subdirectory` — firmware (no install step)

`firmware/hml_object/` builds the components from source inside *your* project.
This is the path for a board that has no POSIX host at all.

```cmake
add_subdirectory(third_party/hml/firmware/hml_object)

add_executable(my_firmware src/main.c src/port_my_board.c)
target_link_libraries(my_firmware PRIVATE hml::hml_core)
```

It provides `hml::hml_<component>` aliases and the aggregate
`hml::hml_firmware` (= protocol + core). There is deliberately **no
`install()`** in this file: firmware is not installed, it is flashed.

Options (cache or `-D`):

| Option | Default | Effect |
|---|---|---|
| `HML_FIRMWARE_COMPONENTS` | `protocol;core;control;udp;rocev2` | which components to build |
| `HML_FIRMWARE_SHARED` | `OFF` | also build the `_shared` arm |
| `HML_FIRMWARE_POSIX` | `OFF` | build `ports/posix` and keep the POSIX tag |

`HML_FIRMWARE_POSIX` defaults to `OFF` and is not "auto": the build machine's
`__linux__` must never leak into a cross build, so the decision is explicit.

### 2.2 The top-level project — install and packages

The top-level `CMakeLists.txt` builds the static targets in place, mirrors them
as shared objects, and installs. It is for host builds and for consumers that
want a prefix.

| Option | Default | Effect |
|---|---|---|
| `HML_BUILD_STATIC` | `ON` | build and install the `.a` files |
| `HML_BUILD_SHARED` | `ON` | build and install the `.so` files |
| `HML_NO_POSIX` | `OFF` | drop `port_posix` from the shared arm and the export set; stamp `HML_NO_POSIX` |
| `HML_INSTALL_TRANSPORTS` | `OFF` | install `hml_udp` / `hml_rocev2` too |

### 2.3 What `HML_NO_POSIX` does and does not do

This is worth stating exactly, because the name suggests more than it does.

`HML_NO_POSIX=ON` makes the *installed package* embedded-flavoured: no
`libhml_port_posix.*` is installed or exported, and `$<INSTALL_INTERFACE:>`
stamps `HML_NO_POSIX` so a consumer's translation units cannot auto-tag as
POSIX. `hml_control` is also dropped from the export set, because it *links*
`hml_port_posix` and an export set that cannot resolve its own
`INTERFACE_LINK_LIBRARIES` is rejected by CMake at generate time.

It does **not** stop `ports/posix` from being compiled. The in-tree host tools,
adapters and tests are POSIX programs and link it; removing it from them would
break the tree for no benefit. If you need a build where POSIX socket code is
never compiled at all, use `firmware/hml_object` (section 2.1), which never adds
that directory.

## 3. The port contract

A port supplies exactly seven functions and nothing else:

```c
uint64_t hml_clock_us(void);                              /* MONOTONIC microseconds */
hml_status hml_sock_udp_create(hml_sock_t **out, uint32_t bind_ip,
                               uint16_t bind_port, bool broadcast);
hml_status hml_sock_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port);
hml_status hml_sock_send(hml_sock_t *s, const void *buf, size_t n);
hml_status hml_sock_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n, int timeout_ms);
void      hml_sock_close(hml_sock_t *s);
```

Notes that are easy to get wrong:

- **`hml_clock_us` must be monotonic.** The control plane's deadlines and the
  latency histogram both depend on it. `hml_reasm` treats a backwards delta as
  a skew sample and discards it, so a wall clock silently turns your p99 into
  "no data". The example stubs use C11 `timespec_get(TIME_UTC)` and say loudly
  that this is wrong for a real target; a real port reads a hardware timer or an
  RTOS tick count.
- **IP arguments are host order.** `hml_sock_udp_create` and
  `hml_sock_set_dest` apply `htonl()` themselves. `hml_dest_cfg.ipv4`, by
  contrast, is network order on the wire — `src/transports/udp/hml_udp_priv.h`
  holds the conversion. Mixing the two up is the single most common port bug.
- **`hml_sock_recv` reports "nothing yet" as a status, not a short read.** A
  bounded wait returns `HML_ERR_TIMEOUT`; a poll or an interrupted read returns
  `HML_ERR_AGAIN`. Neither is an error, and a receiver that treats them as
  failures will drop frames on any loaded machine.

### 3.1 Turning the contract into a link error

`include/hml/hml_partition.h` declares the same seven names in one place, as
the `HML_PORT_FN_LIST` X-macro. `cmake/hml_firmware.cmake` generates
`hml_port_contract.c` from it, and every initialiser in that file is a real
function address:

```c
const hml_port_contract hml_port_contract_check = {
  HML_PORT_FN_LIST(HML_PORT_ENTRY)
};
```

Add that generated file to your port's sources and a missing symbol becomes a
link error that names it, instead of a crash on the first datagram:

```cmake
add_library(my_port STATIC src/port_my_board.c ${hml_firmware_contract_source})
target_link_libraries(my_firmware PRIVATE my_port hml::hml_core)
```

A port stub must **not** define `hml_port_contract_check` itself. Both example
stubs carry a comment saying so, because doing it in both places is a duplicate
symbol at link time.

The generated object is unreferenced data, so an aggressive linker may discard
it. If yours does, force it with `-u hml_port_contract_check` or reference it
from your port's init function.

### 3.2 libc

`HML_LIBC_REQUIREMENTS` in `hml_partition.h` states what the portable arms call:
`memset`, `memcpy`, `malloc`, `free`. On a freestanding target these are your
runtime's hooks (newlib-nano, picolibc, or a static arena). `hml_udp` and
`hml_control` additionally use `calloc`, which is why the transports and the
control plane are not in the bare-metal aggregate target.

## 4. Visibility and the two linkage names

Every public symbol is tagged `HML_API`. On ELF that is
`__attribute__((visibility("default")))`; on MSVC it is `__declspec(dllimport)`
/`dllexport`; with `HML_NO_VISIBILITY` it is empty.

`HML_INLINE` (`static inline`) is used for the header helpers in
`hml_port.h`. It is deliberately *not* `static HML_API inline`: a visibility
attribute on a `static` function is ignored by GCC, and with
`-Werror=attributes` that is a build failure. Some private headers under
`src/bridge/` still write `static HML_API inline`; they are not ours to edit, so
`hml_export.h` suppresses exactly that one diagnostic and nothing else.

A translation unit can ask which partition it is in:

| Macro | Values | Set by |
|---|---|---|
| `HML_LINKAGE_NAME` | `"static"` / `"shared"` | `HML_PARTITION_SHARED` |
| `HML_HAS_POSIX_PORT` | `0` / `1` | `hml_port.h`'s platform tag |
| `HML_LIBC_REQUIREMENTS` | string | nothing, it is documentation |

`HML_HAS_POSIX_PORT` is derived from `HML_PORT_POSIX`, which only
`hml_port.h` defines — so `hml_partition.h` includes `hml_port.h` itself.
Without that include the value would depend on whether some other header
happened to pull it in first, and the same translation unit could report `0` or
`1` purely by include order. That was a real bug; the examples now assert the
value they need with `_Static_assert` rather than trusting it.

## 5. Installed targets

| Target | Present when | Links |
|---|---|---|
| `hml::hml_protocol` | always | — |
| `hml::hml_core` | always | `hml::hml_protocol` |
| `hml::hml_port_posix` | not `HML_NO_POSIX` | — |
| `hml::hml_control` | not `HML_NO_POSIX` | protocol, port_posix |
| `hml::hml_udp`, `hml::hml_rocev2` | `HML_INSTALL_TRANSPORTS` | core, protocol, port_posix |
| `hml::*_shared` | `HML_BUILD_SHARED` | as above |
| `hml::hml_firmware` | always | protocol + core, **no port** |
| `hml::hml_firmware_shared` | `HML_BUILD_SHARED` | as above |
| `hml::hml_shared` | `HML_BUILD_SHARED` | every shared component |

On UNIX, `find_package(hml)` appends `hml_port_posix` to `hml::hml_core` (and
to `hml::hml_core_shared`) by default, so a host consumer that links
`hml::hml_core` gets a working socket for free. That is convenient, and it is
wrong for a portless consumer. If you supply your own port, ask for the honest
one:

```cmake
set(hml_NO_DEFAULT_PORT TRUE)   # before find_package
find_package(hml REQUIRED)
target_link_libraries(fw PRIVATE hml::hml_core)
```

The three `.pc` files follow the same split, and the flags differ on purpose:

| File | Links | Notes |
|---|---|---|
| `hml.pc` | `-lhml_core -lhml_protocol -lhml_port_posix` | unchanged from 0.2.0 |
| `hml_shared.pc` | full paths to the `.so` files | see below |
| `hml_firmware.pc` | `-lhml_core -lhml_protocol` | no port |

`hml_shared.pc` names the shared objects by full path on purpose. This install
ships both `libhml_core.a` and `libhml_core.so`, and the linker prefers the
archive — so a `-lhml_core` spelling would hand a consumer static linkage from
a package named "shared", silently. Spelling the filenames also makes the
loader dependency real, so the binary fails at startup without
`LD_LIBRARY_PATH` instead of quietly working.

## 6. Worked examples

Both are in `examples/firmware_dropin/` and both are run in CI.

| | `non_rtos/` | `linux/` |
|---|---|---|
| consumes | `add_subdirectory` | a staged install via `find_package` |
| port | a stub in the example | `hml_port_posix` from the package |
| socket | never opened (`HML_ERR_NOTSUP`) | real loopback UDP |
| link line | no `hml_port_posix`, no `pthread` | `hml_port_posix` |
| proves | a bare-metal-shaped link is possible | the installed package is self-contained |

```bash
# Bare-metal shape, static and shared
cmake -S examples/firmware_dropin/non_rtos -B /tmp/hml_nr -G Ninja
cmake --build /tmp/hml_nr && /tmp/hml_nr/hml_dropin_non_rtos

cmake -S examples/firmware_dropin/non_rtos -B /tmp/hml_ns -G Ninja \
      -DHML_FIRMWARE_SHARED=ON
cmake --build /tmp/hml_ns && /tmp/hml_ns/hml_dropin_non_rtos

# Installed package, static and shared
cmake --install /tmp/hml_build --prefix /tmp/hml_stage
cmake -S examples/firmware_dropin/linux -B /tmp/hml_ln -G Ninja \
      -DCMAKE_PREFIX_PATH=/tmp/hml_stage
cmake --build /tmp/hml_ln && /tmp/hml_ln/hml_dropin_linux

cmake -S examples/firmware_dropin/linux -B /tmp/hml_ls -G Ninja \
      -DCMAKE_PREFIX_PATH=/tmp/hml_stage -DHML_DROPIN_SHARED=ON \
      -DCMAKE_SKIP_BUILD_RPATH=ON
cmake --build /tmp/hml_ls
/tmp/hml_ls/hml_dropin_linux                                  # loader error
LD_LIBRARY_PATH=/tmp/hml_stage/lib /tmp/hml_ls/hml_dropin_linux  # PASS
```

The `non_rtos` binary can be checked for the absence of POSIX without trusting
the build log:

```bash
nm -u /tmp/hml_nr/hml_dropin_non_rtos | grep -iE 'socket|sendto|recvfrom' || echo none
ldd /tmp/hml_nr/hml_dropin_non_rtos     # libc only
```

## 7. Choosing a partition

| If you are | Use | Because |
|---|---|---|
| writing RTOS/bare-metal firmware | `firmware/hml_object`, `HML_FIRMWARE_POSIX=OFF` | POSIX is never compiled |
| cross-compiling but still on Linux | `firmware/hml_object` with your toolchain | the host's `__linux__` must not leak |
| writing a host test or tool | `find_package(hml)` + `hml::hml_core` | the port is appended for you |
| writing a host tool with its own I/O | `hml_NO_DEFAULT_PORT` + `hml::hml_core` | your own port, no POSIX |
| building a `.so` to be `dlopen`ed | `HML_BUILD_SHARED=ON`, `hml::*_shared` | hidden visibility outside `HML_API` |

## 8. Known gaps

- **`HML_NO_POSIX` is an install flavour, not a build switch.** Section 2.3.
  A genuinely POSIX-free build is `firmware/hml_object`.
- **`hml_firmware_contract_source` is a cache variable.** It is read after
  `hml_firmware_add()` returns, so it is published with `CACHE INTERNAL`. A
  project that adds `firmware/hml_object` twice, or that expects a normal
  variable, will be surprised.
- **The generated port-contract object can be dropped by `--gc-sections`.** See
  section 3.1.
- **The `_shared` arm is not installed by default** for the transports;
  `HML_INSTALL_TRANSPORTS=ON` is required, and even then only for the arm that
  the option also enables.
- **RoCEv2 is experimental and socket-crafted.** It shares no code with the
  Soft-RoCE lab; see `docs/soft_roce_lab.md` for what that lab does and does
  not establish.
