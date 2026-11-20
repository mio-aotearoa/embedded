// SPDX-License-Identifier: GPL-3.0-or-later
#include "sd_atomic.h"
#include "meshcomod_sd_io.h"

#include <stdio.h>
#include <string.h>

void sdEnsureParentDirs(const char *path)
{
    if (!path || !path[0]) return;
    char dir[192];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return;
    *slash = 0;
    for (char *p = dir + 1; *p; ++p)
    {
        if (*p != '/') continue;
        *p = 0;
        SD.mkdir(dir);
        *p = '/';
    }
    SD.mkdir(dir);
    meshcomodMarkSdIo();
}

bool SdAtomicFile::begin(const char *destPath, char *err, size_t errLen)
{
    if (err && errLen) err[0] = 0;
    if (!destPath || !destPath[0] || strlen(destPath) >= sizeof dest_ ||
        strlen(destPath) + 5 >= sizeof tmp_)
    {
        if (err && errLen) snprintf(err, errLen, "destination path is too long");
        return false;
    }
    snprintf(dest_, sizeof dest_, "%s", destPath);
    snprintf(tmp_, sizeof tmp_, "%s.part", dest_);
    sdEnsureParentDirs(tmp_);
    file_ = SD.open(tmp_, FILE_WRITE);
    if (!file_)
    {
        if (err && errLen) snprintf(err, errLen, "cannot write to the SD card");
        return false;
    }
    meshcomodMarkSdIo();
    return true;
}

bool SdAtomicFile::write(const uint8_t *data, size_t n, char *err, size_t errLen)
{
    if (!file_ || !data) return false;
    if (file_.write(data, n) != n)
    {
        if (err && errLen) snprintf(err, errLen, "SD write failed");
        return false;
    }
    meshcomodMarkSdIo();
    return true;
}

bool SdAtomicFile::commit(char *err, size_t errLen)
{
    if (!file_)
    {
        if (err && errLen) snprintf(err, errLen, "no open transfer");
        return false;
    }
    file_.close();
    meshcomodMarkSdIo();
    SD.remove(dest_);
    if (!SD.rename(tmp_, dest_))
    {
        SD.remove(tmp_);
        if (err && errLen) snprintf(err, errLen, "cannot rename the finished file");
        return false;
    }
    meshcomodMarkSdIo();
    return true;
}

void SdAtomicFile::abort(void)
{
    if (file_)
    {
        file_.close();
        meshcomodMarkSdIo();
    }
    if (tmp_[0]) SD.remove(tmp_);
    dest_[0] = tmp_[0] = 0;
}
