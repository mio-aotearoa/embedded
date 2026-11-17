/* Host test for the ZIM parser: builds a minimal, valid, uncompressed ZIM v6
 * archive (one MIME type, two HTML entries, one cluster) and reads it back with
 * ZimArchive. This exercises header parsing, MIME list, path/title pointer
 * lists, dirent decoding, title search, redirect-free resolution and blob
 * offset handling without a multi-gigabyte Wikipedia download.
 *
 * Build:
 *   g++ -std=gnu++17 -I firmware/tdeck-plus/kiwix -I third_party/zstd/lib
 *       firmware/tdeck-plus/test/host/test_zim_archive.cpp
 *       firmware/tdeck-plus/kiwix/zim_reader.cpp <zstd objects> -o /tmp/t
 */
#include <stdint.h>
#include <stdio.h>
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
} // namespace

int main(void)
{
    const char *zimPath = "/tmp/host-test.zim";
    std::vector<uint8_t> z;

    z.resize(80, 0); // header filled in later
    const size_t mimeListPos = z.size();
    const char mime[] = "text/html";
    z.insert(z.end(), mime, mime + sizeof(mime)); // includes NUL
    z.push_back('\0');                            // empty string terminates list

    const size_t direntA = appendDirent(z, "A", "Alpha", 0, 0);
    const size_t direntB = appendDirent(z, "B", "Beta", 0, 1);

    // One uncompressed cluster: info byte, 3 offsets (2 blobs + sentinel),
    // then the two blobs.
    const char blobA[] = "<p>Hello</p>";
    const char blobB[] = "<p>World</p>";
    const size_t clusterPos = z.size();
    z.push_back(0x01); // uncompressed
    const size_t offsetsPos = z.size();
    z.resize(z.size() + 12, 0);
    const uint32_t dataStart = 12;
    put32(z, offsetsPos + 0, dataStart);
    put32(z, offsetsPos + 4, dataStart + (uint32_t)strlen(blobA));
    put32(z, offsetsPos + 8, dataStart + (uint32_t)strlen(blobA) + (uint32_t)strlen(blobB));
    z.insert(z.end(), blobA, blobA + strlen(blobA));
    z.insert(z.end(), blobB, blobB + strlen(blobB));

    const size_t pathPtrPos = z.size();
    z.resize(z.size() + 16, 0);
    put64(z, pathPtrPos + 0, direntA);
    put64(z, pathPtrPos + 8, direntB);

    const size_t titlePtrPos = z.size();
    z.resize(z.size() + 8, 0);
    put32(z, titlePtrPos + 0, 0); // Alpha (dirent index 0)
    put32(z, titlePtrPos + 4, 1); // Beta  (dirent index 1)

    const size_t clusterPtrPos = z.size();
    z.resize(z.size() + 8, 0);
    put64(z, clusterPtrPos, clusterPos);

    const size_t checksumPos = z.size();
    z.resize(z.size() + 16, 0);

    put32(z, 0, 0x044D495Au);
    put16(z, 4, 6); // major
    put16(z, 6, 0); // minor
    put32(z, 24, 2); // entry count
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
    if (archive.entryCount() != 2 || archive.clusterCount() != 1 || archive.majorVersion() != 6) {
        printf("FAIL: header values\n");
        return 3;
    }

    ZimEntry e0, e1;
    if (!archive.direntAt(0, e0) || e0.title != "Alpha" || e0.path != "A" || strcmp(e0.mimeType, "text/html") != 0) {
        printf("FAIL: dirent 0 (%s/%s/%s)\n", e0.path.c_str(), e0.title.c_str(), e0.mimeType);
        return 4;
    }
    if (!archive.direntAt(1, e1) || e1.title != "Beta") {
        printf("FAIL: dirent 1\n");
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

    printf("PASS: ZIM header, dirents, titles, cluster and blob offsets\n");
    return 0;
}
