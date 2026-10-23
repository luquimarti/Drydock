# Drydock

Static recompilation of Saturn games' SH-2 code, running on top of the
[Ymir](https://github.com/StrikerX3/Ymir) emulator core.

Ymir keeps emulating the hardware (VDP1/VDP2, sound, CD, SCU, SMPC). The game's
CPU code is translated ahead of time into C++ and compiled to native code; Ymir's
interpreter runs whatever was not translated. The approach is the same as
[N64Recomp](https://github.com/N64Recomp/N64Recomp).

Drydock is the shipyard. Each game is a **ship**: its own repository with a
recipe and releases for players (the first one is
[TheBortas](https://github.com/luquimarti/TheBortas)).

**No game data lives here or in any ship.** Players provide their own disc and
BIOS; the game is recompiled on their own machine.

## How it works

1. **Recipe** (per ship): the disc's product number, which files on the disc
   hold SH-2 code, where the game loads them, their SHA-256 hashes and a list
   of control-transfer addresses recorded while playing.
2. **Recompile** (`tools/sh2recomp`): static function discovery plus the
   trace, every module (main executable and overlays) translated to C++
   against a small C ABI (`runtime/include/ymir_recomp/abi.h`).
3. **Build** (`tools/drydock`): checks the player's disc against the recipe,
   recompiles, and compiles the result with a bare Clang (no system headers,
   no C runtime) into `recomp/<product>.dll`.
4. **Run** (`runtime/`): before each interpreted instruction the SH-2 asks the
   runtime for native code at PC. Native code runs when its code word, its
   module's check words and the disc's product number all match, so overlays
   loaded at the same address never run each other's code.

Native code charges exactly the cycles Ymir's interpreter charges (fixed cost,
memory access cycles and load-use stalls), so timing matches the interpreter:
the differential fuzzer compares registers, memory and cycle counts, and in
game tests frames are byte-identical between native and interpreted runs.

## Layout

```text
drydock.cmake      library targets (also used by the Ymir fork)
runtime/           runtime + ABI used by generated code
tools/sh2gen/      SH-2 -> C++ emitter
tools/sh2recomp/   whole-game recompiler (static discovery, modules)
tools/drydock/     recipe + disc -> game library
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
ymir-headless bios.bin game.cue 7000 --press 1300:1306:start ... --trace trace.txt

# build a game library from a recipe and a disc image
drydock --recipe ship/recipe.toml --disc game.cue --out . --clang /usr/bin/clang

# run it
ymir-headless bios.bin game.cue 7000 --library recomp/<product>.so
```

## License

GPLv3, like Ymir (`ymir-core` is linked into every tool). Not affiliated with
any game or console maker.
