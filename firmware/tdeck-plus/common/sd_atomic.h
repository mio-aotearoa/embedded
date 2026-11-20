// SPDX-License-Identifier: GPL-3.0-or-later
//
// Write to "<dest>.part", then rename into place. Shared by http_download and
// LocalSend receive so a partial transfer never looks like a finished file.
#pragma once

#include <SD.h>

#include <stddef.h>
#include <stdint.h>

// mkdir every parent of `path` (does not create the leaf file).
void sdEnsureParentDirs(const char *path);

class SdAtomicFile
{
  public:
    // Opens destPath+".part" for writing (creates parents). false → err set.
    bool begin(const char *destPath, char *err, size_t errLen);
    bool write(const uint8_t *data, size_t n, char *err, size_t errLen);
    // Close, remove any existing dest, rename .part → dest.
    bool commit(char *err, size_t errLen);
    // Close and delete the .part file.
    void abort(void);
    bool isOpen(void) const { return file_; }

  private:
    File file_;
    char dest_[192];
    char tmp_[200];
};
