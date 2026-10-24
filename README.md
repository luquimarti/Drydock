# Drydock

**Static recompilation for the Saturn. Native SH-2 code, cycle-exact, on top of [Ymir](https://github.com/StrikerX3/Ymir).**

[![CI](https://github.com/luquimarti/Drydock/actions/workflows/ci.yml/badge.svg)](https://github.com/luquimarti/Drydock/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/license-GPLv3-blue.svg)](LICENSE)
[![Made in Argentina](https://img.shields.io/badge/made%20in-Argentina%20%F0%9F%87%A6%F0%9F%87%B7-74ACDF)](#)
[![Patreon](https://img.shields.io/badge/Patreon-support-F96854?logo=patreon&logoColor=white)](https://www.patreon.com/cw/luquimarti)

Drydock takes the SH-2 code of a Saturn title, translates it ahead of time into
C++ and compiles it into native PC code. That code then runs inside the Ymir
emulator, which keeps handling everything else: video (VDP1/VDP2), sound, CD,
SCU and SMPC. Anything that wasn't recompiled falls back to Ymir's interpreter
automatically, so the result always runs correctly. The approach is the same as
[N64Recomp](https://github.com/N64Recomp/N64Recomp).

## The shipyard and its ships

Drydock is the **shipyard**: the generic toolchain, runtime and emulator hooks.
It doesn't know about any specific title.

A **ship** is one title built in the shipyard. Each ship lives in its own
repository and contains:

- a **recipe** (`ship/recipe.toml`): which files on the disc hold SH-2 code,
  where they load in memory, their SHA-256 hashes, and a trace of addresses
  recorded while playing. No copyrighted data, just a description of the disc.
- default settings and a release workflow that packages a ready-to-use build
  for players.

Ships are named after starships, because every one of them is a cruiser built
in the same dock.

| Ship | Title | Status |
|---|---|---|
| [**TheBortas**](https://github.com/luquimarti/TheBortas) | PDZ II (USA) | First voyage: playable on Windows x64 |

**No copyrighted content lives here or in any ship.** Players bring their own
disc and BIOS, and the native code is built on their own machine.

## How it works

1. **Recipe** (per ship): the disc's product number, its code files, their
   load addresses and hashes, and the recorded control-transfer addresses.
2. **Recompile** (`tools/sh2recomp`): static function discovery plus the
   trace. Every module (main executable and overlays) is translated to C++
   against a small C ABI (`runtime/include/ymir_recomp/abi.h`).
3. **Build** (`tools/drydock`): checks the player's disc against the recipe,
   recompiles, and compiles the result with a bare Clang (no system headers,
   no C runtime) into `recomp/<product>.dll`.
4. **Run** (`runtime/`): before each interpreted instruction, the SH-2 asks the
   runtime for native code at PC. Native code runs only when its code word, its
   module's check words and the disc's product number all match, so overlays
   loaded at the same address never run each other's code.

### Accuracy first

Native code charges exactly the cycles Ymir's interpreter charges (fixed cost,
memory access cycles and load-use stalls), so timing is identical. A
differential fuzzer compares registers, memory and cycle counts against the
interpreter, and on real titles every frame is byte-identical between native
and interpreted runs.

## Roadmap

- [x] Whole-program recompilation with overlays
- [x] Cycle-exact native code, verified frame by frame
- [x] Ship mode: guided BIOS/disc setup and one-time local build
- [ ] Modding / hook system
- [ ] Image filters (scanlines and others)
- [ ] Higher internal resolution and true widescreen
- [ ] More ships

## Layout

```text
drydock.cmake      library targets (also used by the Ymir fork)
runtime/           runtime + ABI used by generated code
tools/sh2gen/      SH-2 -> C++ emitter
tools/sh2recomp/   whole-program recompiler (static discovery, modules)
tools/drydock/     recipe + disc -> native library
tools/headless/    windowless runner: scripted input, screenshots, traces
tests/fuzz/        differential fuzzer against Ymir's interpreter
patches/           the changes in the Ymir fork, as patches
scripts/           Windows build of the frontend
external/Ymir      Ymir fork with the Drydock hooks (submodule)
```

## Building

```sh
git clone --recursive https://github.com/luquimarti/Drydock
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_LINKER_TYPE=LLD
cmake --build build
build/tests/fuzz/sh2fuzz-run build/tests/fuzz/fuzz_programs.bin
```

The Ymir frontend with Drydock (and ship mode) is built from the fork with
`-DYMIR_RECOMP_ROOT=<path to Drydock>`; see `scripts/build_windows.bat` and a
ship's release workflow.

## Tools

```sh
# record control transfers while playing (interpreted)
ymir-headless bios.bin disc.cue 7000 --press 1300:1306:start ... --trace trace.txt

# build a native library from a recipe and a disc image
drydock --recipe ship/recipe.toml --disc disc.cue --out . --clang /usr/bin/clang

# run it
ymir-headless bios.bin disc.cue 7000 --library recomp/<product>.so
```

## Get involved

- **Try a ship** and report what you find in its own repository's issues.
- **Found a bug in the recompiler or runtime?** Open an
  [issue](https://github.com/luquimarti/Drydock/issues) here.
- **Want to build a new ship?** Open an issue with the title and region you
  have in mind, and let's figure out the recipe together.
- **Pull requests are welcome**: fixes, fuzzer cases, docs, platform support.
- Star the repo if you'd like to follow along.

## Support

Drydock is free and open source, and it will stay that way. If you'd like to
help fund development time, you can support it on
**[Patreon](https://www.patreon.com/cw/luquimarti)**. There are no rewards or
early access, just thanks.

## Credits

- [Ymir](https://github.com/StrikerX3/Ymir) by StrikerX3, the emulator Drydock
  runs on.
- [N64Recomp](https://github.com/N64Recomp/N64Recomp), which showed the way.

## License

GPLv3, like Ymir (`ymir-core` is linked into every tool). Not affiliated with
any publisher or hardware maker; all trademarks belong to their owners.

---

🇦🇷 Made in Argentina.
