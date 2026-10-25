#include "build.hpp"

#include "sha256.hpp"

#include "abi_embed.hpp" // DRYDOCK_ABI_HEADER: abi.h as a string
#include <generate.hpp>
#include <ymir_recomp/abi.h>

#include <ymir/media/disc.hpp>
#include <ymir/media/loader/loader.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {
// UTF-8 strings <-> paths (C++20 made u8path/u8string char8_t-based).
fs::path P(const std::string &s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}
std::string S(const fs::path &p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
} // namespace

namespace drydock {

// Bump when generated code changes in a way that needs every library rebuilt.
static constexpr const char *kGeneratorVersion = "drydock-gen-1";

const char *StageName(Stage stage) {
    switch (stage) {
    case Stage::CheckingDisc: return "Checking disc";
    case Stage::Generating: return "Recompiling game code";
    case Stage::Compiling: return "Compiling";
    case Stage::Linking: return "Linking";
    case Stage::Done: return "Done";
    }
    return "";
}

// ---- disc --------------------------------------------------------------------------

namespace {

    // ISO 9660 reader over a loaded disc image (data track, 2048-byte sectors).
    class IsoReader {
    public:
        explicit IsoReader(const ymir::media::Disc &disc) : m_disc(disc) {}

        bool Read(uint32_t lba, uint8_t *out) const {
            const uint32_t fad = lba + 150;
            for (const auto &session : m_disc.sessions)
                for (uint32_t t = 0; t < session.tracks.size(); t++) {
                    const auto &track = session.tracks[t];
                    if (!track.binaryReader || fad < track.startFrameAddress || fad > track.endFrameAddress) continue;
                    return track.ReadSectorUserData(fad, std::span<uint8_t, 2048>(out, 2048));
                }
            return false;
        }

        // Visits every directory record under a directory extent (names without ";1"; "." and ".." skipped).
        template <typename F>
        bool ForEachEntry(uint32_t dirLba, uint32_t dirSize, F &&fn) const {
            for (uint32_t s = 0; s < (dirSize + 2047) / 2048; s++) {
                uint8_t dir[2048];
                if (!Read(dirLba + s, dir)) return false;
                for (uint32_t p = 0; p < 2048 && dir[p] != 0; p += dir[p]) {
                    const uint8_t *r = dir + p;
                    const uint8_t nameLen = r[32];
                    if (nameLen == 1 && (r[33] == 0 || r[33] == 1)) continue; // "." and ".."
                    std::string entry((const char *)r + 33, nameLen);
                    const size_t semi = entry.find(';');
                    if (semi != std::string::npos) entry.resize(semi);
                    if (!entry.empty() && entry.back() == '.') entry.pop_back();
                    fn(entry, Le32(r + 2), Le32(r + 10), (r[25] & 2) != 0);
                }
            }
            return true;
        }

        bool Root(uint32_t &lba, uint32_t &size) const {
            uint8_t sec[2048];
            if (!Read(16, sec) || memcmp(sec + 1, "CD001", 5) != 0) return false;
            lba = Le32(sec + 156 + 2);
            size = Le32(sec + 156 + 10);
            return true;
        }

        // Finds a file by path ("FILE.BIN" or "DIR/FILE.BIN"; names compared without ";1").
        bool FindFile(const std::string &path, uint32_t &lba, uint32_t &size) const {
            uint32_t dirLba, dirSize;
            if (!Root(dirLba, dirSize)) return false;
            size_t from = 0;
            while (true) {
                const size_t slash = path.find('/', from);
                const std::string part = path.substr(from, slash == std::string::npos ? std::string::npos : slash - from);
                const bool wantDir = slash != std::string::npos;
                bool found = false;
                uint32_t foundLba = 0, foundSize = 0;
                ForEachEntry(dirLba, dirSize, [&](const std::string &name, uint32_t l, uint32_t sz, bool isDir) {
                    if (!found && name == part && isDir == wantDir) {
                        found = true;
                        foundLba = l;
                        foundSize = sz;
                    }
                });
                if (!found) return false;
                if (!wantDir) {
                    lba = foundLba;
                    size = foundSize;
                    return true;
                }
                dirLba = foundLba;
                dirSize = foundSize;
                from = slash + 1;
            }
        }

        // Lists every file on the disc in directory order, with its path.
        struct Entry {
            std::string path;
            uint32_t lba, size;
        };
        bool ListFiles(std::vector<Entry> &out) const {
            uint32_t lba, size;
            if (!Root(lba, size)) return false;
            return Walk(lba, size, "", 0, out);
        }

        bool ReadExtent(uint32_t lba, uint32_t size, std::vector<uint8_t> &out) const {
            out.resize(((size_t)size + 2047) / 2048 * 2048);
            for (uint32_t s = 0; s < (size + 2047) / 2048; s++)
                if (!Read(lba + s, out.data() + (size_t)s * 2048)) return false;
            out.resize(size);
            return true;
        }

