#include "uv_fs.h"

#include <stdlib.h>
#include <string.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "assert.h"
#include "err.h"
#include "heap.h"
#include "uv_os.h"

int UvFsCheckDir(const char *dir, char *errmsg)
{
    struct uv_fs_s req;
    int rv;

    /* Make sure we have a directory we can write into. */
    rv = uv_fs_stat(NULL, &req, dir, NULL);
    if (rv != 0) {
        switch (rv) {
            case UV_ENOENT:
                ErrMsgPrintf((char *)errmsg, "directory '%s' does not exist",
                             dir);
                return RAFT_NOTFOUND;
            case UV_EACCES:
                ErrMsgPrintf((char *)errmsg, "can't access directory '%s'",
                             dir);
                return RAFT_UNAUTHORIZED;
            case UV_ENOTDIR:
                ErrMsgPrintf((char *)errmsg, "path '%s' is not a directory",
                             dir);
                return RAFT_INVALID;
        }
        ErrMsgPrintf((char *)errmsg, "can't stat '%s': %s", dir,
                     uv_strerror(rv));
        return RAFT_IOERR;
    }

    if (!(req.statbuf.st_mode & S_IFDIR)) {
        ErrMsgPrintf((char *)errmsg, "path '%s' is not a directory", dir);
        return RAFT_INVALID;
    }

    if (!(req.statbuf.st_mode & S_IWRITE)) {
        ErrMsgPrintf((char *)errmsg, "directory '%s' is not writable", dir);
        return RAFT_INVALID;
    }

    return 0;
}

int UvFsSyncDir(const char *dir, char *errmsg)
{
    uv_file fd;
    int rv;
    rv = UvOsOpen(dir, UV_FS_O_RDONLY | UV_FS_O_DIRECTORY, 0, &fd);
    if (rv != 0) {
        UvOsErrMsg(errmsg, "open directory", rv);
        return RAFT_IOERR;
    }
    rv = UvOsFsync(fd);
    UvOsClose(fd);
    if (rv != 0) {
        UvOsErrMsg(errmsg, "fsync directory", rv);
        return RAFT_IOERR;
    }
    return 0;
}

int UvFsFileExists(const char *dir,
                   const char *filename,
                   bool *exists,
                   char *errmsg)
{
    uv_stat_t sb;
    char path[UV__PATH_SZ];
    int rv;

    UvOsJoin(dir, filename, path);

    rv = UvOsStat(path, &sb);
    if (rv != 0) {
        if (rv == UV_ENOENT) {
            *exists = false;
            goto out;
        }
        UvOsErrMsg(errmsg, "stat", rv);
        return RAFT_IOERR;
    }

    *exists = true;

out:
    return 0;
}

/* Get the size of the given file. */
int UvFsFileSize(const char *dir,
                 const char *filename,
                 off_t *size,
                 char *errmsg)
{
    uv_stat_t sb;
    char path[UV__PATH_SZ];
    int rv;

    UvOsJoin(dir, filename, path);

    rv = UvOsStat(path, &sb);
    if (rv != 0) {
        UvOsErrMsg(errmsg, "stat", rv);
        return RAFT_IOERR;
    }
    *size = (off_t)sb.st_size;

    return 0;
}

int UvFsFileIsEmpty(const char *dir,
                    const char *filename,
                    bool *empty,
                    char *errmsg)
{
    off_t size;
    int rv;

    rv = UvFsFileSize(dir, filename, &size, errmsg);
    if (rv != 0) {
        return rv;
    }
    *empty = size == 0 ? true : false;
    return 0;
}

/* Open a file in a directory. */
static int uvFsOpenFile(const char *dir,
                        const char *filename,
                        int flags,
                        int mode,
                        uv_file *fd,
                        char *errmsg)
{
    char path[UV__PATH_SZ];
    int rv;
    UvOsJoin(dir, filename, path);
    rv = UvOsOpen(path, flags, mode, fd);
    if (rv != 0) {
        UvOsErrMsg(errmsg, "open", rv);
        return RAFT_IOERR;
    }
    return 0;
}

int UvFsOpenFileForReading(const char *dir,
                           const char *filename,
                           uv_file *fd,
                           char *errmsg)
{
    char path[UV__PATH_SZ];
    int flags = O_RDONLY;

    UvOsJoin(dir, filename, path);

    return uvFsOpenFile(dir, filename, flags, 0, fd, errmsg);
}

