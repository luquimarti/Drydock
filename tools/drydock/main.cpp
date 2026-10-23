// drydock-build: recompiles a game from the player's own disc into a native
// library the runtime loads (see build.hpp).
//
//   drydock-build --recipe games/pdz/recipe.toml --disc game.cue --out DIR
//                 [--clang PATH] [--linker PATH] [--jobs N] [--keep] [--check]
//
// --check only verifies the disc against the recipe.

#include "build.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char **argv) {
    std::string recipePath, disc, out = ".", clang, linker;
    unsigned jobs = 0;
    bool keep = false, checkOnly = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--recipe" && i + 1 < argc) recipePath = argv[++i];
        else if (a == "--disc" && i + 1 < argc) disc = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--clang" && i + 1 < argc) clang = argv[++i];
        else if (a == "--linker" && i + 1 < argc) linker = argv[++i];
        else if (a == "--jobs" && i + 1 < argc) jobs = (unsigned)atoi(argv[++i]);
        else if (a == "--keep") keep = true;
        else if (a == "--check") checkOnly = true;
        else {
            fprintf(stderr, "usage: %s --recipe FILE --disc IMAGE [--out DIR] [--clang PATH] [--linker PATH] "
                            "[--jobs N] [--keep] [--check]\n", argv[0]);
            return 2;
        }
    }
    if (recipePath.empty() || disc.empty()) {
        fprintf(stderr, "--recipe and --disc are required\n");
        return 2;
    }
    drydock::Recipe recipe;
    std::string error;
    if (!drydock::LoadRecipe(recipePath, recipe, error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (checkOnly) {
        const drydock::DiscCheck c = drydock::CheckDisc(recipe, disc);
        printf("%s\n", c.ok ? ("OK: " + recipe.DisplayName()).c_str() : c.error.c_str());
        return c.ok ? 0 : 1;
    }
    drydock::Toolchain tc = drydock::BundledToolchain(out);
    if (!clang.empty()) tc.clang = clang;
    if (!linker.empty()) tc.linker = linker;
    tc.jobs = jobs;
    tc.keepWork = keep;

    const auto t0 = std::chrono::steady_clock::now();
    drydock::Stage last = drydock::Stage::Done;
    const drydock::BuildResult r = drydock::BuildGame(
        recipe, disc, out, tc, [&](drydock::Stage s, float f, const std::string &d) {
            if (s != last) {
                printf("\n%s", drydock::StageName(s));
                last = s;
            }
            printf("\r%-24s %3d%%  %-40s", drydock::StageName(s), (int)(f * 100), d.c_str());
            fflush(stdout);
        });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("\n");
    if (!r.ok) {
        fprintf(stderr, "%s\n", r.error.c_str());
        return 1;
    }
    printf("%s ready in %.0f s: %s\n", recipe.DisplayName().c_str(), secs, r.libraryPath.c_str());
    return 0;
}
