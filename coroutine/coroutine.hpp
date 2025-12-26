//
//  coroutine.hpp
//
//  Copyright © 2025 zhoukai. All rights reserved.
//

#ifndef coroutine_hpp
#define coroutine_hpp

#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <memory>
#include <functional>

#ifndef offsetof
# ifdef __compiler_offsetof
#  define offsetof(TYPE, MEMBER)    __compiler_offsetof(TYPE, MEMBER)
# else
#  define offsetof(TYPE, MEMBER)    ((size_t)&((TYPE *)0)->MEMBER)
# endif
#endif

#ifndef likely
# define likely(x)   __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
# define unlikely(x) __builtin_expect(!!(x), 0)
#endif

extern "C" {
void **
#if __amd64
__attribute__((__noinline__, __regparm__(3)))
#else
__attribute__((__noinline__))
#endif
CoroutineMakeCtx(void **, long, void *);

void*
#if __amd64
__attribute__((__noinline__, __regparm__(2)))
#else
__attribute__((__noinline__))
#endif
CoroutineSwapCtx(void ***prev, void ***next);

void
#if __amd64
__attribute__((__noinline__, __regparm__(2)))
#else
__attribute__((__noinline__))
#endif
CoroutineEntry(void *_p, void *_c);

}

namespace utils {

enum CO_STATUS {
    CO_SUSPEND = 0x80,
    CO_RUNNING = 0x81,
    CO_EXITED  = 0xff,
};

class coroutine {
public:
    /*某些系统上如果使用 printf 系列的函数，必须要大于 8192*/
    explicit coroutine(size_t ss = 8192) : taskCtx_(&coroutine::entry, ss) {
        taskCtx_.user = this;
    }
    virtual  ~coroutine(void) {}

    coroutine(coroutine && other) {
        this->operator=(std::move(other));
    }
    coroutine & operator = (coroutine && other) {
        if (other.status_ == CO_STATUS::CO_RUNNING) {
            throw std::invalid_argument("coroutine is running");
        }
        if (likely(this != &other)) {
            func_    = std::move(other.func_);
            taskCtx_ = std::move(other.taskCtx_);
            mainCtx_ = std::move(other.mainCtx_);
            status_  = other.status_;
            other.status_ = CO_STATUS::CO_EXITED;
        }
        return *this;
    }

    /*协程已经退出*/
    bool exited(void) const { return status_ == CO_EXITED; }
    /*构造返回后就处于挂起状态，第一操作是恢复*/
    CO_STATUS resume(void);
    /*让出*/
    void yield(void);
    /*绑定一个回调*/
    void bind(std::function<void(void)> && func) { func_ = std::move(func); }
protected:
    virtual void entry(void) { if (func_ != nullptr) func_(); }
public:
    class context {
        friend class coroutine;
    public:
        using  ctxFunc = void(*)(context*, context *);
        /*栈顶指针转换为上下文句柄*/
        static context* sp2ctx(void *p) {
            return reinterpret_cast<context*>(
                reinterpret_cast<uintptr_t>(p) - offsetof(context, top)
            );
        }
        /*上下文句柄转换为栈底指针*/
        static void **ctx2bp(context *p) {
            return reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(
                reinterpret_cast<uint8_t*>(p->addr) + p->size) & ~(0x0fUL)
            );
        }
        /*切换实现*/
        static context *switch_to(context *curr, context *next) {
            void *prev = CoroutineSwapCtx(&curr->top, &next->top);
            return sp2ctx(prev);
        }
    private:
        context(void) = default;
        context(ctxFunc fn, size_t ss);
        ~context(void) { if (addr) delete [] addr; }
        context(context && other) { this->operator=(std::move(other)); }
        context & operator = (context &&);
        size_t  size{0};
        void *  user{nullptr};
        void ** addr{nullptr}; /* 如果为空 则是一个主协程上下文*/
        void ** top {nullptr};
        context(const context &) = delete;
        context & operator = (const context &) = delete;
    };

#if __aarch64__
    constexpr const static uint8_t nr_reg = 16;
#elif __amd64
    constexpr const static uint8_t nr_reg = 14;
#else
 #error unsupported architecture
#endif
private:
    context taskCtx_;
    context mainCtx_;
    /*简单的记录状态，coroutine本身就不能并发（操作）*/
    CO_STATUS  status_{CO_STATUS::CO_SUSPEND};
    std::function<void(void)> func_{nullptr};
private:
    coroutine(const coroutine &) = delete;
    coroutine & operator=(const coroutine &) = delete;
    static void entry(context *, context *);
};

inline coroutine::context::context(ctxFunc fn, size_t ss)
    : size(ss<512?512:ss)
{
    addr = (void**)new char[size];
    top  = CoroutineMakeCtx(addr, ss, reinterpret_cast<void*>(CoroutineEntry));
    assert(&top[nr_reg] == ctx2bp(this));
    top[nr_reg - 1] = (void*)fn;
    context m;
    switch_to(&m, this);
}