int UvFsAllocateFile(const char *dir,
                     const char *filename,
                     size_t size,
                     uv_file *fd,
                     char *errmsg)
{
    char path[UV__PATH_SZ];
    int flags = O_WRONLY | O_CREAT | O_EXCL; /* Common open flags */
    int rv = 0;

    UvOsJoin(dir, filename, path);

    rv = uvFsOpenFile(dir, filename, flags, S_IRUSR | S_IWUSR, fd, errmsg);
    if (rv != 0) {
        goto err;
    }

    /* Allocate the desired size. */
    rv = UvOsFallocate(*fd, 0, (off_t)size);
    if (rv != 0) {
        switch (rv) {
            case UV_ENOSPC:
                ErrMsgPrintf(errmsg, "not enough space to allocate %zu bytes",
                             size);
                rv = RAFT_NOSPACE;
                break;
            default:
                UvOsErrMsg(errmsg, "posix_allocate", rv);
                rv = RAFT_IOERR;
                break;
        }
        goto err_after_open;
    }

    return 0;

err_after_open:
    UvOsClose(*fd);
    UvOsUnlink(path);
err:
    assert(rv != 0);
    return rv;
}

static int uvFsWriteFile(const char *dir,
                         const char *filename,
                         int flags,
                         struct raft_buffer *bufs,
                         unsigned n_bufs,
                         char *errmsg)
{
    uv_file fd;
    int rv;
    size_t size;
    unsigned i;
    size = 0;
    for (i = 0; i < n_bufs; i++) {
        size += bufs[i].len;
    }
    rv = uvFsOpenFile(dir, filename, flags, S_IRUSR | S_IWUSR, &fd, errmsg);
    if (rv != 0) {
        goto err;
    }
    rv = UvOsWrite(fd, (const uv_buf_t *)bufs, n_bufs, 0);
    if (rv != (int)(size)) {
        if (rv < 0) {
            UvOsErrMsg(errmsg, "write", rv);
        } else {
            ErrMsgPrintf(errmsg, "short write: %d only bytes written", rv);
        }
        goto err_after_file_open;
    }
    rv = UvOsFsync(fd);
    if (rv != 0) {
        UvOsErrMsg(errmsg, "fsync", rv);
        goto err_after_file_open;
    }
    rv = UvOsClose(fd);
    if (rv != 0) {
        UvOsErrMsg(errmsg, "close", rv);
        goto err;
    }
    return 0;

err_after_file_open:
    UvOsClose(fd);
err:
    return rv;
}

int UvFsMakeFile(const char *dir,
                 const char *filename,
                 struct raft_buffer *bufs,
                 unsigned n_bufs,
                 char *errmsg)
{
    int flags = UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_EXCL;
    return uvFsWriteFile(dir, filename, flags, bufs, n_bufs, errmsg);
}

int UvFsMakeOrOverwriteFile(const char *dir,
                            const char *filename,
                            const struct raft_buffer *buf,
                            char *errmsg)
{
    char path[UV__PATH_SZ];
    int flags = UV_FS_O_WRONLY;
    int mode = 0;
    bool exists = true;
    uv_file fd;
    int rv;

    UvOsJoin(dir, filename, path);

open:
    rv = UvOsOpen(path, flags, mode, &fd);
    if (rv != 0) {
        if (rv == UV_ENOENT && !(flags & UV_FS_O_CREAT)) {
            exists = false;
            flags |= UV_FS_O_CREAT;
            mode = S_IRUSR | S_IWUSR;
            goto open;
        }
        goto err;
    }

    rv = UvOsWrite(fd, (const uv_buf_t *)buf, 1, 0);
    if (rv != (int)(buf->len)) {
        if (rv < 0) {
            UvOsErrMsg(errmsg, "write", rv);
        } else {
            ErrMsgPrintf(errmsg, "short write: %d only bytes written", rv);
        }
        goto err_after_file_open;
    }

    if (exists) {
        rv = UvOsFdatasync(fd);
        if (rv != 0) {
            UvOsErrMsg(errmsg, "fsync", rv);
            goto err_after_file_open;
        }
    } else {
        rv = UvOsFsync(fd);
        if (rv != 0) {
            UvOsErrMsg(errmsg, "fsync", rv);
            goto err_after_file_open;
        }
    }

    rv = UvOsClose(fd);
    if (rv != 0) {
        UvOsErrMsg(errmsg, "close", rv);
        goto err;
    }

    if (!exists) {
        rv = UvFsSyncDir(dir, errmsg);
        if (rv != 0) {
            goto err;
        }
    }

    return 0;

err_after_file_open:
    UvOsClose(fd);
err:
    return RAFT_IOERR;
}

