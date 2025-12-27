//
//  log.hpp
//
//
//  Created by zhoukai on 2025/02/27.
//

#ifndef __ezlog_hpp__
#define __ezlog_hpp__

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <mutex>
#include <memory>
#include <chrono>
#include <string>
#include <functional>

#ifndef EZLOG_MODULE_NAME
#define EZLOG_MODULE_NAME "[EZLOG]"
#endif

#ifdef _QNX_
#include <process.h>
#else
#include <sys/syscall.h>
#endif

namespace ezlog {

enum {
    EZLOG_ERR     = 1,
    EZLOG_WARN,
    EZLOG_INFO,
    EZLOG_DEBUG,
};

struct HOOKER;
extern std::shared_ptr<HOOKER> GHOOKER;
#define EZLOG_HOOKER_VAR() \
    std::shared_ptr<ezlog::HOOKER> ezlog::GHOOKER

struct HOOKER {
    int32_t level_ = EZLOG_DEBUG;
    std::function<void(int32_t, const char*, va_list)> func_ = nullptr;
    inline void operator()(int32_t l, const char *f, va_list ap) {
        if (func_) {
            func_(l, f, ap);
        } else if (l <= level_){
            vfprintf(stderr, f, ap);
        }
    }
};

static inline int32_t get_tid(void) {
    static thread_local int32_t tid = -1;
    if (tid == -1) {
#if defined(__APPLE__)
        tid = syscall(SYS_thread_selfid);
#elif defined(_QNX_)
        tid = gettid();
#else
        tid = syscall(SYS_gettid);
#endif
    }
    return tid;
}

static inline uint64_t tick(void) {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    return static_cast<uint64_t>(duration);
}

/*简单的将日志重定向到文件或设置日志等级*/
static inline int32_t initialize(const char *file, int32_t level) {
    if (!GHOOKER) { GHOOKER = std::make_shared<HOOKER>(); }
    int32_t dupfd = 0;
    if (level >= EZLOG_ERR && level <= EZLOG_DEBUG) { GHOOKER->level_ = level; }
    if (!file || !file[0]) { return 0; }
    dupfd = ::open(file, O_APPEND|O_CREAT|O_WRONLY, 0666);
    if (dupfd < 0) {
        fprintf(stderr, "open log file failed : %s , %s", file,
            ::strerror(errno));
        return -1;
    }
    ::dup2(dupfd, STDERR_FILENO);
    ::dup2(dupfd, STDOUT_FILENO);
    ::close(dupfd);
    return 0;
}

/*将日志重定向到回调，注意保证回调的有效性*/
static inline void redirect(std::function<void(int32_t,
        const char *, va_list)> && func)
{
    if (!GHOOKER) { GHOOKER = std::make_shared<HOOKER>(); }
    GHOOKER->func_ = std::move(func);
    return;
}

#ifndef __printflike
#define __printflike(fmtarg, firstvararg) \
    __attribute__((__format__ (__printf__, fmtarg, firstvararg)))
#endif

static inline int32_t __log(int32_t level, const char * fmt, ...)
        __printflike(2, 3);

static inline int32_t __log(int32_t level, const char * fmt, ...) {
    if (level > EZLOG_DEBUG || level < EZLOG_ERR) {return -EINVAL;}
    va_list ap;
    va_start(ap, fmt);
    if (!GHOOKER) { vfprintf(stderr, fmt, ap); }
    else { (*GHOOKER)(level, fmt, ap); }
    va_end(ap);
    return 0;
}

template<typename... Args> static inline
int32_t log(int32_t level, const std::string & fmt, Args&&... args) {
    std::string fmt_ = std::string(EZLOG_MODULE_NAME) + fmt + "\n";
    return __log(level, fmt_.c_str(), std::forward<Args>(args)...);
}

static inline const char * app_file(const char *file) {
    const char *__ptr, *__fname = file;
    return (__ptr = strrchr(__fname, '/')) ? (__ptr + 1) : __fname;
}

}

#define __APP_FUNC__ __PRETTY_FUNCTION__
#define __APP_FILE__ ezlog::app_file(__FILE__)

#ifdef EZLOG_VERBOSE

#define app_log_debug(f, ...) \
ezlog::__log(ezlog::EZLOG_DEBUG, EZLOG_MODULE_NAME \
    " [debug] %d, %llu, %s:%d[%s] - " f "\n", ezlog::get_tid(), \
    ezlog::tick(), __APP_FILE__, __LINE__, __APP_FUNC__, ##__VA_ARGS__)

#define app_log_info(f, ...)  \
ezlog::__log(ezlog::EZLOG_INFO,  EZLOG_MODULE_NAME \
    " [info]  %d, %llu, %s:%d[%s] - " f "\n", ezlog::get_tid(), \
    ezlog::tick(), __APP_FILE__, __LINE__, __APP_FUNC__, ##__VA_ARGS__)

#define app_log_warn(f, ...)  \
ezlog::__log(ezlog::EZLOG_WARN,  EZLOG_MODULE_NAME \
    " [warn]  %d, %llu, %s:%d[%s] - " f "\n", ezlog::get_tid(), \
    ezlog::tick(), __APP_FILE__, __LINE__, __APP_FUNC__, ##__VA_ARGS__)

#define app_log_error(f, ...) \
ezlog::__log(ezlog::EZLOG_ERR,   EZLOG_MODULE_NAME \
    " [error] %d, %llu, %s:%d[%s] - " f "\n", ezlog::get_tid(), \
    ezlog::tick(), __APP_FILE__, __LINE__, __APP_FUNC__, ##__VA_ARGS__)

#else

#define app_log_debug(f, ...) \
ezlog::__log(ezlog::EZLOG_DEBUG, EZLOG_MODULE_NAME \
    " [debug] %d, %lld, %s:%d - " f "\n", ezlog::get_tid(), \
    ezlog::tick(), __APP_FILE__, __LINE__, ##__VA_ARGS__)

#define app_log_info(f, ...)  \
ezlog::__log(ezlog::EZLOG_INFO,  EZLOG_MODULE_NAME \
    " [info]  %d, %lld, %s:%d - " f "\n", ezlog::get_tid(), \
    ezlog::tick(), __APP_FILE__, __LINE__, ##__VA_ARGS__)

#define app_log_warn(f, ...)  \
ezlog::__log(ezlog::EZLOG_WARN,  EZLOG_MODULE_NAME \
    " [warn]  %d, %llu, %s:%d - " f "\n", ezlog::get_tid(), \
    ezlog::tick(), __APP_FILE__, __LINE__, ##__VA_ARGS__)

#define app_log_error(f, ...) \
ezlog::__log(ezlog::EZLOG_ERR,   EZLOG_MODULE_NAME \
    " [error] %d, %llu, %s:%d - " f "\n", ezlog::get_tid(), \
    ezlog::tick(), __APP_FILE__, __LINE__, ##__VA_ARGS__)

#endif

#endif
