// Originally by D. Richard Hipp, Public Domain
// https://www.sqlite.org/src/file/ext/misc/fileio.c

// Modified by Anton Zhiyanov, MIT License
// https://github.com/nalgeon/sqlean/

// Scalar `fileio` functions.

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#if !defined(_WIN32) && !defined(WIN32)
#include <dirent.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>

#else

#define _MSC_VER 1929
#define FILEIO_WIN32_DLL
#include <direct.h>
#include <io.h>
#include "../test_windirent.h"
#include "windows.h"
#define dirent DIRENT

#ifndef chmod
#define chmod _chmod
#endif

#ifndef stat
#define stat _stat
#endif

#define mkdir(path, mode) _mkdir(path)
#define lstat(path, buf) stat(path, buf)

#endif  // !defined(_WIN32) && !defined(WIN32)

#include <errno.h>
#include <time.h>

#include "extension.h"

#include "../sqlite3ext.h"
SQLITE_EXTENSION_INIT3

/*
** Set the result stored by context ctx to a blob containing the
** contents of file zName.  Or, leave the result unchanged (NULL)
** if the file does not exist or is unreadable.
**
** If the file exceeds the SQLite blob size limit, through an
** SQLITE_TOOBIG error.
**
** Throw an SQLITE_IOERR if there are difficulties pulling the file
** off of disk.
*/
static void readFileContents(sqlite3_context* ctx, const char* zName) {
    FILE* in;
    sqlite3_int64 nIn;
    void* pBuf;
    sqlite3* db;
    int mxBlob;

    in = fopen(zName, "rb");
    if (in == 0) {
        /* File does not exist or is unreadable. Leave the result set to NULL. */
        return;
    }
    fseek(in, 0, SEEK_END);
    nIn = ftell(in);
    rewind(in);
    db = sqlite3_context_db_handle(ctx);
    mxBlob = sqlite3_limit(db, SQLITE_LIMIT_LENGTH, -1);
    if (nIn > mxBlob) {
        sqlite3_result_error_code(ctx, SQLITE_TOOBIG);
        fclose(in);
        return;
    }
    pBuf = sqlite3_malloc64(nIn ? nIn : 1);
    if (pBuf == 0) {
        sqlite3_result_error_nomem(ctx);
        fclose(in);
        return;
    }
    if (nIn == (sqlite3_int64)fread(pBuf, 1, (size_t)nIn, in)) {
        sqlite3_result_blob64(ctx, pBuf, nIn, sqlite3_free);
    } else {
        sqlite3_result_error_code(ctx, SQLITE_IOERR);
        sqlite3_free(pBuf);
    }
    fclose(in);
}

/*
** Implementation of the "readfile(X)" SQL function.  The entire content
** of the file named X is read and returned as a BLOB.  NULL is returned
** if the file does not exist or is unreadable.
*/
static void sqlite3_readfile(sqlite3_context* context, int argc, sqlite3_value** argv) {
    const char* zName;
    (void)(argc); /* Unused parameter */
    zName = (const char*)sqlite3_value_text(argv[0]);
    if (zName == 0)
        return;
    readFileContents(context, zName);
}

/*
** Set the error message contained in context ctx to the results of
** vprintf(zFmt, ...).
*/
static void ctxErrorMsg(sqlite3_context* ctx, const char* zFmt, ...) {
    char* zMsg = 0;
    va_list ap;
    va_start(ap, zFmt);
    zMsg = sqlite3_vmprintf(zFmt, ap);
    sqlite3_result_error(ctx, zMsg, -1);
    sqlite3_free(zMsg);
    va_end(ap);
}

#if defined(_WIN32)
/*
** This function is designed to convert a Win32 FILETIME structure into the
** number of seconds since the Unix Epoch (1970-01-01 00:00:00 UTC).
*/
static sqlite3_uint64 fileTimeToUnixTime(LPFILETIME pFileTime) {
    SYSTEMTIME epochSystemTime;
    ULARGE_INTEGER epochIntervals;
    FILETIME epochFileTime;
    ULARGE_INTEGER fileIntervals;

    memset(&epochSystemTime, 0, sizeof(SYSTEMTIME));
    epochSystemTime.wYear = 1970;
    epochSystemTime.wMonth = 1;
    epochSystemTime.wDay = 1;
    SystemTimeToFileTime(&epochSystemTime, &epochFileTime);
    epochIntervals.LowPart = epochFileTime.dwLowDateTime;
    epochIntervals.HighPart = epochFileTime.dwHighDateTime;

    fileIntervals.LowPart = pFileTime->dwLowDateTime;
    fileIntervals.HighPart = pFileTime->dwHighDateTime;

    return (fileIntervals.QuadPart - epochIntervals.QuadPart) / 10000000;
}

