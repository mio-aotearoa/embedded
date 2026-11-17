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

    // Case-insensitive prefix search over the title pointer list. The list is
    // ordered by <namespace><title> in byte order (ZIM spec), so the search
    // binary-searches that exact order and tries the common case variants of
    // the query; a case-insensitive comparison over the raw list would probe
    // outside the matching range. `limit` caps the returned matches.
    std::vector<ZimEntry> searchTitles(const char *prefix, size_t limit);
    bool findByTitle(const char *prefix, ZimEntry &out);

    // Look up a dirent by full "N/path" (namespace letter, slash, path), the
    // form ZIM HTML uses for images: "I/foo.jpg" or "../I/foo.jpg".
    bool findByPath(const char *fullPath, ZimEntry &out);

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
    bool titleEntryAt(uint32_t titleIndex, char &ns, std::string &title, uint32_t &entryIndex);
    bool titleNsBounds(char ns, uint32_t &lo, uint32_t &hi);
    uint32_t titleLowerBound(char ns, const std::string &key, uint32_t lo, uint32_t hi);

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