int UvFsFileHasOnlyTrailingZeros(uv_file fd, bool *flag, char *errmsg)
{
    struct raft_buffer buf;
    off_t size;
    off_t offset;
    size_t i;
    int rv;

    /* Save the current offset. */
    offset = lseek(fd, 0, SEEK_CUR);

    /* Figure the size of the rest of the file. */
    size = lseek(fd, 0, SEEK_END);
    if (size == -1) {
        UvOsErrMsg(errmsg, "lseek", -errno);
        return RAFT_IOERR;
    }
    size -= offset;

    /* Reposition the file descriptor offset to the original offset. */
    offset = lseek(fd, offset, SEEK_SET);
    if (offset == -1) {
        UvOsErrMsg(errmsg, "lseek", -errno);
        return RAFT_IOERR;
    }

    buf.len = (size_t)size;
    buf.base = HeapMalloc(buf.len);
    if (buf.base == NULL) {
        ErrMsgOom(errmsg);
        return RAFT_NOMEM;
    }

    rv = UvFsReadInto(fd, &buf, errmsg);
    if (rv != 0) {
        return rv;
    }

    for (i = 0; i < (size_t)size; i++) {
        if (((char *)buf.base)[i] != 0) {
            *flag = false;
            goto done;
        }
    }

    *flag = true;

done:
    HeapFree(buf.base);

    return 0;
}

bool UvFsIsAtEof(uv_file fd)
{
    off_t offset;
    off_t size;
    offset = lseek(fd, 0, SEEK_CUR); /* Get the current offset */
    size = lseek(fd, 0, SEEK_END);   /* Get file size */
    lseek(fd, offset, SEEK_SET);     /* Restore current offset */
    return offset == size;           /* Compare current offset and size */
}

int UvFsReadInto(uv_file fd, struct raft_buffer *buf, char *errmsg)
{
    int rv;
    /* TODO: use uv_fs_read() */
    rv = (int)read(fd, buf->base, buf->len);
    if (rv == -1) {
        UvOsErrMsg(errmsg, "read", -errno);
        return RAFT_IOERR;
    }
    assert(rv >= 0);
    if ((size_t)rv < buf->len) {
        ErrMsgPrintf(errmsg, "short read: %d bytes instead of %zu", rv,
                     buf->len);
        return RAFT_IOERR;
    }
    return 0;
}

int UvFsReadFile(const char *dir,
                 const char *filename,
                 struct raft_buffer *buf,
                 char *errmsg)
{
    uv_stat_t sb;
    char path[UV__PATH_SZ];
    uv_file fd;
    int rv;

    UvOsJoin(dir, filename, path);

    rv = UvOsStat(path, &sb);
    if (rv != 0) {
        UvOsErrMsg(errmsg, "stat", rv);
        rv = RAFT_IOERR;
        goto err;
    }

    rv = uvFsOpenFile(dir, filename, O_RDONLY, 0, &fd, errmsg);
    if (rv != 0) {
        goto err;
    }

    buf->len = (size_t)sb.st_size;
    buf->base = HeapMalloc(buf->len);
    if (buf->base == NULL) {
        ErrMsgOom(errmsg);
        rv = RAFT_NOMEM;
        goto err_after_open;
    }

    rv = UvFsReadInto(fd, buf, errmsg);
    if (rv != 0) {
        goto err_after_buf_alloc;
    }

    UvOsClose(fd);

    return 0;

err_after_buf_alloc:
    HeapFree(buf->base);
err_after_open:
    UvOsClose(fd);
err:
    return rv;
}

int UvFsReadFileInto(const char *dir,
                     const char *filename,
                     struct raft_buffer *buf,
                     char *errmsg)
{
    char path[UV__PATH_SZ];
    uv_file fd;
    int rv;

    UvOsJoin(dir, filename, path);

    rv = uvFsOpenFile(dir, filename, O_RDONLY, 0, &fd, errmsg);
    if (rv != 0) {
        goto err;
    }

    rv = UvFsReadInto(fd, buf, errmsg);
    if (rv != 0) {
        goto err_after_open;
    }

    UvOsClose(fd);

    return 0;

err_after_open:
    UvOsClose(fd);
err:
    return rv;
}

