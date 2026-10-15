# `renode/boards/` -- the machine-checked vendor index

One file, [`index`](index), plus this note.

## Why an index at all

The multi-vendor harness has the same five facts about a platform in five
different places: the `.resc` that models it, the linker script that lays the
image out in it, the `board_config.h` the firmware compiles with, the `vendor.env`
the build script reads, and the `.robot` that asserts on it. Five copies of one
truth is five chances to drift, and drift in a memory map is the nasty kind: the
image loads into a region the emulated machine does not have and the failure
looks like a wild jump rather than a mismatched map.

So `index` is the manifest, and
[`../scripts/selfcheck_vendors.sh`](../scripts/selfcheck_vendors.sh) asserts every
one of those five places against it. The check runs with no emulator, no cross
compiler and no network, and it is a CTest test (`<vendor>_renode_selfcheck`)
that is expected to pass on every machine. If the five ever disagree, that test
goes red.

## Format

`index` is a `#`-commented TSV: one header line naming the columns, then one row
per vendor. It is deliberately not a Markdown table, because it has to be parsed
by a script without a Markdown parser. It still reads fine as-is.

## Adding a vendor

1. Add the row to `index`.
2. Add `renode/vendors/<vendor>/` with the `.resc`, the `.robot`, the
   `firmware/vendor.env`, the `firmware/platform/board_config.h`, the linker
   script and the startup file.
3. Register the test in `../CMakeLists.txt`.
4. Run `../scripts/selfcheck_vendors.sh` and fix whatever it reports.

## Status

**Nothing in this index has ever been executed.** The machine this was written
on has no Renode, no cross compiler and no network access to either. These rows
describe what *will* be asked to run when a real toolchain appears; they are not
results. See [`../../docs/renode_multi_vendor.md`](../../docs/renode_multi_vendor.md).
