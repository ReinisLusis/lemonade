// Does a download hand itself to the system downloader when this machine has
// one, and fetch itself when it does not?
//
// Needs a supervisor running against the store this machine is configured for,
// so it is opt-in:
//
//   ABSTRACTION_STORE=C:/temp/sysstore  jobd run
//   test_offload.exe <https url> <bytes> <sha256:hex> <output path>
//
// Without a supervisor it asserts the opposite: offload declines, and the
// caller is expected to download for itself exactly as before.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <abstraction/job/discovery.h>
#include <lemon/download_job.h>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace lemon;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

int main(int argc, char** argv) {
    const std::string store = abstraction::job::machine_store();
    const abstraction::job::Supervisor sup = abstraction::job::supervisor_of(store);

    std::printf("machine store : %s\n", store.empty() ? "(none configured)" : store.c_str());
    std::printf("supervisor    : %s%s\n", sup.owner.empty() ? "(none)" : sup.owner.c_str(),
                sup.alive ? " [alive]" : " [not alive]");
    if (!sup.tier.empty()) {
        std::printf("  which itself delegates to: %s\n", sup.tier.c_str());
    }
    std::printf("\n");

    check("discovery agrees with the heartbeat",
          download::offload_available() == sup.alive);

    if (!sup.alive) {
        check("with no supervisor, offload declines so the caller fetches it itself",
              !download::offload_available());
        std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    if (argc < 5) {
        std::printf("a supervisor is running; pass <url> <bytes> <digest> <out> to hand it a real transfer\n");
        std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    download::Context ctx;
    ctx.url = argv[1];
    ctx.size = std::atoll(argv[2]);
    ctx.digest = argv[3];
    ctx.final_path = argv[4];
    ctx.partial_path = std::string(argv[4]) + ".partial";
    ctx.group_id = "test:offload";
    ctx.display_name = "offload test";
    ctx.file = fs::path(argv[4]).filename().string();
    ctx.file_index = 0;
    ctx.total_files = 1;

    std::error_code ec;
    fs::remove(ctx.final_path, ec);
    fs::remove(ctx.partial_path, ec);

    std::int64_t last = -1;
    const auto started = std::chrono::steady_clock::now();
    std::string error;
    const bool handled = download::offload(
        ctx,
        [&](std::size_t done, std::size_t total) {
            const std::int64_t pct = total ? static_cast<std::int64_t>(100 * done / total) : 0;
            if (pct != last && pct % 10 == 0) {
                std::printf("   %3lld%%  %llu / %llu   (this process is not moving these bytes)\n",
                            static_cast<long long>(pct), static_cast<unsigned long long>(done),
                            static_cast<unsigned long long>(total));
                last = pct;
            }
            return true;
        },
        error);

    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();

    check("offload took the transfer", handled);
    check("it reported no error", error.empty());
    if (!error.empty()) std::printf("      error: %s\n", error.c_str());
    check("the file is where the caller asked for it", fs::exists(ctx.final_path));
    if (fs::exists(ctx.final_path)) {
        const auto got = fs::file_size(ctx.final_path);
        check("and it is the right size", static_cast<std::int64_t>(got) == ctx.size);
        std::printf("      %llu bytes in %llds, fetched by %s\n",
                    static_cast<unsigned long long>(got), static_cast<long long>(secs),
                    sup.owner.c_str());
    }

    std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