int UvFsRemoveFile(const char *dir, const char *filename, char *errmsg)
{
    char path[UV__PATH_SZ];
    int rv;
    UvOsJoin(dir, filename, path);
    rv = UvOsUnlink(path);
    if (rv != 0) {
        UvOsErrMsg(errmsg, "unlink", rv);
        return RAFT_IOERR;
    }
    return 0;
}

int UvFsTruncateAndRenameFile(const char *dir,
                              size_t size,
                              const char *filename1,
                              const char *filename2,
                              char *errmsg)
{
    char path1[UV__PATH_SZ];
    char path2[UV__PATH_SZ];
    uv_file fd;
    int rv;

    UvOsJoin(dir, filename1, path1);
    UvOsJoin(dir, filename2, path2);

    /* Truncate and rename. */
    rv = UvOsOpen(path1, UV_FS_O_RDWR, 0, &fd);
    if (rv != 0) {
        UvOsErrMsg(errmsg, "open", rv);
        goto err;
    }
    rv = UvOsTruncate(fd, (off_t)size);
    if (rv != 0) {
        UvOsErrMsg(errmsg, "truncate", rv);
        goto err_after_open;
    }
    rv = UvOsFsync(fd);
    if (rv != 0) {
        UvOsErrMsg(errmsg, "fsync", rv);
        goto err_after_open;
    }
    UvOsClose(fd);

    rv = UvOsRename(path1, path2);
    if (rv != 0) {
        UvOsErrMsg(errmsg, "rename", rv);
        goto err;
    }

    return 0;

err_after_open:
    UvOsClose(fd);
err:
    return RAFT_IOERR;
}

/* Check if direct I/O is possible on the given fd. */
static int probeDirectIO(int fd, size_t *size, char *errmsg)
{
    struct statfs fs_info; /* To check the file system type. */
    void *buf;             /* Buffer to use for the probe write. */
    int rv;

    rv = UvOsSetDirectIo(fd);
    if (rv != 0) {
        if (rv != UV_EINVAL) {
            /* UNTESTED: the parameters are ok, so this should never happen. */
            UvOsErrMsg(errmsg, "fnctl", rv);
            return RAFT_IOERR;
        }
        rv = fstatfs(fd, &fs_info);
        if (rv == -1) {
            /* UNTESTED: in practice ENOMEM should be the only failure mode */
            UvOsErrMsg(errmsg, "fstatfs", -errno);
            return RAFT_IOERR;
        }
        switch (fs_info.f_type) {
            case 0x01021994: /* TMPFS_MAGIC */
            case 0x2fc12fc1: /* ZFS magic */
                *size = 0;
                return 0;
            default:
                /* UNTESTED: this is an unsupported file system. */
#if defined(__s390x__)
                ErrMsgPrintf(errmsg, "unsupported file system: %ux",
                             fs_info.f_type);
#else
                ErrMsgPrintf(errmsg, "unsupported file system: %zx",
                             fs_info.f_type);
#endif
                return RAFT_IOERR;
        }
    }

    /* Try to peform direct I/O, using various buffer size. */
    *size = 4096;
    while (*size >= 512) {
        buf = raft_aligned_alloc(*size, *size);
        if (buf == NULL) {
            ErrMsgOom(errmsg);
            return RAFT_NOMEM;
        }
        memset(buf, 0, *size);
        rv = (int)write(fd, buf, *size);
        raft_free(buf);
        if (rv > 0) {
            /* Since we fallocate'ed the file, we should never fail because of
             * lack of disk space, and all bytes should have been written. */
            assert(rv == (int)(*size));
            return 0;
        }
        assert(rv == -1);
        if (errno != EIO && errno != EOPNOTSUPP) {
            /* UNTESTED: this should basically fail only because of disk errors,
             * since we allocated the file with posix_fallocate. */

            /* FIXME: this is a workaround because shiftfs doesn't return EINVAL
             * in the fnctl call above, for example when the underlying fs is
             * ZFS. */
            if (errno == EINVAL && *size == 4096) {
                *size = 0;
                return 0;
            }

            UvOsErrMsg(errmsg, "write", -errno);
            return RAFT_IOERR;
        }
        *size = *size / 2;
    }

    *size = 0;
    return 0;
}

