#include "zim_reader.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#ifndef fseeko
#define fseeko fseek
#endif

namespace
{
constexpr uint32_t c_zimMagic = 0x044D495Au;
constexpr uint16_t c_direntRedirect = 0xFFFFu;
constexpr uint16_t c_direntLinkTarget = 0xFFFEu;
constexpr uint16_t c_direntDeleted = 0xFFFDu;
constexpr size_t c_maxMimes = 64;
constexpr size_t c_maxCString = 1024;
constexpr size_t c_maxClusterBytes = 16u * 1024u * 1024u;

uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

void setError(char *err, size_t cap, const char *fmt, ...)
{
    if (!err || !cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

char lower(char c) { return (char)tolower((unsigned char)c); }

} // namespace

ZimArchive::~ZimArchive() { close(); }

void ZimArchive::close(void)
{
    if (_fp) {
        fclose(_fp);
        _fp = nullptr;
    }
    _path.clear();
    _mimes.clear();
    _entryCount = _clusterCount = 0;
    _major = _minor = 0;
}

bool ZimArchive::readAt(uint64_t off, void *dst, size_t n)
{
    if (!_fp) {
        return false;
    }
    if (fseeko(_fp, (off_t)off, SEEK_SET) != 0) {
        return false;
    }
    return fread(dst, 1, n, _fp) == n;
}

bool ZimArchive::readU16(uint64_t off, uint16_t &v)
{
    uint8_t b[2];
    if (!readAt(off, b, sizeof(b))) {
        return false;
    }
    v = le16(b);
    return true;
}

bool ZimArchive::readU32(uint64_t off, uint32_t &v)
{
    uint8_t b[4];
    if (!readAt(off, b, sizeof(b))) {
        return false;
    }
    v = le32(b);
    return true;
}

bool ZimArchive::readU64(uint64_t off, uint64_t &v)
{
    uint8_t b[8];
    if (!readAt(off, b, sizeof(b))) {
        return false;
    }
    v = le64(b);
    return true;
}

bool ZimArchive::readCString(uint64_t &off, std::string &out)
{
    out.clear();
    for (size_t i = 0; i < c_maxCString; i++) {
        char c = 0;
        if (!readAt(off, &c, 1)) {
            return false;
        }
        off++;
        if (c == '\0') {
            return true;
        }
        out.push_back(c);
    }
    return false;
}

bool ZimArchive::open(const char *path, char *err, size_t errCap)
{
    close();
    _fp = fopen(path, "rb");
    if (!_fp) {
        setError(err, errCap, "cannot open %s", path);
        return false;
    }
    _path = path;

    uint8_t hdr[80];
    if (!readAt(0, hdr, sizeof(hdr))) {
        setError(err, errCap, "short header");
        close();
        return false;
    }
    if (le32(hdr) != c_zimMagic) {
        setError(err, errCap, "not a ZIM archive");
        close();
        return false;
    }
    _major = le16(hdr + 4);
    _minor = le16(hdr + 6);
    if (_major != 5 && _major != 6) {
        setError(err, errCap, "unsupported ZIM version %u", _major);
        close();
        return false;
    }
    _entryCount = le32(hdr + 24);
    _clusterCount = le32(hdr + 28);
    _pathPtrPos = le64(hdr + 32);
    _titlePtrPos = le64(hdr + 40);
    _clusterPtrPos = le64(hdr + 48);
    _mimeListPos = le64(hdr + 56);
    _mainPageIndex = le32(hdr + 64);
    _checksumPos = le64(hdr + 72);

    if (_entryCount == 0 || _clusterCount == 0 || _pathPtrPos == 0 || _clusterPtrPos == 0) {
        setError(err, errCap, "empty or truncated header");
        close();
        return false;
    }

    uint64_t off = _mimeListPos ? _mimeListPos : 80;
    _mimes.clear();
    for (size_t i = 0; i < c_maxMimes; i++) {
        std::string mime;
        if (!readCString(off, mime)) {
            setError(err, errCap, "bad MIME list");
            close();
            return false;
        }
        if (mime.empty()) {
            break;
        }
        _mimes.push_back(mime);
    }
    if (_mimes.empty()) {
        setError(err, errCap, "no MIME types");
        close();
        return false;
    }
    return true;
}

bool ZimArchive::readDirentAt(uint64_t off, ZimEntry &out)
{
    uint8_t fixed[16];
    if (!readAt(off, fixed, sizeof(fixed))) {
        return false;
    }
    out = ZimEntry{};
    out.mime = le16(fixed);
    const uint8_t paramLen = fixed[2];
    out.ns = (char)fixed[3];
    out.revision = le32(fixed + 4);

    uint64_t strOff = off + 16;
    if (out.mime == c_direntRedirect) {
        out.isRedirect = true;
        out.redirectIndex = le32(fixed + 8);
        strOff = off + 12;
    } else if (out.mime == c_direntLinkTarget || out.mime == c_direntDeleted) {
        return false; // deprecated dirents are skipped by the spec
    } else {
        out.cluster = le32(fixed + 8);
        out.blob = le32(fixed + 12);
        if (out.mime < _mimes.size()) {
            out.mimeType = _mimes[out.mime].c_str();
        }
    }

    if (!readCString(strOff, out.path)) {
        return false;
    }
    if (!readCString(strOff, out.title)) {
        return false;
    }
    if (out.title.empty()) {
        out.title = out.path;
    }
    (void)paramLen;
    return true;
}

bool ZimArchive::direntAt(uint32_t index, ZimEntry &out)
{
    if (index >= _entryCount) {
        return false;
    }
    uint64_t off = 0;
    if (!readU64(_pathPtrPos + (uint64_t)index * 8, off)) {
        return false;
    }
    return readDirentAt(off, out);
}

bool ZimArchive::resolve(const ZimEntry &entry, ZimEntry &out, int depth)
{
    if (!entry.isRedirect) {
        out = entry;
        return true;
    }
    if (depth >= 8) {
        return false;
    }
    ZimEntry next;
    if (!direntAt(entry.redirectIndex, next)) {
        return false;
    }
    return resolve(next, out, depth + 1);
}

bool ZimArchive::mainPage(ZimEntry &out)
{
    if (_mainPageIndex == 0xFFFFFFFFu || _mainPageIndex >= _entryCount) {
        return false;
    }
    ZimEntry entry;
    if (!direntAt(_mainPageIndex, entry)) {
        return false;
    }
    return resolve(entry, out);
}

bool ZimArchive::titlePointer(uint32_t titleIndex, uint32_t &entryIndex)
{
    return readU32(_titlePtrPos + (uint64_t)titleIndex * 4, entryIndex);
}

namespace
{
// Byte-order comparison of a title-list entry against a search key, which is
// the order the list itself uses: namespace first, then the title bytes.
int cmpTitleKey(char entryNs, const std::string &entryTitle, char ns, const std::string &key)
{
    if (entryNs != ns) {
        return (unsigned char)entryNs < (unsigned char)ns ? -1 : 1;
    }
    return entryTitle.compare(key);
}

bool startsWithCi(const std::string &title, const std::string &prefix)
{
    if (title.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); i++) {
        if (lower(title[i]) != lower(prefix[i])) {
            return false;
        }
    }
    return true;
}

void addVariant(std::vector<std::string> &keys, const std::string &key)
{
    if (key.empty()) {
        return;
    }
    for (const std::string &k : keys) {
        if (k == key) {
            return;
        }
    }
    keys.push_back(key);
}

// The user types on a T-Deck keyboard, so the case they type is not the case
// the archive stored. Each variant is searched with a byte-order lower bound,
// which is the only comparison the title list is actually sorted by.
std::vector<std::string> titleKeys(const char *prefix)
{
    const std::string typed(prefix);
    std::vector<std::string> keys;
    addVariant(keys, typed);

    std::string lowerKey = typed;
    for (char &c : lowerKey) c = lower(c);
    addVariant(keys, lowerKey);

    std::string upperKey = typed;
    for (char &c : upperKey) c = (char)toupper((unsigned char)c);
    addVariant(keys, upperKey);

    std::string capKey = lowerKey;
    if (!capKey.empty()) {
        capKey[0] = (char)toupper((unsigned char)capKey[0]);
    }
    addVariant(keys, capKey);
    return keys;
}
} // namespace

bool ZimArchive::titleEntryAt(uint32_t titleIndex, char &ns, std::string &title, uint32_t &entryIndex)
{
    if (!titlePointer(titleIndex, entryIndex)) {
        return false;
    }
    ZimEntry e;
    if (!direntAt(entryIndex, e)) {
        return false;
    }
    ns = e.ns;
    title = e.title;
    return true;
}

// [lo, hi) is the sub-range of the title list that holds namespace `ns`.
bool ZimArchive::titleNsBounds(char ns, uint32_t &lo, uint32_t &hi)
{
    const unsigned char want = (unsigned char)ns;
    uint32_t a = 0, b = _entryCount;
    while (a < b) {
        const uint32_t mid = a + (b - a) / 2;
        char mns;
        std::string title;
        uint32_t entryIndex = 0;
        if (!titleEntryAt(mid, mns, title, entryIndex)) {
            return false;
        }
        if ((unsigned char)mns < want) {
            a = mid + 1;
        } else {
            b = mid;
        }
    }
    lo = a;
    b = _entryCount;
    while (a < b) {
        const uint32_t mid = a + (b - a) / 2;
        char mns;
        std::string title;
        uint32_t entryIndex = 0;
        if (!titleEntryAt(mid, mns, title, entryIndex)) {
            return false;
        }
        if ((unsigned char)mns <= want) {
            a = mid + 1;
        } else {
            b = mid;
        }
    }
    hi = a;
    return lo < hi;
}

// First index in [lo, hi) whose (namespace, title) is not < (ns, key).
uint32_t ZimArchive::titleLowerBound(char ns, const std::string &key, uint32_t lo, uint32_t hi)
{
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        char mns;
        std::string title;
        uint32_t entryIndex = 0;
        if (!titleEntryAt(mid, mns, title, entryIndex)) {
            return hi;
        }
        if (cmpTitleKey(mns, title, ns, key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

std::vector<ZimEntry> ZimArchive::searchTitles(const char *prefix, size_t limit)
{
    std::vector<ZimEntry> results;
    if (!isOpen() || !prefix || !*prefix || _titlePtrPos == 0) {
        return results;
    }

    // Titles are grouped by namespace, and articles all live in one of them.
    // Take it from the main page when there is one, and always try the classic
    // article namespaces so archives without a main page still search.
    std::vector<char> namespaces;
    ZimEntry main;
    if (mainPage(main)) {
        namespaces.push_back(main.ns);
    }
    for (char ns : {'A', 'C'}) {
        bool known = false;
        for (char n : namespaces) {
            if (n == ns) {
                known = true;
            }
        }
        if (!known) {
            namespaces.push_back(ns);
        }
    }

    const std::vector<std::string> keys = titleKeys(prefix);
    std::vector<uint32_t> seen;
    for (char ns : namespaces) {
        uint32_t lo = 0, hi = 0;
        if (!titleNsBounds(ns, lo, hi)) {
            continue;
        }
        for (const std::string &key : keys) {
            for (uint32_t i = titleLowerBound(ns, key, lo, hi); i < hi; i++) {
                char mns;
                std::string title;
                uint32_t entryIndex = 0;
                if (!titleEntryAt(i, mns, title, entryIndex)) {
                    break;
                }
                // Entries are byte-sorted, so the first title that no longer
                // matches the key ends this run of matches.
                if (mns != ns || !startsWithCi(title, key)) {
                    break;
                }
                bool dup = false;
                for (uint32_t s : seen) {
                    if (s == entryIndex) {
                        dup = true;
                        break;
                    }
                }
                if (dup) {
                    continue;
                }
                seen.push_back(entryIndex);

                ZimEntry e;
                if (direntAt(entryIndex, e)) {
                    ZimEntry resolved;
                    if (resolve(e, resolved) && !resolved.isRedirect) {
                        results.push_back(resolved);
                    }
                }
                if (results.size() >= limit) {
                    return results;
                }
            }
        }
    }
    return results;
}

bool ZimArchive::findByTitle(const char *prefix, ZimEntry &out)
{
    auto results = searchTitles(prefix, 1);
    if (results.empty()) {
        return false;
    }
    return resolve(results[0], out);
}

bool ZimArchive::findByPath(const char *fullPath, ZimEntry &out)
{
    if (!fullPath || fullPath[0] == '\0' || fullPath[1] != '/')
    {
        return false;
    }
    const char ns = fullPath[0];
    const char *path = fullPath + 2;

    // The path pointer list is sorted by (namespace, path).
    uint32_t lo = 0, hi = _entryCount;
    while (lo < hi)
    {
        const uint32_t mid = lo + (hi - lo) / 2;
        ZimEntry e;
        if (!direntAt(mid, e))
        {
            return false;
        }
        int cmp;
        if (e.ns != ns)
        {
            cmp = (unsigned char)e.ns < (unsigned char)ns ? -1 : 1;
        }
        else
        {
            cmp = strcmp(e.path.c_str(), path);
        }
        if (cmp == 0)
        {
            out = e;
            return true;
        }
        if (cmp < 0)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return false;
}

bool ZimArchive::readCluster(uint32_t cluster, std::vector<uint8_t> &out, char *err, size_t errCap)
{
    if (cluster >= _clusterCount) {
        setError(err, errCap, "cluster %u out of range", cluster);
        return false;
    }
    uint64_t start = 0, next = 0;
    if (!readU64(_clusterPtrPos + (uint64_t)cluster * 8, start)) {
        setError(err, errCap, "cluster pointer unreadable");
        return false;
    }
    if (cluster + 1 < _clusterCount) {
        if (!readU64(_clusterPtrPos + (uint64_t)(cluster + 1) * 8, next)) {
            setError(err, errCap, "cluster pointer unreadable");
            return false;
        }
    } else {
        next = _checksumPos;
    }
    if (next <= start + 1) {
        setError(err, errCap, "empty cluster");
        return false;
    }
    size_t compressedLen = (size_t)(next - start - 1);
    if (compressedLen > c_maxClusterBytes) {
        setError(err, errCap, "cluster too large (%u bytes)", (unsigned)compressedLen);
        return false;
    }

    uint8_t info = 0;
    if (!readAt(start, &info, 1)) {
        setError(err, errCap, "cluster header unreadable");
        return false;
    }
    const uint8_t compression = info & 0x0F;

    if (compression == 0 || compression == 1) {
        out.resize(compressedLen);
        if (!readAt(start + 1, out.data(), compressedLen)) {
            setError(err, errCap, "cluster read failed");
            return false;
        }
        return true;
    }

    if (compression != 5) {
        setError(err, errCap,
                 compression == 4 ? "cluster is LZMA2/XZ (re-export the ZIM with zstd)"
                                  : "unknown cluster compression %u",
                 compression);
        return false;
    }

    std::vector<uint8_t> compressed(compressedLen);
    if (!readAt(start + 1, compressed.data(), compressedLen)) {
        setError(err, errCap, "cluster read failed");
        return false;
    }

    unsigned long long frameSize = ZSTD_getFrameContentSize(compressed.data(), compressedLen);
    if (frameSize == ZSTD_CONTENTSIZE_ERROR || frameSize == ZSTD_CONTENTSIZE_UNKNOWN) {
        setError(err, errCap, "zstd frame has no content size");
        return false;
    }
    if (frameSize > c_maxClusterBytes) {
        setError(err, errCap, "decompressed cluster too large");
        return false;
    }

    out.resize((size_t)frameSize);
    size_t rc = ZSTD_decompress(out.data(), out.size(), compressed.data(), compressedLen);
    if (ZSTD_isError(rc)) {
        setError(err, errCap, "zstd: %s", ZSTD_getErrorName(rc));
        return false;
    }
    out.resize(rc);
    return true;
}

bool ZimArchive::readBlob(const ZimEntry &entry, std::vector<uint8_t> &out, char *err, size_t errCap)
{
    out.clear();
    if (entry.isRedirect) {
        setError(err, errCap, "redirect entry has no blob");
        return false;
    }

    std::vector<uint8_t> cluster;
    if (!readCluster(entry.cluster, cluster, err, errCap)) {
        return false;
    }

    // The extended-offset flag (bit 4) lives in the cluster info byte at the
    // cluster's file offset; readCluster consumed only the byte, so re-read it.
    uint64_t start = 0;
    bool ext = false;
    if (readU64(_clusterPtrPos + (uint64_t)entry.cluster * 8, start)) {
        uint8_t info = 0;
        if (readAt(start, &info, 1)) {
            ext = (info & 0x10) != 0;
        }
    }
    if (ext && _major != 6) {
        setError(err, errCap, "extended cluster in a v5 archive");
        return false;
    }

    // Blob offsets are the first bytes of the decompressed cluster.
    const size_t offsetSize = ext ? 8 : 4;
    auto readOffset = [&](size_t index, uint64_t &value) -> bool {
        size_t pos = index * offsetSize;
        if (pos + offsetSize > cluster.size()) {
            return false;
        }
        value = ext ? le64(cluster.data() + pos) : le32(cluster.data() + pos);
        return true;
    };

    uint64_t first = 0;
    if (!readOffset(0, first) || first < offsetSize) {
        setError(err, errCap, "bad blob offset table");
        return false;
    }
    // offset_0 = (blobCount + 1) * offsetSize, so the last entry is the
    // sentinel end-of-data offset.
    const uint64_t offsetCount = first / offsetSize;
    const uint64_t blobCount = offsetCount ? offsetCount - 1 : 0;
    if (blobCount == 0 || entry.blob >= blobCount) {
        setError(err, errCap, "blob %u/%u out of range", entry.blob, (unsigned)blobCount);
        return false;
    }
    uint64_t begin = 0, end = 0;
    if (!readOffset(entry.blob, begin) || !readOffset((size_t)entry.blob + 1, end)) {
        setError(err, errCap, "blob offset unreadable");
        return false;
    }
    if (end < begin || end > cluster.size()) {
        setError(err, errCap, "blob bounds invalid");
        return false;
    }
    out.assign(cluster.begin() + (ptrdiff_t)begin, cluster.begin() + (ptrdiff_t)end);
    return true;
}
