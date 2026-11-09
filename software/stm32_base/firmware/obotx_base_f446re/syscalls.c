/* No filesystem or stdio transport on this board. In particular, printf must
 * never inject debug text into the JSON UART. Provide explicit failing hooks
 * instead of libnosys stubs whose linker warnings confuse CubeIDE's parser. */
#include <errno.h>
#include <sys/stat.h>

int _close(int file)
{
    (void)file;
    errno = EBADF;
    return -1;
}

int _fstat(int file, struct stat *status)
{
    (void)file;
    (void)status;
    errno = EBADF;
    return -1;
}

int _isatty(int file)
{
    (void)file;
    errno = ENOTTY;
    return 0;
}

int _lseek(int file, int offset, int direction)
{
    (void)file;
    (void)offset;
    (void)direction;
    errno = EBADF;
    return -1;
}

int _read(int file, char *buffer, int length)
{
    (void)file;
    (void)buffer;
    (void)length;
    errno = EBADF;
    return -1;
}

int _write(int file, char *buffer, int length)
{
    (void)file;
    (void)buffer;
    (void)length;
    errno = EBADF;
    return -1;
}

int _getpid(void)
{
    return 1; /* Single bare-metal execution context. */
}

int _kill(int pid, int signal)
{
    (void)pid;
    (void)signal;
    errno = EINVAL;
    return -1;
}