inline coroutine::context & coroutine::context::operator = (context && other) {
    if (likely(this != &other)) {
        user = other.user; addr = other.addr; top = other.top; size = other.size;
        other.user = other.addr = other.top  = nullptr; other.size = 0;
    }
    return *this;
}

inline CO_STATUS coroutine::resume(void) {
    if (likely(status_ != CO_STATUS::CO_EXITED)) {
        status_ = CO_STATUS::CO_RUNNING;
        context::switch_to(&mainCtx_, &taskCtx_);
        assert(status_ != CO_STATUS::CO_RUNNING);
    }
    return status_;
}

inline void coroutine::yield(void) {
    assert(status_ == CO_STATUS::CO_RUNNING);
    status_ = CO_STATUS::CO_SUSPEND;
    context::switch_to(&taskCtx_, &mainCtx_);
    assert(status_ == CO_STATUS::CO_RUNNING);
}

inline void coroutine::entry(context *prev, context *next) {
    auto _this = static_cast<coroutine*>(prev->user);
    assert(_this);
    try {_this->entry(); } catch (...) { }
    _this->status_ = CO_STATUS::CO_EXITED;
    context::switch_to(&_this->taskCtx_, &_this->mainCtx_);
}

}

extern "C" {

#ifndef __APPLE__
#define COROUTINE_ASM_FUNC(name)  \
    ".global "#name"\n"      \
    #name":\n"
#else
#define COROUTINE_ASM_FUNC(name)  \
    ".global _"#name"\n"     \
    "_"#name":\n"
#endif

#define __COROUTINE_ENTRY_FUNC__ \
void CoroutineEntry(void *_p, void *_c) { \
    using namespace utils; \
    auto *curr = coroutine::context::sp2ctx(_c); \
    auto *prev = coroutine::context::sp2ctx(_p); \
    auto func = reinterpret_cast<coroutine::context::ctxFunc>( \
            coroutine::context::ctx2bp(curr)[-1] \
    ); \
    prev = coroutine::context::switch_to(curr, prev); \
    func(curr, prev); \
    ::write(STDERR_FILENO, "Abnormal exit.\n", 15); \
    ::abort(); \
}

/*
 * 初始化状态，各个寄存器，仅 BP、FPCR、NZCV 寄存器需要初始化为有效的值（复用了创建者的值）
void * CoroutineMakeCtx(void *stack, long size, void *func)
{
    uintptr_t x0 = (uintptr_t)stack + size;
    x0 &= ~0x0f;
    uintptr_t x8 = x0;
    x0 -= 128;
    uintptr_t *top = (uintptr_t*)x0;
    uintptr_t x9 = FPCR;
    *(top++) = x9; *(top++) = 0;
    *(top++) = 0; *(top++) = 0;
    *(top++) = 0; *(top++) = 0;
    *(top++) = 0; *(top++) = 0;
    *(top++) = 0; *(top++) = 0;
    *(top++) = 0; *(top++) = x8; // x8的值是栈帧指针 用于恢复 x29
    x9 = NZCV;
    *(top++) = x9; *(top++) = func; // func的值 函数返回地址 x30
    *(top++) = 0; *(top++) = 0;
    return (void*)x0;
}
 * 切换上下文， prev、next 存储上下文的 栈顶指针
void *CoroutineSwapCtx(uintptr_t *prev, uintptr_t *next)
{
    // 保存上下文
    uintptr_t x0 = prev;
    uintptr_t x8 = sp - 112; // 扩展栈指针，然后保存上下文（各种寄存器的值）
    *(uintptr_t*)x0 = x8;
    uintptr_t x9 = FPCR;
    uintptr_t *top = (uintptr_t*)x8;
    *(top++) = x9; *(top++) = x19;
    *(top++) = x20; *(top++) = x21;
    *(top++) = x22; *(top++) = x23;
    *(top++) = x24; *(top++) = x25;
    *(top++) = x26; *(top++) = x27;
    *(top++) = x28; *(top++) = x29; // 当前上下文的栈帧指针
    x9 = NZCV;
    *(top++) = x9; *(top++) = x30; // 当前上下文函数的返回地址
    // 恢复上下文
    uintptr_t x1 = next;
    x8 = *(uintptr_t*)x1; // 将栈顶存入 x8
    x9 = *(top++); x19 = *(top++);
    FPCR = x9;
    x20 = *(top++); x21 = *(top++);
    x22 = *(top++); x23 = *(top++);
    x24 = *(top++); x25 = *(top++);
    x26 = *(top++); x27 = *(top++);
    x28 = *(top++); x29 = *(top++);
    x9 = *(top++); x30 = *(top++);
    NZCV = x9;
    sp = top;
    *next = top;
    return (void*)x0;
}
 */
#if __aarch64__

#define COROUTINE_FUNC_IMPL() \
__COROUTINE_ENTRY_FUNC__ \
asm ( \
    ".text\n" \
    ".align 4,0x90;\n" \
    COROUTINE_ASM_FUNC(CoroutineMakeCtx) \
    "add x0, x0, x1\n" \
    "and x0, x0, #~0xF\n" \
    "mov x8, x0\n" \
    "sub x0, x0, #128\n" \
    "mrs x9, FPCR\n" \
    "stp x9, xzr, [x0, #0]\n" \
    "stp xzr, xzr, [x0, #16]\n" \
    "stp xzr, xzr, [x0, #32]\n" \
    "stp xzr, xzr, [x0, #48]\n" \
    "stp xzr, xzr, [x0, #64]\n" \
    "stp xzr, x8, [x0, #80]\n" \
    "mrs x9, NZCV\n" \
    "stp x9, x2, [x0, #96]\n" \
    "stp xzr, xzr, [x0, #112]\n" \
    "ret\n"\
); \
asm ( \
    ".text\n" \
    ".align 4,0x90;\n" \
    COROUTINE_ASM_FUNC(CoroutineSwapCtx) \
    "sub x8, sp, #112\n" \
    "str x8, [x0]\n" \
    "mrs x9, FPCR\n" \
    "stp x9, x19, [x8], #16\n" \
    "stp x20, x21, [x8], #16\n" \
    "stp x22, x23, [x8], #16\n" \
    "stp x24, x25, [x8], #16\n" \
    "stp x26, x27, [x8], #16\n" \
    "stp x28, x29, [x8], #16\n" \
    "mrs x9, NZCV\n" \
    "stp x9, x30, [x8], #16\n" \
    "ldr x8, [x1]\n" \
    "ldp x9, x19, [x8], #16\n" \
    "msr FPCR, x9\n" \
    "ldp x20, x21, [x8], #16\n" \
    "ldp x22, x23, [x8], #16\n" \
    "ldp x24, x25, [x8], #16\n" \
    "ldp x26, x27, [x8], #16\n" \
    "ldp x28, x29, [x8], #16\n" \
    "ldp x9, x30, [x8], #16\n" \
    "msr NZCV, x9\n" \
    "mov sp, x8\n" \
    "str x8, [x1]\n" \
    "ret\n"\
)

#elif __amd64

#define COROUTINE_FUNC_IMPL() \
__COROUTINE_ENTRY_FUNC__ \
asm ( \
    ".text\n" \
    ".align 16\n" \
    COROUTINE_ASM_FUNC(CoroutineMakeCtx) \
    "addq %rsi, %rdi\n" \
    "andq $-16, %rdi\n" \
    "movq %rdi, %rax\n" \
    "leaq -112(%rax), %rax\n" \
    "movq $0, 0x58(%rax)\n" \
    "movq %rdx, 0x50(%rax)\n" \
    "pushfq\n" \
    "popq 0x48(%rax)\n" \
    "movq %rdi, 0x40(%rax)\n" \
    "movq $0, 0x38(%rax)\n" \
    "movq $0, 0x30(%rax)\n" \
    "movq $0, 0x28(%rax)\n" \
    "movq $0, 0x20(%rax)\n" \
    "movq $0, 0x18(%rax)\n" \
    "movq $0, 0x10(%rax)\n" \
    "movq $0, 0x08(%rax)\n" \
    "stmxcsr (%rax)\n" \
    "fnstcw 0x4(%rax)\n" \
    "retq\n" \
); \
asm ( \
    ".text\n" \
    ".align 16\n" \
    COROUTINE_ASM_FUNC(CoroutineSwapCtx) \
    "pushfq\n" \
    "pushq %rbp\n" \
    "pushq %rbx\n" \
    "pushq %r10\n" \
    "pushq %r11\n" \
    "pushq %r12\n" \
    "pushq %r13\n" \
    "pushq %r14\n" \
    "pushq %r15\n" \
    "leaq -0x8(%rsp), %rsp\n" \
    "stmxcsr (%rsp)\n" \
    "fnstcw 0x4(%rsp)\n" \
    "movq %rsp, (%rdi)\n" \
    "movq (%rsi), %rsp\n" \
    "ldmxcsr (%rsp)\n" \
    "fldcw 0x4(%rsp)\n" \
    "leaq 0x8(%rsp), %rsp\n" \
    "popq %r15\n" \
    "popq %r14\n" \
    "popq %r13\n" \
    "popq %r12\n" \
    "popq %r11\n" \
    "popq %r10\n" \
    "popq %rbx\n" \
    "popq %rbp\n" \
    "popfq\n" \
    "popq %rcx\n" \
    "movq %rdi, %rax\n" \
    "jmpq *%rcx\n" \
)

#else
 #error unsupported architecture
#endif

}

#endif