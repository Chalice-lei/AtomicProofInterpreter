#ifndef TBC_UTIL_DEFER_H
#define TBC_UTIL_DEFER_H

#include <functional>
#include <iostream>

// RAII Defer：在析构时调用传入的可调用对象。
template <typename F>
class Defer
{
public:
    explicit Defer(F&& func) : func_(std::forward<F>(func))
    {}

    ~Defer()
    {
        func_();
    }

    Defer(const Defer&) = delete;
    Defer& operator=(const Defer&) = delete;
    Defer(Defer&&) = delete;
    Defer& operator=(Defer&&) = delete;

private:
    F func_;
};

template <typename F>
auto make_defer(F&& func) -> Defer<F>
{
    return Defer<F>(std::forward<F>(func));
}

// 用 __LINE__ 生成唯一变量名以避免命名冲突
#define DEFER_CONCAT_IMPL(x, y) x##y
#define DEFER_CONCAT(x, y) DEFER_CONCAT_IMPL(x, y)

// DEFER(callable): 接受 lambda/函数指针/函数对象
#define DEFER(callable)                                                        \
    auto DEFER_CONCAT(defer_, __LINE__) = make_defer(callable)

// DEFER_BLOCK(stmts...): 类似 Go 的 defer 语法
#define DEFER_BLOCK(...)                                                       \
    auto DEFER_CONCAT(defer_, __LINE__) = make_defer([&]() { __VA_ARGS__ })
#endif // TBC_UTIL_DEFER_H