/*
** This function attempts to normalize the time values found in the stat()
** buffer to UTC.  This is necessary on Win32, where the runtime library
** appears to return these values as local times.
*/
static void statTimesToUtc(const char* zPath, struct stat* pStatBuf) {
    HANDLE hFindFile;
    WIN32_FIND_DATAW fd;
    LPWSTR zUnicodeName;
    extern LPWSTR sqlite3_win32_utf8_to_unicode(const char*);
    zUnicodeName = sqlite3_win32_utf8_to_unicode(zPath);
    if (zUnicodeName) {
        memset(&fd, 0, sizeof(WIN32_FIND_DATAW));
        hFindFile = FindFirstFileW(zUnicodeName, &fd);
        if (hFindFile != NULL) {
            pStatBuf->st_ctime = (time_t)fileTimeToUnixTime(&fd.ftCreationTime);
            pStatBuf->st_atime = (time_t)fileTimeToUnixTime(&fd.ftLastAccessTime);
            pStatBuf->st_mtime = (time_t)fileTimeToUnixTime(&fd.ftLastWriteTime);
            FindClose(hFindFile);
        }
        sqlite3_free(zUnicodeName);
    }
}
#endif

/*
** This function is used in place of stat().  On Windows, special handling
** is required in order for the included time to be returned as UTC.  On all
** other systems, this function simply calls stat().
*/
static int fileStat(const char* zPath, struct stat* pStatBuf) {
#if defined(_WIN32)
    int rc = stat(zPath, pStatBuf);
    if (rc == 0)
        statTimesToUtc(zPath, pStatBuf);
    return rc;
#else
    return stat(zPath, pStatBuf);
#endif
}

/*
** Argument zFile is the name of a file that will be created and/or written
** by SQL function writefile(). This function ensures that the directory
** zFile will be written to exists, creating it if required. The permissions
** for any path components created by this function are set in accordance
** with the current umask.
**
** If an OOM condition is encountered, SQLITE_NOMEM is returned. Otherwise,
** SQLITE_OK is returned if the directory is successfully created, or
** SQLITE_ERROR otherwise.
*/
static int makeParentDirectory(const char* zFile) {
    char* zCopy = sqlite3_mprintf("%s", zFile);
    int rc = SQLITE_OK;

    if (zCopy == 0) {
        rc = SQLITE_NOMEM;
    } else {
        int nCopy = (int)strlen(zCopy);
        int i = 1;

        while (rc == SQLITE_OK) {
            struct stat sStat;
            int rc2;

            for (; zCopy[i] != '/' && i < nCopy; i++)
                ;
            if (i == nCopy)
                break;
            zCopy[i] = '\0';

            rc2 = fileStat(zCopy, &sStat);
            if (rc2 != 0) {
                if (mkdir(zCopy, 0777))
                    rc = SQLITE_ERROR;
            } else {
                if (!S_ISDIR(sStat.st_mode))
                    rc = SQLITE_ERROR;
            }
            zCopy[i] = '/';
            i++;
        }

        sqlite3_free(zCopy);
    }

    return rc;
}

/*
 * Creates a directory named `path` with permission bits `mode`.
 */
static int makeDirectory(sqlite3_context* ctx, const char* path, mode_t mode) {
    int res = mkdir(path, mode);
    if (res != 0) {
        /* The mkdir() call to create the directory failed. This might not
        ** be an error though - if there is already a directory at the same
        ** path and either the permissions already match or can be changed
        ** to do so using chmod(), it is not an error.  */
        struct stat sStat;
        if (errno != EEXIST || 0 != fileStat(path, &sStat) || !S_ISDIR(sStat.st_mode) ||
            ((sStat.st_mode & 0777) != (mode & 0777) && 0 != chmod(path, mode & 0777))) {
            return 1;
        }
    }
    return 0;
}

/*
 * Creates a symbolic link named `dst`, pointing to `src`.
 */