        bool ReadFile(const std::string &name, std::vector<uint8_t> &out) const {
            uint32_t lba, size;
            if (!FindFile(name, lba, size)) return false;
            return ReadExtent(lba, size, out);
        }

    private:
        bool Walk(uint32_t dirLba, uint32_t dirSize, const std::string &prefix, int depth,
                  std::vector<Entry> &out) const {
            if (depth > 16) return true;
            std::vector<Entry> dirs;
            const bool ok = ForEachEntry(dirLba, dirSize, [&](const std::string &name, uint32_t l, uint32_t sz, bool isDir) {
                if (isDir) dirs.push_back({prefix + name + "/", l, sz});
                else out.push_back({prefix + name, l, sz});
            });
            if (!ok) return false;
            for (const Entry &d : dirs)
                if (!Walk(d.lba, d.size, d.path, depth + 1, out)) return false;
            return true;
        }

        static uint32_t Le32(const uint8_t *p) {
            return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }
        const ymir::media::Disc &m_disc;
    };

    std::string TrimRight(std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
        return s;
    }

} // namespace

DiscCheck CheckDisc(const Recipe &recipe, const std::string &discPath) {
    DiscCheck result;
    const std::string game = recipe.DisplayName();
    ymir::media::Disc disc;
    std::string loaderMessage;
    if (!ymir::media::LoadDisc(P(discPath), disc, false,
                               [&](ymir::media::MessageType, std::string msg) { loaderMessage = msg; })) {
        result.error = "Cannot open the disc image" + (loaderMessage.empty() ? "." : ": " + loaderMessage);
        return result;
    }
    const std::string product = TrimRight(disc.header.productNumber);
    if (product != recipe.product) {
        result.error = "This disc is not " + game + ". Expected product number " + recipe.product + ", found " +
                       (product.empty() ? "none" : product) + ". Please select your copy of " + game + ".";
        return result;
    }
    IsoReader iso(disc);
    for (const RecipeModule &m : recipe.modules) {
        std::vector<uint8_t> data;
        if (!iso.ReadFile(m.file, data)) {
            result.error = "This disc image is incomplete or damaged: " + m.file + " cannot be read.";
            return result;
        }
        if (Sha256Hex(data) != m.sha256) {
            result.error = "This copy of " + game + " is a different version than the one supported (" + m.file +
                           " does not match). Please use the supported disc.";
            return result;
        }
        result.files.push_back(std::move(data));
    }
    result.ok = true;
    return result;
}

DiscInfo InspectDisc(const std::string &discPath, const DiscFileVisitor &visit) {
    DiscInfo info;
    ymir::media::Disc disc;
    std::string loaderMessage;
    if (!ymir::media::LoadDisc(P(discPath), disc, false,
                               [&](ymir::media::MessageType, std::string msg) { loaderMessage = msg; })) {
        info.error = "Cannot open the disc image" + (loaderMessage.empty() ? "." : ": " + loaderMessage);
        return info;
    }
    const auto &h = disc.header;
    info.title = TrimRight(h.gameTitle);
    info.product = TrimRight(h.productNumber);
    info.version = TrimRight(h.version);
    info.releaseDate = TrimRight(h.releaseDate);
    info.areaCodes = TrimRight(h.rawCompatAreaCode);
    info.firstReadAddress = h.firstReadAddress;
    info.firstReadSize = h.firstReadSize;

    IsoReader iso(disc);
    std::vector<IsoReader::Entry> entries;
    if (!iso.ListFiles(entries)) {
        info.error = "No ISO 9660 file system found on the data track.";
        return info;
    }
    for (const auto &e : entries) {
        DiscFile f;
        f.path = e.path;
        f.size = e.size;
        f.lba = e.lba;
        std::vector<uint8_t> data;
        f.readable = iso.ReadExtent(e.lba, e.size, data);
        if (f.readable) f.sha256 = Sha256Hex(data);
        // The BIOS boots the first file of the root directory (file ID 2)
        if (info.bootFile.empty() && e.path.find('/') == std::string::npos) info.bootFile = e.path;
        if (visit && f.readable) visit(f, data);
        info.files.push_back(std::move(f));
    }
    info.ok = true;
    return info;
}

// ---- toolchain and processes ---------------------------------------------------------

Toolchain BundledToolchain(const std::string &exeDir) {
    Toolchain t;
    const fs::path llvm = P(exeDir) / "tools" / "llvm";
#ifdef _WIN32
    t.clang = S(llvm / "clang.exe");
    t.linker = S(llvm / "lld-link.exe");
#else
    t.clang = S(llvm / "clang");
#endif
    return t;
}

namespace {

