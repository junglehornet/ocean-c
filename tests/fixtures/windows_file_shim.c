// Run the Windows x64 assembly API boundary against native POSIX files in tests.
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <stdlib.h>
#include <errno.h>
#define API __attribute__((ms_abi, noinline))
static uint32_t last_error;
static int read_failure_after=-1, read_calls;
static int seek_failure_after=-1, seek_calls;
void FailReadAfter(int count) { read_failure_after=count; read_calls=0; }
void FailSeekAfter(int count) { seek_failure_after=count; seek_calls=0; }
static int descriptor(void *handle) { return (int)(intptr_t)handle - 1; }
API uint32_t GetLastError(void) { return last_error; }
API void *GetStdHandle(uint32_t selector) {
    int fd = (int)(0u - selector) - 10;
    if (fd < 0 || fd > 2) abort();
    return (void *)(intptr_t)(fd + 1);
}
API uint32_t GetFileType(void *handle) {
    struct stat info;
    if (fstat(descriptor(handle), &info)) return 0;
    return S_ISREG(info.st_mode) ? 1 : S_ISFIFO(info.st_mode) ? 3 : 2;
}
API int ReadFile(void *handle, void *data, uint32_t count, uint32_t *read_count, void *overlapped) {
    if (overlapped) abort();
    if (read_failure_after >= 0 && read_calls++ >= read_failure_after) {
        *read_count=0; last_error=5; return 0;
    }
    ssize_t n = read(descriptor(handle), data, count > 2 ? 2 : count);
    *read_count = n > 0 ? (uint32_t)n : 0;
    last_error = n == 0 ? 109 : 5;
    return n > 0;
}
API int WriteFile(void *handle, const void *data, uint32_t count, uint32_t *written, void *overlapped) {
    if (overlapped) abort();
    ssize_t n = write(descriptor(handle), data, count > 2 ? 2 : count);
    *written = n > 0 ? (uint32_t)n : 0;
    return n >= 0;
}
API int PeekNamedPipe(void *handle, void *data, uint32_t size, uint32_t *read_count,
                      uint32_t *available, uint32_t *left) {
    if (data || size || read_count || left) abort();
    int count = 0;
    if (ioctl(descriptor(handle), FIONREAD, &count)) { last_error=6; return 0; }
    *available = (uint32_t)count;
    struct pollfd entry = {descriptor(handle), POLLIN, 0};
    if (!count && poll(&entry, 1, 0) > 0 && (entry.revents & POLLHUP)) {
        last_error=109; return 0;
    }
    return 1;
}
API int GetConsoleMode(void *handle, uint32_t *mode) { (void)handle; *mode=2; return 1; }
API int GetNumberOfConsoleInputEvents(void *handle, uint32_t *count) { (void)handle; *count=0; return 1; }
API int PeekConsoleInputW(void *handle, void *records, uint32_t count, uint32_t *found) {
    (void)handle; (void)records; (void)count; *found=0; return 1;
}
API int CloseHandle(void *handle) { return close(descriptor(handle)) == 0; }
API void *CreateFileW(const uint16_t *path, uint32_t access, uint32_t sharing, void *security,
                      uint32_t creation, uint32_t attributes, void *template_file) {
    if (sharing != 7 || security || attributes != 128 || template_file) abort();
    char text[4096]; size_t length=0;
    for (size_t i=0; path[i]; i++) {
        uint32_t code=path[i];
        if (code >= 0xd800 && code <= 0xdbff) {
            uint32_t low=path[++i];
            if (low < 0xdc00 || low > 0xdfff) abort();
            code=0x10000 + ((code-0xd800)<<10) + low-0xdc00;
        } else if (code >= 0xdc00 && code <= 0xdfff) abort();
        if (length+4 >= sizeof(text)) abort();
        if (code < 0x80) text[length++]=(char)code;
        else if (code < 0x800) { text[length++]=(char)(0xc0|(code>>6)); text[length++]=(char)(0x80|(code&63)); }
        else if (code < 0x10000) { text[length++]=(char)(0xe0|(code>>12)); text[length++]=(char)(0x80|((code>>6)&63)); text[length++]=(char)(0x80|(code&63)); }
        else { text[length++]=(char)(0xf0|(code>>18)); text[length++]=(char)(0x80|((code>>12)&63)); text[length++]=(char)(0x80|((code>>6)&63)); text[length++]=(char)(0x80|(code&63)); }
    }
    text[length]=0;
    int readable=(access&0x80000000u)!=0, writable=(access&0x40000004u)!=0;
    int flags=readable ? (writable ? O_RDWR : O_RDONLY) : O_WRONLY;
    if (access&4u) flags|=O_APPEND;
    if (creation==2) flags|=O_CREAT|O_TRUNC;
    else if (creation==4) flags|=O_CREAT;
    else if (creation!=3) abort();
    int fd=open(text,flags|O_CLOEXEC,0666);
    return fd < 0 ? (void *)(intptr_t)-1 : (void *)(intptr_t)(fd+1);
}

API int SetFilePointerEx(void *handle, int64_t offset, int64_t *position, uint32_t origin) {
    if (seek_failure_after >= 0 && seek_calls++ >= seek_failure_after) return 0;
    off_t result=lseek(descriptor(handle),(off_t)offset,(int)origin);
    if (result < 0) return 0;
    *position=(int64_t)result;
    return 1;
}
API int GetFileSizeEx(void *handle, int64_t *size) {
    struct stat info;
    if (fstat(descriptor(handle),&info) || !S_ISREG(info.st_mode)) return 0;
    *size=(int64_t)info.st_size;
    return 1;
}