static int createSymlink(sqlite3_context* ctx, const char* src, const char* dst) {
#if defined(_WIN32) || defined(WIN32)
    return 0;
#else
    int res = symlink(src, dst) < 0;
    if (res < 0) {
        return 1;
    }
    return 0;
#endif
}

/*
 * Writes blob `pData` to a file specified by `zFile`,
 * with permission bits `mode` and modification time `mtime` (-1 to not set).
 * Returns the number of written bytes.
 */
static int writeFile(sqlite3_context* pCtx,
                     const char* zFile,
                     sqlite3_value* pData,
                     mode_t mode,
                     sqlite3_int64 mtime) {
    sqlite3_int64 nWrite = 0;
    const char* z;
    int rc = 0;
    FILE* out = fopen(zFile, "wb");
    if (out == 0)
        return 1;
    z = (const char*)sqlite3_value_blob(pData);
    if (z) {
        sqlite3_int64 n = fwrite(z, 1, sqlite3_value_bytes(pData), out);
        nWrite = sqlite3_value_bytes(pData);
        if (nWrite != n) {
            rc = 1;
        }
    }
    fclose(out);
    if (rc == 0 && mode && chmod(zFile, mode)) {
        rc = 1;
    }
    if (rc)
        return 2;
    sqlite3_result_int64(pCtx, nWrite);

    if (mtime >= 0) {
#if defined(_WIN32)
#if !SQLITE_OS_WINRT
        /* Windows */
        FILETIME lastAccess;
        FILETIME lastWrite;
        SYSTEMTIME currentTime;
        LONGLONG intervals;
        HANDLE hFile;
        LPWSTR zUnicodeName;
        extern LPWSTR sqlite3_win32_utf8_to_unicode(const char*);

        GetSystemTime(&currentTime);
        SystemTimeToFileTime(&currentTime, &lastAccess);
        intervals = Int32x32To64(mtime, 10000000) + 116444736000000000;
        lastWrite.dwLowDateTime = (DWORD)intervals;
        lastWrite.dwHighDateTime = intervals >> 32;
        zUnicodeName = sqlite3_win32_utf8_to_unicode(zFile);
        if (zUnicodeName == 0) {
            return 1;
        }
        hFile = CreateFileW(zUnicodeName, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS, NULL);
        sqlite3_free(zUnicodeName);
        if (hFile != INVALID_HANDLE_VALUE) {
            BOOL bResult = SetFileTime(hFile, NULL, &lastAccess, &lastWrite);
            CloseHandle(hFile);
            return !bResult;
        } else {
            return 1;
        }
#endif
#elif defined(AT_FDCWD) && 0 /* utimensat() is not universally available */
        /* Recent unix */
        struct timespec times[2];
        times[0].tv_nsec = times[1].tv_nsec = 0;
        times[0].tv_sec = time(0);
        times[1].tv_sec = mtime;
        if (utimensat(AT_FDCWD, zFile, times, AT_SYMLINK_NOFOLLOW)) {
            return 1;
        }
#else
        /* Legacy unix */
        struct timeval times[2];
        times[0].tv_usec = times[1].tv_usec = 0;
        times[0].tv_sec = time(0);
        times[1].tv_sec = mtime;
        if (utimes(zFile, times)) {
            return 1;
        }
#endif
    }

    return 0;
}

// Writes data to a file.
// writefile(path, data[, perm[, mtime]])
static void sqlite3_writefile(sqlite3_context* context, int argc, sqlite3_value** argv) {
    sqlite3_int64 mtime = -1;

    if (argc < 2 || argc > 4) {
        sqlite3_result_error(context, "wrong number of arguments to function writefile()", -1);
        return;
    }

    const char* zFile = (const char*)sqlite3_value_text(argv[0]);
    if (zFile == 0) {
        return;
    }

    mode_t perm = 0666;
    if (argc >= 3) {
        perm = (mode_t)sqlite3_value_int(argv[2]);
    }

    if (argc == 4) {
        mtime = sqlite3_value_int64(argv[3]);
    }

    int res = writeFile(context, zFile, argv[1], perm, mtime);
    if (res == 1 && errno == ENOENT) {
        if (makeParentDirectory(zFile) == SQLITE_OK) {
            res = writeFile(context, zFile, argv[1], perm, mtime);
        }
    }

    if (argc > 2 && res != 0) {
        ctxErrorMsg(context, "failed to write file: %s", zFile);
    }
}

