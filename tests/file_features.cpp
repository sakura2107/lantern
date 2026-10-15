#include "lantern/win.hpp"
#include <iostream>
#include <thread>
using namespace lantern;
static int checks = 0;
static void check(bool value, const char* text) {
    ++checks;
    if (!value)
        throw std::runtime_error(text);
}
template <class F> void rejects(F action, const char* text) {
    bool failed = false;
    try {
        action();
    } catch (const Error&) {
        failed = true;
    }
    check(failed, text);
}
int main() {
    auto root = fs::temp_directory_path() / wide("Lantern-file-features-" + random_token());
    fs::create_directories(root / L"source/nested");
    try {
        Bytes small{1, 2, 3};
        write_file(root / L"source/b.bin", small);
        write_file(root / L"source/A.bin", Bytes{9});
        write_file(root / L"source/nested/hit.txt", small);
        FileService files(root);
        auto page = files.page("source", 0, 2, "", 0, false, "");
        check(page.total == 3 && page.entries.size() == 2, "page bounds");
        check(page.entries[0].name == "nested" && page.entries[1].name == "A.bin",
              "folders first casefold sort");
        auto next = files.page("source", 2, 2, "", 0, false, page.generation);
        check(next.entries.size() == 1 && next.entries[0].name == "b.bin", "stable page continuation");
        auto size = files.page("source", 0, 256, "", 1, true, "");
        check(size.entries[1].name == "b.bin", "descending size");
        write_file(root / L"source/new.bin", small);
        rejects([&] { files.page("source", 2, 2, "", 0, false, page.generation); },
                "directory mutation invalidates pages");
        auto search = files.search("source", "hit");
        bool done = false;
        size_t hits = 0;
        for (int i = 0; i < 100 && !done; i++) {
            auto chunk = search->next();
            done = chunk.done;
            hits += chunk.entries.size();
            if (!chunk.entries.empty())
                check(chunk.entries[0].name == "nested/hit.txt", "recursive result relative path");
        }
        check(done && hits == 1, "incremental search completion");
        search.reset();
        Cancel cancel;
        uint64_t copied = 0;
        files.copy_tree("source", "copy", cancel, [&](uint64_t n, uint64_t) { copied = n; });
        check(files.stat("copy/nested/hit.txt").hash == sha256(small) && copied > 0,
              "directory copy committed data");
        rejects([&] { files.copy_tree("source", "source/inside", cancel, {}); }, "copy into itself rejected");
        rejects([&] { files.copy_tree("source", "copy", cancel, {}); },
                "copy preserves existing destination");
        Cancel stopped;
        stopped.requested = true;
        rejects([&] { files.copy_tree("source", "cancelled", stopped, {}); }, "copy cancellation");
        check(!fs::exists(root / L"cancelled"), "no partial published");
        Cancel midway;
        rejects(
            [&] {
                files.copy_tree("source", "midway", midway,
                                [&](uint64_t, uint64_t) { midway.requested = true; });
            },
            "midcopy cancellation");
        check(!fs::exists(root / L"midway"), "midcopy no partial published");
        files.remove_tree("copy", cancel, {});
        check(!fs::exists(root / L"copy"), "recursive delete");
        FileJobs jobs(files);
        auto id = jobs.start(0, "source", "job-copy", "owner");
        FileJobInfo result;
        for (int i = 0; i < 500; i++) {
            result = jobs.poll(id, "owner");
            if (result.state == TaskState::completed || result.state == TaskState::failed)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(result.state == TaskState::completed, "async copy complete");
        rejects([&] { jobs.poll(id, "different-owner"); }, "job owner isolation");
        auto hash = jobs.start(2, "source/b.bin", "", "owner", "session");
        for (int i = 0; i < 500; i++) {
            result = jobs.poll(hash, "owner");
            if (result.state == TaskState::completed)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(result.file.hash == sha256(small), "async source hash");
        rejects([&] { files.page("../escape", 0, 256, "", 0, false, ""); }, "page traversal denied");
        rejects([&] { files.search("../escape", "x")->next(); }, "search traversal denied");
        rejects([&] { files.preview_source("../escape"); }, "preview traversal denied");
        rejects([&] { image_thumbnail(small, 128); }, "malformed thumbnail rejected");
        // Handcrafted 1x1 uncompressed 24-bit BMP, decoded by the actual WIC provider.
        Bytes bmp(58);
        bmp[0] = 'B';
        bmp[1] = 'M';
        bmp[2] = 58;
        bmp[10] = 54;
        bmp[14] = 40;
        bmp[18] = 1;
        bmp[22] = 1;
        bmp[26] = 1;
        bmp[28] = 24;
        bmp[34] = 4;
        bmp[56] = 255;
        auto jpeg = image_thumbnail(bmp, 128);
        check(jpeg.size() > 2 && jpeg[0] == 255 && jpeg[1] == 216, "real thumbnail JPEG");
        jobs.stop();
        std::cout << "file features: " << checks << " checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    fs::remove_all(root);
    return 0;
}
