#pragma once
#include <utility>

// ==========================================
// Static Delegate - 零开销回调抽象
// ==========================================
template<typename Signature>
class StaticDelegate;

// 特化：R(Args...)
template<typename R, typename... Args>
class StaticDelegate<R(Args...)> {
public:
    using FuncType = R(*)(void*, Args...);

    // 默认构造
    constexpr StaticDelegate() noexcept : func_(nullptr), obj_(nullptr) {}

    // 判空
    constexpr explicit operator bool() const noexcept { return func_ != nullptr; }
    constexpr bool is_null() const noexcept { return func_ == nullptr; }

    // 调用操作符
    R operator()(Args... args) const {
        return func_(obj_, static_cast<Args&&>(args)...);
    }

    // 绑定成员函数（核心模板魔术）
    template<typename T, R(T::*Method)(Args...)>
    static constexpr StaticDelegate bind(T* obj) noexcept {
        return StaticDelegate(&thunk<T, Method>, obj);
    }

    template<typename T, R(T::*Method)(Args...) const>
    static constexpr StaticDelegate bind(const T* obj) noexcept {
        return StaticDelegate(&thunk_const<T, Method>,
            const_cast<void*>(static_cast<const void*>(obj)));
    }

    // 绑定普通函数/静态函数
    static constexpr StaticDelegate bind(R(*func)(Args...)) noexcept {
        return StaticDelegate(&function_thunk, reinterpret_cast<void*>(func));
    }

    // 比较（用于查找/删除）
    constexpr bool operator==(const StaticDelegate& other) const noexcept {
        return func_ == other.func_ && obj_ == other.obj_;
    }
    constexpr bool operator!=(const StaticDelegate& other) const noexcept {
        return !(*this == other);
    }

private:
    FuncType func_;
    void* obj_;

    constexpr StaticDelegate(FuncType f, void* o) noexcept : func_(f), obj_(o) {}

    // 静态转发函数（编译期实例化）
    template<typename T, R(T::*Method)(Args...)>
    static R thunk(void* obj, Args... args) {
        return (static_cast<T*>(obj)->*Method)(static_cast<Args&&>(args)...);
    }

    template<typename T, R(T::*Method)(Args...) const>
    static R thunk_const(void* obj, Args... args) {
        return (static_cast<const T*>(obj)->*Method)(static_cast<Args&&>(args)...);
    }

    static R function_thunk(void* func, Args... args) {
        return (reinterpret_cast<R(*)(Args...)>(func))(static_cast<Args&&>(args)...);
    }
};

// ==========================================
// 辅助宏（简化语法）
// ==========================================
#define SD_BIND(obj, method) \
    StaticDelegate<std::decay_t<decltype(std::declval<decltype(obj)>()->method(std::declval<Args>()...))>(Args...)>::bind<decltype(obj), method>(obj)

#define SD_BIND_HANDLER(obj, method) \
    StaticDelegate<void(void*)>::bind<decltype(obj), method>(obj)

#define SD_BIND_TIMER(obj, method) \
    StaticDelegate<void()>::bind<decltype(obj), method>(obj)