// Creates a symlink.
// symlink(src, dst)
static void sqlite3_symlink(sqlite3_context* context, int argc, sqlite3_value** argv) {
    if (argc != 2) {
        sqlite3_result_error(context, "wrong number of arguments to function symlink()", -1);
        return;
    }

    const char* src = (const char*)sqlite3_value_text(argv[0]);
    if (src == 0) {
        return;
    }
    const char* dst = (const char*)sqlite3_value_text(argv[1]);

    int res = createSymlink(context, src, dst);
    if (res != 0) {
        ctxErrorMsg(context, "failed to create symlink to: %s", src);
    }
}

// Creates a directory.
// mkdir(path, perm)
static void sqlite3_mkdir(sqlite3_context* context, int argc, sqlite3_value** argv) {
    if (argc != 1 && argc != 2) {
        sqlite3_result_error(context, "wrong number of arguments to function mkdir()", -1);
        return;
    }

    const char* path = (const char*)sqlite3_value_text(argv[0]);
    if (path == 0) {
        return;
    }

    mode_t perm = 0777;
    if (argc == 2) {
        perm = (mode_t)sqlite3_value_int(argv[1]);
    }

    int res = makeDirectory(context, path, perm);

    if (res != 0) {
        ctxErrorMsg(context, "failed to create directory: %s", path);
    }
}

// Given a numberic st_mode from stat(), convert it into a human-readable
// text string in the style of "ls -l".
// lsmode(mode)
static void sqlite3_lsmode(sqlite3_context* context, int argc, sqlite3_value** argv) {
    int i;
    int iMode = sqlite3_value_int(argv[0]);
    char z[16];
    (void)argc;
    if (S_ISLNK(iMode)) {
        z[0] = 'l';
    } else if (S_ISREG(iMode)) {
        z[0] = '-';
    } else if (S_ISDIR(iMode)) {
        z[0] = 'd';
    } else {
        z[0] = '?';
    }
    for (i = 0; i < 3; i++) {
        int m = (iMode >> ((2 - i) * 3));
        char* a = &z[1 + i * 3];
        a[0] = (m & 0x4) ? 'r' : '-';
        a[1] = (m & 0x2) ? 'w' : '-';
        a[2] = (m & 0x1) ? 'x' : '-';
    }
    z[10] = '\0';
    sqlite3_result_text(context, z, -1, SQLITE_TRANSIENT);
}

#if defined(FILEIO_WIN32_DLL) && (defined(_WIN32) || defined(WIN32))
/* To allow a standalone DLL, make test_windirent.c use the same
 * redefined SQLite API calls as the above extension code does.
 * Just pull in this .c to accomplish this. As a beneficial side
 * effect, this extension becomes a single translation unit. */

/*
** Implementation of the POSIX getenv() function using the Win32 API.
** This function is not thread-safe.
*/
const char* windirent_getenv(const char* name) {
    static char value[32768];                    /* Maximum length, per MSDN */
    DWORD dwSize = sizeof(value) / sizeof(char); /* Size in chars */
    DWORD dwRet;                                 /* Value returned by GetEnvironmentVariableA() */

    memset(value, 0, sizeof(value));
    dwRet = GetEnvironmentVariableA(name, value, dwSize);
    if (dwRet == 0 || dwRet > dwSize) {
        /*
        ** The function call to GetEnvironmentVariableA() failed -OR-
        ** the buffer is not large enough.  Either way, return NULL.
        */
        return 0;
    } else {
        /*
        ** The function call to GetEnvironmentVariableA() succeeded
        ** -AND- the buffer contains the entire value.
        */
        return value;
    }
}

