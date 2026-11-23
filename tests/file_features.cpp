#include "lantern/win.hpp"
#include <barrier>
#include <future>
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
template <class F> void rejects_code(Code expected, F action, const char* text) {
    bool matched = false;
    try {
        action();
    } catch (const Error& e) {
        matched = e.code == expected;
    }
    check(matched, text);
}
struct FileIdentity {
    DWORD volume = 0;
    DWORD index_high = 0;
    DWORD index_low = 0;
    bool operator==(const FileIdentity&) const = default;
};
static FileIdentity file_identity(const fs::path& path) {
    Handle h(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    check(h.h != INVALID_HANDLE_VALUE, "open file for NTFS identity");
    BY_HANDLE_FILE_INFORMATION info{};
    check(GetFileInformationByHandle(h, &info) != FALSE, "query NTFS file identity");
    return {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
}
static void verify_concurrent_uploads(const fs::path& base) {
    auto root = base / L"concurrent-uploads";
    fs::create_directory(root);
    FileService files(root);
    Bytes large(8 * 1024 * 1024, 0x51), small(4096, 0x23);
    auto digest = sha256(large);
    auto [id, offset] = files.begin("large.bin", large.size(), digest);
    while (offset < large.size())
        offset = files.chunk(id, offset, std::span(large).subspan(size_t(offset), chunk_size));
    auto stage = [&](const std::string& task) { return root / L".rdx-private" / wide(task); };
    auto original = file_identity(stage(id));
    auto manifest = stage(id);
    manifest += L".meta";
    auto original_manifest = read_file(manifest);

    std::promise<void> in_sha, cancel;
    auto cancel_signal = cancel.get_future();
    int checkpoints = 0;
    auto hashing = std::async(std::launch::async, [&] {
        try {
            files.commit(id, [&] {
                if (++checkpoints != 2) return false; // after the first 1 MiB read
                in_sha.set_value();
                if (cancel_signal.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
                    throw std::runtime_error("independent mutation blocked by upload SHA");
                return true;
            });
            return Code::ok;
        } catch (const Error& e) { return e.code; }
    });
    check(in_sha.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready,
          "large upload reached mid-hash checkpoint");
    auto [second, start] = files.begin("small.bin", small.size(), sha256(small));
    files.chunk(second, start, small);
    auto second_identity = file_identity(stage(second));
    files.commit(second);
    files.mutate(Op::rename, "small.bin", "moved.bin");
    check(file_identity(root / L"moved.bin") == second_identity && read_file(root / L"moved.bin") == small,
          "independent upload, flush, publish and rename completed during SHA with identical NTFS ID");
    files.mutate(Op::remove, "moved.bin");
    cancel.set_value();
    check(hashing.get() == Code::cancelled && checkpoints == 2, "commit cancelled at first MiB");
    check(!fs::exists(root / L"large.bin") && read_file(manifest) == original_manifest &&
          file_identity(stage(id)) == original, "cancel preserved staging identity and manifest");
    auto resumed = files.begin("large.bin", large.size(), digest);
    check(resumed.first == id && resumed.second == large.size(), "cancelled SHA resumes at durable offset");
    files.commit(id);
    check(file_identity(root / L"large.bin") == original && hash_file(root / L"large.bin") == digest,
          "retried SHA published original NTFS object");

    auto [a, a_start] = files.begin("race.bin", small.size(), sha256(small));
    Bytes other(small.size(), 0x32);
    auto [b, b_start] = files.begin("RACE.BIN", other.size(), sha256(other));
    files.chunk(a, a_start, small);
    files.chunk(b, b_start, other);
    auto identity_a = file_identity(stage(a)), identity_b = file_identity(stage(b));
    std::barrier start_commit(2);
    auto race = [&](const std::string& task) {
        start_commit.arrive_and_wait();
        try {
            files.commit(task);
            return Code::ok;
        } catch (const Error& e) { return e.code; }
    };
    auto first = std::async(std::launch::async, [&] { return race(a); });
    auto result_b = race(b), result_a = first.get();
    bool a_won = result_a == Code::ok;
    check((a_won && result_b == Code::conflict) || (result_a == Code::conflict && result_b == Code::ok),
          "same-target commits have one winner and one conflict");
    check(file_identity(root / L"race.bin") == (a_won ? identity_a : identity_b) &&
          read_file(root / L"race.bin") == (a_won ? small : other), "winner contents and NTFS ID preserved");
    std::cout << "upload concurrency: cancelled at 1 MiB, resumed; independent NTFS IDs verified; "
                 "same target: 1 published, 1 conflict\n";
}
static void verify_upload_identity(const fs::path& base) {
    auto upload_root = base / L"upload-identity";
    fs::create_directories(upload_root / L"uploaded");
    Bytes payload(4096);
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = uint8_t((i * 73 + 19) & 0xff);
    auto payload_hash = sha256(payload);
    Bytes resume_data(2048);
    for (size_t i = 0; i < resume_data.size(); ++i)
        resume_data[i] = uint8_t((i * 31 + 7) & 0xff);
    auto resume_hash = sha256(resume_data);
    std::string resume_id;
    FileIdentity staged_resume_id;
    {
        FileService svc(upload_root);
        // 1. Verified staging object and published destination must share volume serial + NTFS file ID.
        auto [id, off] = svc.begin("uploaded/verified.bin", payload.size(), payload_hash);
        check(off == 0, "upload begins at zero offset");
        check(svc.chunk(id, 0, std::span<const uint8_t>(payload).first(1500)) == 1500, "first upload chunk");
        check(svc.chunk(id, 1500, std::span<const uint8_t>(payload).subspan(1500)) == payload.size(),
              "second upload chunk");
        auto staged_path = upload_root / L".rdx-private" / wide(id);
        auto meta_path = staged_path;
        meta_path += L".meta";
        auto staged_id = file_identity(staged_path);
        svc.commit(id, {});
        auto published_path = upload_root / L"uploaded/verified.bin";
        auto published_id = file_identity(published_path);
        check(published_id == staged_id, "published file identity matches verified staging object");
        check(read_file(published_path) == payload && !fs::exists(staged_path) && !fs::exists(meta_path),
              "published upload matches payload and removes staging artifacts");

        // 2. Existing destination is refused without overwriting or corrupting original file.
        Bytes original{'k', 'e', 'e', 'p', '-', 'o', 'r', 'i', 'g'};
        auto existing_path = upload_root / L"uploaded/existing.bin";
        write_file(existing_path, original);
        auto existing_id = file_identity(existing_path);
        rejects_code(
            Code::conflict, [&] { svc.begin("uploaded/existing.bin", payload.size(), payload_hash); },
            "upload begin rejects existing destination");
        check(read_file(existing_path) == original && file_identity(existing_path) == existing_id,
              "existing destination untouched after begin conflict");

        auto [cid, coff] = svc.begin("uploaded/raced.bin", payload.size(), payload_hash);
        check(coff == 0, "raced upload begins at zero");
        check(svc.chunk(cid, 0, payload) == payload.size(), "raced upload chunks written");
        auto raced_path = upload_root / L"uploaded/raced.bin";
        write_file(raced_path, original);
        auto raced_orig_id = file_identity(raced_path);
        rejects_code(
            Code::conflict, [&] { svc.commit(cid, {}); },
            "upload commit refuses to overwrite existing destination");
        check(read_file(raced_path) == original && file_identity(raced_path) == raced_orig_id,
              "existing destination content and identity preserved on commit conflict");

        // 3. Hash verification failure retains staging file and recovery manifest.
        Bytes expected_data{'v', 'a', 'l', 'i', 'd', '-', 'd', 'a', 't', 'a'};
        Bytes corrupt_data{'b', 'r', 'o', 'k', 'e', 'n', 'd', 'a', 't', 'a'};
        auto [bad_id, bad_off] =
            svc.begin("uploaded/corrupt.bin", expected_data.size(), sha256(expected_data));
        check(bad_off == 0, "corrupt upload begins at zero");
        check(svc.chunk(bad_id, 0, corrupt_data) == corrupt_data.size(), "corrupt upload chunk written");
        auto bad_stage = upload_root / L".rdx-private" / wide(bad_id);
        auto bad_meta = bad_stage;
        bad_meta += L".meta";
        rejects_code(
            Code::integrity, [&] { svc.commit(bad_id, {}); }, "upload hash mismatch rejected");
        check(!fs::exists(upload_root / L"uploaded/corrupt.bin"), "corrupt upload not published");
        check(fs::exists(bad_stage) && fs::exists(bad_meta),
              "hash mismatch retains staging file and recovery manifest");
        check(read_file(bad_stage) == corrupt_data, "retained staging payload intact");

        // 4a. Persist partial upload before tearing down FileService.
        auto [rid, roff] = svc.begin("uploaded/resumed.bin", resume_data.size(), resume_hash);
        check(roff == 0, "resumable upload starts at zero");
        check(svc.chunk(rid, 0, std::span<const uint8_t>(resume_data).first(777)) == 777,
              "partial upload persisted");
        resume_id = rid;
        staged_resume_id = file_identity(upload_root / L".rdx-private" / wide(resume_id));
    }
    // 4b. Rebuild FileService, resume from persisted offset, and commit with preserved file identity.
    {
        FileService resumed(upload_root);
        auto [rid, roff] = resumed.begin("uploaded/resumed.bin", resume_data.size(), resume_hash);
        check(rid == resume_id && roff == 777, "rebuilt FileService resumes from persisted offset");
        check(resumed.chunk(rid, roff, std::span<const uint8_t>(resume_data).subspan(777)) ==
                  resume_data.size(),
              "resumed upload remainder written");
        auto staged_path = upload_root / L".rdx-private" / wide(rid);
        auto meta_path = staged_path;
        meta_path += L".meta";
        auto staged_before_commit = file_identity(staged_path);
        check(staged_before_commit == staged_resume_id, "resumed upload retains staging file identity");
        resumed.commit(rid, {});
        auto published_path = upload_root / L"uploaded/resumed.bin";
        check(file_identity(published_path) == staged_before_commit,
              "resumed commit publishes identical NTFS file object");
        check(read_file(published_path) == resume_data && !fs::exists(staged_path) && !fs::exists(meta_path),
              "resumed upload committed cleanly");
    }
}
static void set_mtime(const fs::path& path, uint64_t ticks) {
    Handle h(CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    check(h.h != INVALID_HANDLE_VALUE, "open path for SetFileTime");
    FILETIME ft{DWORD(ticks & 0xffffffffu), DWORD(ticks >> 32)};
    check(SetFileTime(h, nullptr, nullptr, &ft) != FALSE, "SetFileTime");
}
static std::vector<std::string> entry_names(const DirectoryPage& p) {
    std::vector<std::string> names;
    names.reserve(p.entries.size());
    for (const auto& e : p.entries)
        names.push_back(e.name);
    return names;
}
static std::string raw_dir_version(const fs::path& dir) {
    Handle h(CreateFileW(dir.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    check(h.h != INVALID_HANDLE_VALUE, "open sort dir for generation");
    BY_HANDLE_FILE_INFORMATION i{};
    check(GetFileInformationByHandle(h, &i) != FALSE, "query sort dir generation");
    return std::to_string(i.dwVolumeSerialNumber) + ":" + std::to_string(i.nFileIndexHigh) + ":" +
           std::to_string(i.nFileIndexLow) + ":" + std::to_string(i.ftLastWriteTime.dwHighDateTime) + ":" +
           std::to_string(i.ftLastWriteTime.dwLowDateTime);
}
static void verify_directory_sorting(const fs::path& base) {
    auto dir = base / L"sort-regression";
    fs::create_directories(dir / L"dirB");
    fs::create_directories(dir / L"DirA");
    // Equal size (5 B) & equal mtime files to exercise folded-name tie-breaks across all sort modes.
    constexpr uint64_t t_same = 133500000000000000ULL;
    constexpr uint64_t t_newer = 133500000600000000ULL;
    constexpr uint64_t t_older = 133499999400000000ULL;
    write_file(dir / L"c_same.bin", Bytes(5, 1));
    write_file(dir / L"B_same.bin", Bytes(5, 2));
    write_file(dir / L"a_same.bin", Bytes(5, 3));
    write_file(dir / L"d_big.bin", Bytes(20, 4));
    write_file(dir / L"e_small.bin", Bytes(2, 5));
    set_mtime(dir / L"DirA", t_same);
    set_mtime(dir / L"dirB", t_same);
    set_mtime(dir / L"c_same.bin", t_same);
    set_mtime(dir / L"B_same.bin", t_same);
    set_mtime(dir / L"a_same.bin", t_same);
    set_mtime(dir / L"d_big.bin", t_newer);
    set_mtime(dir / L"e_small.bin", t_older);

    FileService svc(base);
    // 1. Name sort ascending & descending (folders always first, casefolded order).
    auto name_asc = svc.page("sort-regression", 0, 256, "", 0, false, "");
    check(entry_names(name_asc) ==
              std::vector<std::string>{"DirA", "dirB", "a_same.bin", "B_same.bin", "c_same.bin", "d_big.bin",
                                       "e_small.bin"},
          "name sort ascending");
    auto name_desc = svc.page("sort-regression", 0, 256, "", 0, true, "");
    check(entry_names(name_desc) ==
              std::vector<std::string>{"dirB", "DirA", "e_small.bin", "d_big.bin", "c_same.bin", "B_same.bin",
                                       "a_same.bin"},
          "name sort descending keeps directories first");

    // 2. Generation fingerprint byte-exact check against specification.
    Writer fp;
    fp.str(raw_dir_version(dir));
    fp.str("");
    fp.u32(0);
    fp.u32(0);
    for (const auto& e : name_asc.entries) {
        fp.str(e.name);
        fp.u32(e.directory);
        fp.u64(e.size);
        fp.u64(e.mtime);
    }
    check(name_asc.generation == sha256(fp.b), "generation fingerprint byte-identical");

    // 3. Size sort ascending & descending (directories share size=0 -> tie-break by folded name;
    //    equal-size files a/B/c also tie-break by folded name in both directions).
    auto size_asc = svc.page("sort-regression", 0, 256, "", 1, false, "");
    check(entry_names(size_asc) ==
              std::vector<std::string>{"DirA", "dirB", "e_small.bin", "a_same.bin", "B_same.bin", "c_same.bin",
                                       "d_big.bin"},
          "size sort ascending with same-size name tie-break");
    auto size_desc = svc.page("sort-regression", 0, 256, "", 1, true, "");
    check(entry_names(size_desc) ==
              std::vector<std::string>{"dirB", "DirA", "d_big.bin", "c_same.bin", "B_same.bin", "a_same.bin",
                                       "e_small.bin"},
          "size sort descending with same-size name tie-break");

    // 4. Mtime sort ascending & descending (equal mtime dirs & files tie-break by folded name).
    auto mtime_asc = svc.page("sort-regression", 0, 256, "", 2, false, "");
    check(entry_names(mtime_asc) ==
              std::vector<std::string>{"DirA", "dirB", "e_small.bin", "a_same.bin", "B_same.bin", "c_same.bin",
                                       "d_big.bin"},
          "mtime sort ascending with same-mtime name tie-break");
    auto mtime_desc = svc.page("sort-regression", 0, 256, "", 2, true, "");
    check(entry_names(mtime_desc) ==
              std::vector<std::string>{"dirB", "DirA", "d_big.bin", "c_same.bin", "B_same.bin", "a_same.bin",
                                       "e_small.bin"},
          "mtime sort descending with same-mtime name tie-break");

    // 5. Case-insensitive filter + paged continuation + external change invalidation.
    auto filt0 = svc.page("sort-regression", 0, 2, "SAME", 0, false, "");
    check(filt0.total == 3 && entry_names(filt0) == std::vector<std::string>{"a_same.bin", "B_same.bin"},
          "case-insensitive filter page 0");
    auto filt1 = svc.page("sort-regression", 2, 2, "SAME", 0, false, filt0.generation);
    check(entry_names(filt1) == std::vector<std::string>{"c_same.bin"}, "filtered page continuation");
    write_file(dir / L"z_same.bin", Bytes(5, 9));
    rejects_code(
        Code::conflict, [&] { svc.page("sort-regression", 2, 2, "SAME", 0, false, filt0.generation); },
        "external addition invalidates filtered pagination generation");
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
        verify_upload_identity(root);
        verify_concurrent_uploads(root);
        verify_directory_sorting(root);
        jobs.stop();
        std::cout << "file features: " << checks << " checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    fs::remove_all(root);
    return 0;
}
