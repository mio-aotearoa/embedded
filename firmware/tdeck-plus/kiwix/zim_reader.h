// Minimal read-only ZIM (Kiwix) archive reader for the T-Deck Plus.
//
// Scope: enough of the format to browse text articles from an SD card on a
// 320x240 screen. ZIM v5/v6, content and redirect entries, uncompressed and
// Zstandard clusters. LZMA2/XZ clusters (older archives) are rejected with a
// clear message instead of being misread.
//
// Format reference: openzim.org/wiki/ZIM_file_format. libzim itself is not used
// because it needs a desktop C++17 runtime, ICU and XZ; this is the small subset
// the reader needs. Rendering is not done here either: the extracted HTML is
// displayed by wadamesh's own Reader (see kiwix_app.cpp).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <string>
#include <vector>

struct ZimEntry
{
    uint16_t mime = 0;
    char ns = 'C';
    uint32_t revision = 0;
    uint32_t cluster = 0;
    uint32_t blob = 0;
    bool isRedirect = false;
    uint32_t redirectIndex = 0;
    std::string path;
    std::string title;
    const char *mimeType = "";
};

class ZimArchive
{
  public:
    ~ZimArchive();

    bool open(const char *path, char *err, size_t errCap);
    void close(void);
    bool isOpen(void) const { return _fp != nullptr; }

    uint16_t majorVersion(void) const { return _major; }
    uint32_t entryCount(void) const { return _entryCount; }
    uint32_t clusterCount(void) const { return _clusterCount; }
    const char *path(void) const { return _path.c_str(); }

    bool direntAt(uint32_t index, ZimEntry &out);
    bool resolve(const ZimEntry &entry, ZimEntry &out, int depth = 0);
    bool mainPage(ZimEntry &out);

    // Case-insensitive prefix search over the title pointer list (binary search
    // plus a forward scan for the prefix). `limit` caps the returned matches.
    std::vector<ZimEntry> searchTitles(const char *prefix, size_t limit);
    bool findByTitle(const char *prefix, ZimEntry &out);

    bool readBlob(const ZimEntry &entry, std::vector<uint8_t> &out, char *err, size_t errCap);

  private:
    bool readAt(uint64_t off, void *dst, size_t n);
    bool readU16(uint64_t off, uint16_t &v);
    bool readU32(uint64_t off, uint32_t &v);
    bool readU64(uint64_t off, uint64_t &v);
    bool readCString(uint64_t &off, std::string &out);
    bool readDirentAt(uint64_t off, ZimEntry &out);
    bool readCluster(uint32_t cluster, std::vector<uint8_t> &out, char *err, size_t errCap);
    bool titlePointer(uint32_t titleIndex, uint32_t &entryIndex);

    FILE *_fp = nullptr;
    std::string _path;

    uint16_t _major = 0;
    uint16_t _minor = 0;
    uint32_t _entryCount = 0;
    uint32_t _clusterCount = 0;
    uint64_t _pathPtrPos = 0;
    uint64_t _titlePtrPos = 0;
    uint64_t _clusterPtrPos = 0;
    uint64_t _mimeListPos = 0;
    uint64_t _checksumPos = 0;
    uint32_t _mainPageIndex = 0xFFFFFFFFu;
    std::vector<std::string> _mimes;
};