    // Runs a program without a console window; its output goes to `logPath`.
    int Run(const std::vector<std::string> &args, const std::string &logPath) {
#ifdef _WIN32
        std::wstring cmd;
        for (const std::string &a : args) {
            if (!cmd.empty()) cmd += L' ';
            cmd += L'"' + P(a).wstring() + L'"';
        }
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE log = CreateFileW(P(logPath).wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = log;
        si.hStdError = log;
        si.hStdInput = nullptr;
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                       nullptr, &si, &pi);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (!ok) return -1;
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return (int)code;
#else
        const pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid == 0) {
            const int fd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                dup2(fd, 1);
                dup2(fd, 2);
            }
            std::vector<char *> argv;
            for (const std::string &a : args) argv.push_back(const_cast<char *>(a.c_str()));
            argv.push_back(nullptr);
            execv(argv[0], argv.data());
            _exit(127);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    }

    std::string ReadText(const fs::path &path, size_t maxBytes = 2000) {
        std::ifstream in(path, std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (s.size() > maxBytes) s = s.substr(0, maxBytes) + "...";
        return s;
    }

    bool WriteBytes(const fs::path &path, const std::string &data) {
        std::ofstream out(path, std::ios::binary);
        out.write(data.data(), (std::streamsize)data.size());
        return (bool)out;
    }

    // Fingerprint of everything the library depends on.
    std::string Fingerprint(const Recipe &recipe) {
        std::string s = std::string(kGeneratorVersion) + "|abi" + std::to_string(DRYDOCK_ABI_VERSION) + "|" +
                        recipe.product;
        for (const RecipeModule &m : recipe.modules) s += "|" + m.file + "@" + std::to_string(m.base) + "=" + m.sha256;
        if (!recipe.tracePath.empty()) {
            std::ifstream in(P(recipe.tracePath), std::ios::binary);
            const std::string t((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            s += "|trace=" + Sha256Hex((const uint8_t *)t.data(), t.size());
        }
        return Sha256Hex((const uint8_t *)s.data(), s.size());
    }

    fs::path StampPath(const Recipe &recipe, const std::string &dir) {
        return P(dir) / "recomp" / (recipe.product + ".stamp");
    }

} // namespace

std::string LibraryPath(const Recipe &recipe, const std::string &dir) {
#ifdef _WIN32
    return S(P(dir) / "recomp" / (recipe.product + ".dll"));
#else
    return S(P(dir) / "recomp" / (recipe.product + ".so"));
#endif
}

bool IsLibraryCurrent(const Recipe &recipe, const std::string &dir) {
    std::error_code ec;
    if (!fs::exists(P(LibraryPath(recipe, dir)), ec)) return false;
    std::ifstream in(StampPath(recipe, dir));
    std::string stamp;
    std::getline(in, stamp);
    return stamp == Fingerprint(recipe);
}

// ---- build -------------------------------------------------------------------------

BuildResult BuildGame(const Recipe &recipe, const std::string &discPath, const std::string &dir,
                      const Toolchain &toolchain, const Progress &progress, const std::atomic<bool> *cancel) {
    BuildResult result;
    auto report = [&](Stage s, float f, const std::string &d) {
        if (progress) progress(s, f, d);
    };
    auto cancelled = [&] { return cancel && cancel->load(); };
    std::error_code ec;

    // 1. the player's disc
    report(Stage::CheckingDisc, 0.0f, recipe.DisplayName());
    DiscCheck check = CheckDisc(recipe, discPath);
    if (!check.ok) {
        result.error = check.error;
        return result;
    }
    if (!fs::exists(P(toolchain.clang), ec)) {
        result.error = "The compiler is missing (" + toolchain.clang + "). Please reinstall.";
        return result;
    }

    // 2. recompile to C++
    std::vector<sh2recomp::TraceTarget> trace;
    if (!recipe.tracePath.empty() && !sh2recomp::LoadTrace(recipe.tracePath, trace)) {
        result.error = "Cannot read " + recipe.tracePath + ". Please reinstall.";
        return result;
    }
    std::vector<sh2recomp::ModuleInput> inputs;
    for (size_t i = 0; i < recipe.modules.size(); i++) {
        sh2recomp::ModuleInput in;
        in.name = recipe.modules[i].file;
        in.base = recipe.modules[i].base;
        in.image = std::move(check.files[i]);
        if (recipe.modules[i].useTrace) in.trace = trace;
        inputs.push_back(std::move(in));
    }
    report(Stage::Generating, 0.0f, "");
    sh2recomp::GameOutput gen = sh2recomp::GenerateGame(
        inputs, recipe.product, true, [&](size_t done, size_t total, const std::string &module) {
            report(Stage::Generating, (float)done / (float)total, module);
        });
    inputs.clear();
    if (cancelled()) {
        result.error = "Cancelled.";
        return result;
    }

    const fs::path recompDir = P(dir) / "recomp";
    const fs::path work = recompDir / ("build-" + recipe.product);
    fs::remove_all(work, ec);
    fs::create_directories(work / "include" / "ymir_recomp", ec);
    if (ec || !WriteBytes(work / "include" / "ymir_recomp" / "abi.h", DRYDOCK_ABI_HEADER)) {
        result.error = "Cannot write to " + S(work) + ". Is the folder writable?";
        return result;
    }
    std::vector<fs::path> sources;
    for (const sh2recomp::GeneratedFile &f : gen.files) {
        sources.push_back(work / f.name);
        if (!WriteBytes(sources.back(), f.contents)) {
            result.error = "Cannot write " + S(sources.back()) + ". Is there enough disk space?";
            return result;
        }
    }
    gen.files.clear();
    // biggest first, so the parallel compile ends evenly
    std::sort(sources.begin(), sources.end(),
              [](const fs::path &a, const fs::path &b) { return fs::file_size(a) > fs::file_size(b); });

    // 3. compile
#ifdef _WIN32
    const std::vector<std::string> flags = {"--target=x86_64-pc-windows-msvc", "-mno-stack-arg-probe"};
    const char *objExt = ".obj";
#else
    const std::vector<std::string> flags = {"-fPIC", "-fvisibility=hidden"};
    const char *objExt = ".o";
#endif
    std::vector<std::string> objects(sources.size());
    std::atomic<size_t> next{0}, done{0};
    std::atomic<bool> failed{false};
    std::mutex errMutex;
    std::string compileError;
    unsigned jobs = toolchain.jobs ? toolchain.jobs : std::max(1u, std::thread::hardware_concurrency());
    jobs = std::min<unsigned>(jobs, (unsigned)sources.size());
    report(Stage::Compiling, 0.0f, "");
    auto worker = [&] {
        for (;;) {
            const size_t i = next++;
            if (i >= sources.size() || failed || cancelled()) return;
            const fs::path obj = fs::path(sources[i]).replace_extension(objExt);
            std::vector<std::string> args = {toolchain.clang, "-std=c++17", "-O1", "-w", "-ffreestanding",
                                             "-fno-exceptions", "-fno-rtti", "-nostdinc", "-nostdinc++",
                                             "-DDRYDOCK_FREESTANDING", "-DDRYDOCK_BUILD_LIBRARY",
                                             "-I" + S(work / "include")};
            args.insert(args.end(), flags.begin(), flags.end());
            args.insert(args.end(), {"-c", S(sources[i]), "-o", S(obj)});
            const fs::path log = fs::path(obj).replace_extension(".log");
            if (Run(args, S(log)) != 0) {
                std::lock_guard lock(errMutex);
                if (!failed.exchange(true))
                    compileError = "Compiling " + S(sources[i].filename()) + " failed:\n" + ReadText(log);
                return;
            }
            objects[i] = S(obj);
            const size_t d = ++done;
            report(Stage::Compiling, (float)d / (float)sources.size(), S(sources[i].filename()));
        }
    };
    std::vector<std::thread> threads;
    for (unsigned j = 0; j < jobs; j++) threads.emplace_back(worker);
    for (auto &t : threads) t.join();
    if (cancelled()) {
        result.error = "Cancelled.";
        return result;
    }
    if (failed) {
        result.error = compileError;
        return result;
    }

    // 4. link
    report(Stage::Linking, 0.0f, "");
    const std::string library = LibraryPath(recipe, dir);
    const std::string temp = library + ".tmp";
    std::vector<std::string> link;
#ifdef _WIN32
    const fs::path rsp = work / "link.rsp";
    {
        std::string r = "/dll /noentry /nodefaultlib /noimplib /machine:x64 /out:\"" + temp + "\"\n";
        for (const std::string &o : objects) r += "\"" + o + "\"\n";
        WriteBytes(rsp, r);
    }
    link = {toolchain.linker, "@" + S(rsp)};
#else
    link = {toolchain.clang, "-shared", "-nostdlib", "-o", temp};
    link.insert(link.end(), objects.begin(), objects.end());
#endif
    const fs::path linkLog = work / "link.log";
    if (Run(link, S(linkLog)) != 0) {
        result.error = "Linking failed:\n" + ReadText(linkLog);
        return result;
    }
    fs::remove(P(library), ec);
    fs::rename(P(temp), P(library), ec);
    if (ec) {
        result.error = "Cannot write " + library + " (" + ec.message() + ").";
        return result;
    }
    WriteBytes(StampPath(recipe, dir), Fingerprint(recipe) + "\n");
    if (!toolchain.keepWork) fs::remove_all(work, ec);

    report(Stage::Done, 1.0f, library);
    result.ok = true;
    result.libraryPath = library;
    return result;
}

} // namespace drydock