#if defined(RWF_NOWAIT)
/* Check if fully non-blocking async I/O is possible on the given fd. */
static int probeAsyncIO(int fd, size_t size, bool *ok, char *errmsg)
{
    void *buf;                  /* Buffer to use for the probe write */
    aio_context_t ctx = 0;      /* KAIO context handle */
    struct iocb iocb;           /* KAIO request object */
    struct iocb *iocbs = &iocb; /* Because the io_submit() API sucks */
    struct io_event event;      /* KAIO response object */
    int n_events;
    int rv;

    /* Setup the KAIO context handle */
    rv = UvOsIoSetup(1, &ctx);
    if (rv != 0) {
        UvOsErrMsg(errmsg, "io_setup", rv);
        /* UNTESTED: in practice this should fail only with ENOMEM */
        return RAFT_IOERR;
    }

    /* Allocate the write buffer */
    buf = raft_aligned_alloc(size, size);
    if (buf == NULL) {
        ErrMsgOom(errmsg);
        return RAFT_NOMEM;
    }
    memset(buf, 0, size);

    /* Prepare the KAIO request object */
    memset(&iocb, 0, sizeof iocb);
    iocb.aio_lio_opcode = IOCB_CMD_PWRITE;
    *((void **)(&iocb.aio_buf)) = buf;
    iocb.aio_nbytes = size;
    iocb.aio_offset = 0;
    iocb.aio_fildes = (uint32_t)fd;
    iocb.aio_reqprio = 0;
    iocb.aio_rw_flags |= RWF_NOWAIT | RWF_DSYNC;

    /* Submit the KAIO request */
    rv = UvOsIoSubmit(ctx, 1, &iocbs);
    if (rv != 0) {
        /* UNTESTED: in practice this should fail only with ENOMEM */
        raft_free(buf);
        UvOsIoDestroy(ctx);
        /* On ZFS 0.8 this is not properly supported yet. */
        if (errno == EOPNOTSUPP) {
            *ok = false;
            return 0;
        }
        UvOsErrMsg(errmsg, "io_submit", rv);
        return RAFT_IOERR;
    }

    /* Fetch the response: will block until done. */
    n_events = UvOsIoGetevents(ctx, 1, 1, &event, NULL);
    assert(n_events == 1);

    /* Release the write buffer. */
    raft_free(buf);

    /* Release the KAIO context handle. */
    rv = UvOsIoDestroy(ctx);
    if (rv != 0) {
        UvOsErrMsg(errmsg, "io_destroy", rv);
        return RAFT_IOERR;
    }

    if (event.res > 0) {
        assert(event.res == (int)size);
        *ok = true;
    } else {
        /* UNTESTED: this should basically fail only because of disk errors,
         * since we allocated the file with posix_fallocate and the block size
         * is supposed to be correct. */
        *ok = false;
    }

    return 0;
}
#endif /* RWF_NOWAIT */

#define UV__FS_PROBE_FILE ".probe"
#define UV__FS_PROBE_FILE_SIZE 4096

int UvFsProbeCapabilities(const char *dir,
                          size_t *direct,
                          bool *async,
                          char *errmsg)
{
    int fd; /* File descriptor of the probe file */
    int rv;
    char ignored[RAFT_ERRMSG_BUF_SIZE];

    /* Create a temporary probe file. */
    UvFsRemoveFile(dir, UV__FS_PROBE_FILE, ignored);
    rv = UvFsAllocateFile(dir, UV__FS_PROBE_FILE, UV__FS_PROBE_FILE_SIZE, &fd,
                          errmsg);
    if (rv != 0) {
        ErrMsgWrapf(errmsg, "create I/O capabilities probe file");
        goto err;
    }
    UvFsRemoveFile(dir, UV__FS_PROBE_FILE, ignored);

    /* Check if we can use direct I/O. */
    rv = probeDirectIO(fd, direct, errmsg);
    if (rv != 0) {
        goto err_after_file_open;
    }

#if !defined(RWF_NOWAIT)
    /* We can't have fully async I/O, since io_submit might potentially block.
     */
    *async = false;
#else
    /* If direct I/O is not possible, we can't perform fully asynchronous
     * I/O, because io_submit might potentially block. */
    if (*direct == 0) {
        *async = false;
        goto out;
    }
    rv = probeAsyncIO(fd, *direct, async, errmsg);
    if (rv != 0) {
        goto err_after_file_open;
    }
#endif /* RWF_NOWAIT */

#if defined(RWF_NOWAIT)
out:
#endif /* RWF_NOWAIT */
    close(fd);
    return 0;

err_after_file_open:
    close(fd);
err:
    return rv;
}
