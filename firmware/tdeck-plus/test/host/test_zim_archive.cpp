/* Host test for the ZIM parser: builds a minimal, valid, uncompressed ZIM v6
 * archive (one MIME type, five HTML entries, one cluster) and reads it back
 * with ZimArchive. This exercises header parsing, MIME list, path/title pointer
 * lists, dirent decoding, title search, redirect-free resolution and blob
 * offset handling without a multi-gigabyte Wikipedia download.
 *
 * The title list is written in the order the ZIM spec requires — byte order of
 * <namespace><title> — so the search is tested against a list where the last
 * entry ("alphabet") sorts after "Gamma": a case-insensitive comparison over
 * the raw list would probe past it, and the entries "Gamma" / "Gamma ray"
 * exercise the shared-prefix case.
 *
 * Build:
 *   g++ -std=gnu++17 -I firmware/tdeck-plus/kiwix -I third_party/zstd/lib
 *       firmware/tdeck-plus/test/host/test_zim_archive.cpp
 *       firmware/tdeck-plus/kiwix/zim_reader.cpp <zstd objects> -o /tmp/t
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "zim_reader.h"

namespace
{
void put16(std::vector<uint8_t> &b, size_t off, uint16_t v)
{
    b[off] = (uint8_t)(v & 0xFF);
    b[off + 1] = (uint8_t)(v >> 8);
}

void put32(std::vector<uint8_t> &b, size_t off, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        b[off + i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    }
}

void put64(std::vector<uint8_t> &b, size_t off, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        b[off + i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    }
}

size_t appendDirent(std::vector<uint8_t> &b, const char *path, const char *title, uint32_t cluster, uint32_t blob)
{
    size_t off = b.size();
    b.resize(off + 16);
    put16(b, off, 0);      // MIME index 0 = text/html
    b[off + 2] = 0;        // parameter len
    b[off + 3] = 'C';      // namespace
    put32(b, off + 4, 0);  // revision
    put32(b, off + 8, cluster);
    put32(b, off + 12, blob);
    b.insert(b.end(), path, path + strlen(path) + 1);
    b.insert(b.end(), title, title + strlen(title) + 1);
    return off;
}

bool writeFile(const char *path, const std::vector<uint8_t> &bytes)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return false;
    }
    bool ok = fwrite(bytes.data(), 1, bytes.size(), fp) == bytes.size();
    fclose(fp);
    return ok;
}

// Titles of the results, in the order the search returned them.
std::vector<std::string> resultTitles(const std::vector<ZimEntry> &hits)
{
    std::vector<std::string> out;
    for (const ZimEntry &e : hits) {
        out.push_back(e.title);
    }
    return out;
}

bool hasTitle(const std::vector<ZimEntry> &hits, const char *title)
{
    for (const ZimEntry &e : hits) {
        if (e.title == title) {
            return true;
        }
    }
    return false;
}
} // namespace

int main(void)
{
    // Prefer TMPDIR (set in Nix builds and often writable on locked-down macOS
    // where /tmp itself is not); fall back to a relative path.
    char zimPathBuf[256];
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir && tmpdir[0]) {
        snprintf(zimPathBuf, sizeof(zimPathBuf), "%s/host-test.zim", tmpdir);
    } else {
        snprintf(zimPathBuf, sizeof(zimPathBuf), "host-test.zim");
    }
    const char *zimPath = zimPathBuf;
    std::vector<uint8_t> z;

    z.resize(80, 0); // header filled in later
    const size_t mimeListPos = z.size();
    const char mime[] = "text/html";
    z.insert(z.end(), mime, mime + sizeof(mime)); // includes NUL
    z.push_back('\0');                            // empty string terminates list

    // Entries, in path order (the path pointer list is sorted by path).
    const char *paths[] = {"A", "B", "C", "D", "E"};
    const char *titles[] = {"Alpha", "Beta", "alphabet", "Gamma", "Gamma ray"};
    const char *blobs[] = {"<p>Hello</p>", "<p>World</p>", "<p>letters</p>", "<p>G</p>", "<p>GR</p>"};
    const size_t entries = sizeof(paths) / sizeof(paths[0]);

    size_t direntOff[entries];
    for (size_t i = 0; i < entries; i++) {
        direntOff[i] = appendDirent(z, paths[i], titles[i], 0, (uint32_t)i);
    }

    // One uncompressed cluster: info byte, (entries + 1) offsets, then the blobs.
    const size_t clusterPos = z.size();
    z.push_back(0x01); // uncompressed
    const size_t offsetsPos = z.size();
    z.resize(z.size() + (entries + 1) * 4, 0);
    const uint32_t dataStart = (uint32_t)((entries + 1) * 4);
    uint32_t running = dataStart;
    for (size_t i = 0; i < entries; i++) {
        put32(z, offsetsPos + i * 4, running);
        running += (uint32_t)strlen(blobs[i]);
    }
    put32(z, offsetsPos + entries * 4, running); // sentinel
    for (size_t i = 0; i < entries; i++) {
        z.insert(z.end(), blobs[i], blobs[i] + strlen(blobs[i]));
    }

    const size_t pathPtrPos = z.size();
    z.resize(z.size() + entries * 8, 0);
    for (size_t i = 0; i < entries; i++) {
        put64(z, pathPtrPos + i * 8, direntOff[i]);
    }

    // Title pointer list ordered by <namespace><title> in byte order:
    // Alpha(0), Beta(1), Gamma(3), "Gamma ray"(4), alphabet(2).
    const uint32_t titleOrder[] = {0, 1, 3, 4, 2};
    const size_t titlePtrPos = z.size();
    z.resize(z.size() + entries * 4, 0);
    for (size_t i = 0; i < entries; i++) {
        put32(z, titlePtrPos + i * 4, titleOrder[i]);
    }

    const size_t clusterPtrPos = z.size();
    z.resize(z.size() + 8, 0);
    put64(z, clusterPtrPos, clusterPos);

    const size_t checksumPos = z.size();
    z.resize(z.size() + 16, 0);

    put32(z, 0, 0x044D495Au);
    put16(z, 4, 6); // major
    put16(z, 6, 0); // minor
    put32(z, 24, (uint32_t)entries);
    put32(z, 28, 1); // cluster count
    put64(z, 32, pathPtrPos);
    put64(z, 40, titlePtrPos);
    put64(z, 48, clusterPtrPos);
    put64(z, 56, mimeListPos);
    put32(z, 64, 0); // main page = entry 0
    put32(z, 68, 0xFFFFFFFFu);
    put64(z, 72, checksumPos);

    if (!writeFile(zimPath, z)) {
        printf("FAIL: cannot write %s\n", zimPath);
        return 1;
    }

    ZimArchive archive;
    char err[160] = {0};
    if (!archive.open(zimPath, err, sizeof(err))) {
        printf("FAIL: open: %s\n", err);
        return 2;
    }
    if (archive.entryCount() != (uint32_t)entries || archive.clusterCount() != 1 ||
        archive.majorVersion() != 6) {
        printf("FAIL: header values\n");
        return 3;
    }

    ZimEntry e0, e4;
    if (!archive.direntAt(0, e0) || e0.title != "Alpha" || e0.path != "A" ||
        strcmp(e0.mimeType, "text/html") != 0) {
        printf("FAIL: dirent 0 (%s/%s/%s)\n", e0.path.c_str(), e0.title.c_str(), e0.mimeType);
        return 4;
    }
    if (!archive.direntAt(4, e4) || e4.title != "Gamma ray") {
        printf("FAIL: dirent 4 (%s)\n", e4.title.c_str());
        return 5;
    }

    ZimEntry byTitle;
    if (!archive.findByTitle("Bet", byTitle) || byTitle.title != "Beta") {
        printf("FAIL: title search\n");
        return 6;
    }

    std::vector<uint8_t> blob;
    if (!archive.readBlob(byTitle, blob, err, sizeof(err))) {
        printf("FAIL: readBlob: %s\n", err);
        return 7;
    }
    std::string text((const char *)blob.data(), blob.size());
    if (text != "<p>World</p>") {
        printf("FAIL: blob contents \"%s\"\n", text.c_str());
        return 8;
    }

    ZimEntry main;
    if (!archive.mainPage(main) || main.title != "Alpha") {
        printf("FAIL: main page\n");
        return 9;
    }

    // An upper-case query must find the mixed-case titles, even though byte
    // order puts "alphabet" after "Gamma".
    const std::vector<ZimEntry> upper = archive.searchTitles("GAMMA", 10);
    if (upper.size() != 2 || !hasTitle(upper, "Gamma") || !hasTitle(upper, "Gamma ray")) {
        std::vector<std::string> got = resultTitles(upper);
        printf("FAIL: case search, %u hit(s):", (unsigned)got.size());
        for (const std::string &t : got) {
            printf(" \"%s\"", t.c_str());
        }
        printf("\n");
        return 10;
    }

    // A lower-case query must find a capitalised title.
    const std::vector<ZimEntry> lower = archive.searchTitles("alpha", 10);
    if (lower.size() != 2 || !hasTitle(lower, "Alpha") || !hasTitle(lower, "alphabet")) {
        std::vector<std::string> got = resultTitles(lower);
        printf("FAIL: lower-case search, %u hit(s):", (unsigned)got.size());
        for (const std::string &t : got) {
            printf(" \"%s\"", t.c_str());
        }
        printf("\n");
        return 11;
    }

    // Rows that share a prefix must stay distinct entries: the app opens the
    // dirent it was given, not the first title match for the label.
    const std::vector<ZimEntry> shared = archive.searchTitles("Gamma", 10);
    if (shared.size() != 2 || shared[0].path == shared[1].path) {
        printf("FAIL: shared prefix (%u hit(s))\n", (unsigned)shared.size());
        return 12;
    }

    if (!archive.searchTitles("zzz", 10).empty()) {
        printf("FAIL: unrelated query returned hits\n");
        return 13;
    }

    printf("PASS: ZIM header, dirents, titles, cluster and blob offsets\n");
    return 0;
}