/*
** Implementation of the POSIX opendir() function using the MSVCRT.
*/
LPDIR opendir(const char* dirname) {
    struct _finddata_t data;
    LPDIR dirp = (LPDIR)sqlite3_malloc(sizeof(DIR));
    SIZE_T namesize = sizeof(data.name) / sizeof(data.name[0]);

    if (dirp == NULL)
        return NULL;
    memset(dirp, 0, sizeof(DIR));

    /* TODO: Remove this if Unix-style root paths are not used. */
    if (sqlite3_stricmp(dirname, "/") == 0) {
        dirname = windirent_getenv("SystemDrive");
    }

    memset(&data, 0, sizeof(struct _finddata_t));
    _snprintf(data.name, namesize, "%s\\*", dirname);
    dirp->d_handle = _findfirst(data.name, &data);

    if (dirp->d_handle == BAD_INTPTR_T) {
        closedir(dirp);
        return NULL;
    }

    /* TODO: Remove this block to allow hidden and/or system files. */
    if (is_filtered(data)) {
    next:

        memset(&data, 0, sizeof(struct _finddata_t));
        if (_findnext(dirp->d_handle, &data) == -1) {
            closedir(dirp);
            return NULL;
        }

        /* TODO: Remove this block to allow hidden and/or system files. */
        if (is_filtered(data))
            goto next;
    }

    dirp->d_first.d_attributes = data.attrib;
    strncpy(dirp->d_first.d_name, data.name, NAME_MAX);
    dirp->d_first.d_name[NAME_MAX] = '\0';

    return dirp;
}

/*
** Implementation of the POSIX readdir() function using the MSVCRT.
*/
LPDIRENT readdir(LPDIR dirp) {
    struct _finddata_t data;

    if (dirp == NULL)
        return NULL;

    if (dirp->d_first.d_ino == 0) {
        dirp->d_first.d_ino++;
        dirp->d_next.d_ino++;

        return &dirp->d_first;
    }

next:

    memset(&data, 0, sizeof(struct _finddata_t));
    if (_findnext(dirp->d_handle, &data) == -1)
        return NULL;

    /* TODO: Remove this block to allow hidden and/or system files. */
    if (is_filtered(data))
        goto next;

    dirp->d_next.d_ino++;
    dirp->d_next.d_attributes = data.attrib;
    strncpy(dirp->d_next.d_name, data.name, NAME_MAX);
    dirp->d_next.d_name[NAME_MAX] = '\0';

    return &dirp->d_next;
}

/*
** Implementation of the POSIX readdir_r() function using the MSVCRT.
*/
INT readdir_r(LPDIR dirp, LPDIRENT entry, LPDIRENT* result) {
    struct _finddata_t data;

    if (dirp == NULL)
        return EBADF;

    if (dirp->d_first.d_ino == 0) {
        dirp->d_first.d_ino++;
        dirp->d_next.d_ino++;

        entry->d_ino = dirp->d_first.d_ino;
        entry->d_attributes = dirp->d_first.d_attributes;
        strncpy(entry->d_name, dirp->d_first.d_name, NAME_MAX);
        entry->d_name[NAME_MAX] = '\0';

        *result = entry;
        return 0;
    }

next:

    memset(&data, 0, sizeof(struct _finddata_t));
    if (_findnext(dirp->d_handle, &data) == -1) {
        *result = NULL;
        return ENOENT;
    }

    /* TODO: Remove this block to allow hidden and/or system files. */
    if (is_filtered(data))
        goto next;

    entry->d_ino = (ino_t)-1; /* not available */
    entry->d_attributes = data.attrib;
    strncpy(entry->d_name, data.name, NAME_MAX);
    entry->d_name[NAME_MAX] = '\0';

    *result = entry;
    return 0;
}

/*
** Implementation of the POSIX closedir() function using the MSVCRT.
*/
INT closedir(LPDIR dirp) {
    INT result = 0;

    if (dirp == NULL)
        return EINVAL;

    if (dirp->d_handle != NULL_INTPTR_T && dirp->d_handle != BAD_INTPTR_T) {
        result = _findclose(dirp->d_handle);
    }

    sqlite3_free(dirp);
    return result;
}
#endif

int fileioscalar_init(sqlite3* db) {
    static const int flags = SQLITE_UTF8 | SQLITE_DIRECTONLY;
    sqlite3_create_function(db, "lsmode", 1, SQLITE_UTF8, 0, sqlite3_lsmode, 0, 0);
    sqlite3_create_function(db, "mkdir", -1, flags, 0, sqlite3_mkdir, 0, 0);
    sqlite3_create_function(db, "readfile", 1, flags, 0, sqlite3_readfile, 0, 0);
    sqlite3_create_function(db, "symlink", 2, flags, 0, sqlite3_symlink, 0, 0);
    sqlite3_create_function(db, "writefile", -1, flags, 0, sqlite3_writefile, 0, 0);
    return SQLITE_OK;
}