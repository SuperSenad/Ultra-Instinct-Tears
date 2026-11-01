// Ultra Instinct Tears launcher: the little of the C++ standard library it uses (string, vector, optional, map, set, function,
// pair and a few algorithms), on plain C headers only. The launcher builds with -nostdinc++ -fno-exceptions
// -fno-rtti, so it needs no C++ standard library headers: the builder package's trimmed MSYS2 CLANG64 has only the
// few bc7enc uses. Out of memory aborts. Not a general-purpose library: just what launcher/*.cpp call.
#pragma once
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// Brace lists ({a, b, c}) need std::initializer_list, which the compiler fills in as {pointer, length}; it is the one
// name declared in std, as libc++ does it, since <initializer_list> is a C++ library header.
namespace std {
template <class E> class initializer_list {
    const E *b_;
    size_t n_;
    constexpr initializer_list(const E *b, size_t n) : b_(b), n_(n) {}

public:
    typedef E value_type;
    constexpr initializer_list() : b_(nullptr), n_(0) {}
    constexpr size_t size() const { return n_; }
    constexpr const E *begin() const { return b_; }
    constexpr const E *end() const { return b_ + n_; }
};
}  // namespace std

struct mini_place {};
inline void *operator new(size_t, mini_place, void *p) noexcept { return p; }
inline void operator delete(void *, mini_place, void *) noexcept {}

namespace mini {

template <class T> struct no_ref { typedef T type; };
template <class T> struct no_ref<T &> { typedef T type; };
template <class T> struct no_ref<T &&> { typedef T type; };
template <class T> struct no_cv { typedef T type; };
template <class T> struct no_cv<const T> { typedef T type; };
template <class T> struct no_cv<volatile T> { typedef T type; };
template <class T> struct no_cv<const volatile T> { typedef T type; };
template <class T> struct fn_ptr { typedef T type; };
template <class R, class... A> struct fn_ptr<R(A...)> { typedef R (*type)(A...); };
template <class T> struct decay { typedef typename fn_ptr<typename no_cv<typename no_ref<T>::type>::type>::type type; };
template <class A, class B> struct is_same { static constexpr bool value = false; };
template <class A> struct is_same<A, A> { static constexpr bool value = true; };
template <bool B, class T = void> struct enable_if {};
template <class T> struct enable_if<true, T> { typedef T type; };
// A forwarding constructor template must not take the class itself: for a non-const lvalue (`auto b = a;` with a
// non-const a) T(U&&) with U = X& is a better match than X(const X&), and the "copy" recurses forever (0013-0015's
// launcher crash on Apply: mini::function wrapped itself until the stack ran out).
template <class X, class U> struct not_self { static constexpr bool value = !is_same<typename decay<U>::type, X>::value; };
template <class T> constexpr typename no_ref<T>::type &&move(T &&t) noexcept {
    return static_cast<typename no_ref<T>::type &&>(t);
}
template <class T> constexpr T &&forward(typename no_ref<T>::type &t) noexcept { return static_cast<T &&>(t); }
template <class T> constexpr T &&forward(typename no_ref<T>::type &&t) noexcept { return static_cast<T &&>(t); }
template <class T> void swap(T &a, T &b) { T t = move(a); a = move(b); b = move(t); }

template <class T> constexpr const T &min(const T &a, const T &b) { return b < a ? b : a; }
template <class T> constexpr const T &max(const T &a, const T &b) { return a < b ? b : a; }
template <class T> constexpr const T &clamp(const T &v, const T &lo, const T &hi) { return v < lo ? lo : hi < v ? hi : v; }

inline float fabs(float v) { return ::fabsf(v); }
inline double fabs(double v) { return ::fabs(v); }
inline float floor(float v) { return ::floorf(v); }
inline double floor(double v) { return ::floor(v); }
inline float round(float v) { return ::roundf(v); }
inline double round(double v) { return ::round(v); }
inline float sin(float v) { return ::sinf(v); }
inline double sin(double v) { return ::sin(v); }
inline float cos(float v) { return ::cosf(v); }
inline double cos(double v) { return ::cos(v); }
inline float exp(float v) { return ::expf(v); }
inline double exp(double v) { return ::exp(v); }
inline float fmod(float a, float b) { return ::fmodf(a, b); }
inline double fmod(double a, double b) { return ::fmod(a, b); }

inline void *grab(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) abort();
    return p;
}

template <class It, class V> It find(It a, It b, const V &v) {
    for (; a != b; ++a)
        if (*a == v) return a;
    return b;
}
template <class It, class P> It remove_if(It a, It b, P pred) {
    It out = a;
    for (; a != b; ++a)
        if (!pred(*a)) {
            if (out != a) *out = move(*a);
            ++out;
        }
    return out;
}

// ---------------------------------------------------------------- vector

template <class T> class vector {
    T *p_ = nullptr;
    size_t n_ = 0, cap_ = 0;
    void grow(size_t want) {
        if (want <= cap_) return;
        size_t c = cap_ ? cap_ * 2 : 4;
        if (c < want) c = want;
        T *q = static_cast<T *>(grab(c * sizeof(T)));
        for (size_t i = 0; i < n_; i++) {
            new (mini_place(), q + i) T(move(p_[i]));
            p_[i].~T();
        }
        free(p_);
        p_ = q, cap_ = c;
    }

public:
    typedef T value_type;
    typedef T *iterator;
    typedef const T *const_iterator;
    vector() {}
    explicit vector(size_t n) { resize(n); }
    vector(size_t n, const T &v) { resize(n, v); }
    vector(const vector &o) { *this = o; }
    vector(std::initializer_list<T> l) { *this = l; }
    vector &operator=(std::initializer_list<T> l) {
        clear();
        grow(l.size());
        for (const T &v : l) new (mini_place(), p_ + n_++) T(v);
        return *this;
    }
    vector(vector &&o) noexcept : p_(o.p_), n_(o.n_), cap_(o.cap_) { o.p_ = nullptr, o.n_ = o.cap_ = 0; }
    ~vector() { clear(); free(p_); }
    vector &operator=(const vector &o) {
        if (this == &o) return *this;
        clear();
        grow(o.n_);
        for (size_t i = 0; i < o.n_; i++) new (mini_place(), p_ + i) T(o.p_[i]);
        n_ = o.n_;
        return *this;
    }
    vector &operator=(vector &&o) noexcept {
        if (this == &o) return *this;
        clear();
        free(p_);
        p_ = o.p_, n_ = o.n_, cap_ = o.cap_;
        o.p_ = nullptr, o.n_ = o.cap_ = 0;
        return *this;
    }
    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    T *data() { return p_; }
    const T *data() const { return p_; }
    T &operator[](size_t i) { return p_[i]; }
    const T &operator[](size_t i) const { return p_[i]; }
    T &front() { return p_[0]; }
    const T &front() const { return p_[0]; }
    T &back() { return p_[n_ - 1]; }
    const T &back() const { return p_[n_ - 1]; }
    T *begin() { return p_; }
    T *end() { return p_ + n_; }
    const T *begin() const { return p_; }
    const T *end() const { return p_ + n_; }
    void reserve(size_t n) { grow(n); }
    void clear() {
        for (size_t i = 0; i < n_; i++) p_[i].~T();
        n_ = 0;
    }
    void resize(size_t n) {
        grow(n);
        while (n_ < n) new (mini_place(), p_ + n_++) T();
        while (n_ > n) p_[--n_].~T();
    }
    void resize(size_t n, const T &v) {
        grow(n);
        while (n_ < n) new (mini_place(), p_ + n_++) T(v);
        while (n_ > n) p_[--n_].~T();
    }
    void push_back(const T &v) {
        if (n_ == cap_) {          // v may live in this vector
            T copy(v);
            grow(n_ + 1);
            new (mini_place(), p_ + n_++) T(move(copy));
            return;
        }
        new (mini_place(), p_ + n_++) T(v);
    }
    void push_back(T &&v) {
        if (n_ == cap_) {
            T keep(move(v));
            grow(n_ + 1);
            new (mini_place(), p_ + n_++) T(move(keep));
            return;
        }
        new (mini_place(), p_ + n_++) T(move(v));
    }
    template <class... A> T &emplace_back(A &&...a) {
        T v(forward<A>(a)...);
        push_back(move(v));
        return back();
    }
    void pop_back() { p_[--n_].~T(); }
    T *insert(const T *pos, T v) {
        size_t at = (size_t)(pos - p_);
        push_back(move(v));
        for (size_t i = n_ - 1; i > at; i--) mini::swap(p_[i], p_[i - 1]);
        return p_ + at;
    }
    T *erase(const T *a, const T *b) {
        size_t i = (size_t)(a - p_), j = (size_t)(b - p_), k = i;
        if (i == j) return p_ + i;
        for (; j < n_; j++, k++) p_[k] = move(p_[j]);
        while (n_ > k) p_[--n_].~T();
        return p_ + i;
    }
    T *erase(const T *a) { return erase(a, a + 1); }
    void swap(vector &o) noexcept {
        mini::swap(p_, o.p_);
        mini::swap(n_, o.n_);
        mini::swap(cap_, o.cap_);
    }
    bool operator==(const vector &o) const {
        if (n_ != o.n_) return false;
        for (size_t i = 0; i < n_; i++)
            if (!(p_[i] == o.p_[i])) return false;
        return true;
    }
    bool operator!=(const vector &o) const { return !(*this == o); }
};

// ---------------------------------------------------------------- string

template <class C> class basic_string {
    C *p_ = nullptr;           // always 0-terminated once allocated; c_str() of an empty string is ""
    size_t n_ = 0, cap_ = 0;
    static size_t len(const C *s) {
        size_t n = 0;
        while (s[n]) n++;
        return n;
    }
    void grow(size_t want) {
        if (want <= cap_) return;
        size_t c = cap_ ? cap_ * 2 : 15;
        if (c < want) c = want;
        C *q = static_cast<C *>(grab((c + 1) * sizeof(C)));
        if (n_) memcpy(q, p_, n_ * sizeof(C));
        q[n_] = 0;
        free(p_);
        p_ = q, cap_ = c;
    }

public:
    static constexpr size_t npos = (size_t)-1;
    typedef C value_type;
    typedef C *iterator;
    typedef const C *const_iterator;
    basic_string() {}
    basic_string(const C *s) { assign(s, len(s)); }
    basic_string(const C *s, size_t n) { assign(s, n); }
    basic_string(const C *a, const C *b) { assign(a, (size_t)(b - a)); }
    basic_string(size_t n, C c) {
        grow(n);
        for (size_t i = 0; i < n; i++) p_[i] = c;
        n_ = n;
        if (p_) p_[n_] = 0;
    }
    basic_string(const basic_string &o) { assign(o.data(), o.n_); }
    basic_string(basic_string &&o) noexcept : p_(o.p_), n_(o.n_), cap_(o.cap_) { o.p_ = nullptr, o.n_ = o.cap_ = 0; }
    ~basic_string() { free(p_); }
    basic_string &operator=(const basic_string &o) { return this == &o ? *this : assign(o.data(), o.n_); }
    basic_string &operator=(basic_string &&o) noexcept {
        if (this == &o) return *this;
        free(p_);
        p_ = o.p_, n_ = o.n_, cap_ = o.cap_;
        o.p_ = nullptr, o.n_ = o.cap_ = 0;
        return *this;
    }
    basic_string &operator=(const C *s) { return assign(s, len(s)); }
    basic_string &operator=(C c) { return assign(&c, 1); }
    basic_string &assign(const C *s, size_t n) {
        if (s >= p_ && p_ && s <= p_ + n_) {   // a part of this string
            basic_string t(s, n);
            return *this = move(t);
        }
        n_ = 0;
        grow(n);
        if (n) memcpy(p_, s, n * sizeof(C));
        n_ = n;
        if (p_) p_[n_] = 0;
        return *this;
    }

    const C *c_str() const {
        static const C none[1] = {0};
        return p_ ? p_ : none;
    }
    const C *data() const { return c_str(); }
    C *data() {
        grow(1);
        return p_;
    }
    size_t size() const { return n_; }
    size_t length() const { return n_; }
    bool empty() const { return n_ == 0; }
    C &operator[](size_t i) { return p_[i]; }
    const C &operator[](size_t i) const { return c_str()[i]; }
    C &front() { return p_[0]; }
    C front() const { return p_[0]; }
    C &back() { return p_[n_ - 1]; }
    C back() const { return p_[n_ - 1]; }
    C *begin() { return p_; }
    C *end() { return p_ + n_; }
    const C *begin() const { return c_str(); }
    const C *end() const { return c_str() + n_; }
    void clear() {
        n_ = 0;
        if (p_) p_[0] = 0;
    }
    void reserve(size_t n) { grow(n); }
    void resize(size_t n, C c = 0) {
        grow(n);
        for (size_t i = n_; i < n; i++) p_[i] = c;
        n_ = n;
        if (p_) p_[n_] = 0;
    }
    void push_back(C c) { append(&c, 1); }
    void pop_back() { p_[--n_] = 0; }

    basic_string &append(const C *s, size_t n) {
        if (!n) return *this;
        if (p_ && s >= p_ && s <= p_ + n_) {
            basic_string t(s, n);
            return append(t.p_, n);
        }
        grow(n_ + n);
        memcpy(p_ + n_, s, n * sizeof(C));
        n_ += n;
        p_[n_] = 0;
        return *this;
    }
    basic_string &append(const C *s) { return append(s, len(s)); }
    basic_string &append(const basic_string &s) { return append(s.data(), s.n_); }
    basic_string &append(size_t n, C c) {
        while (n--) push_back(c);
        return *this;
    }
    basic_string &operator+=(const basic_string &s) { return append(s); }
    basic_string &operator+=(const C *s) { return append(s); }
    basic_string &operator+=(C c) { return append(&c, 1); }

    basic_string substr(size_t pos = 0, size_t n = npos) const {
        if (pos > n_) pos = n_;
        if (n > n_ - pos) n = n_ - pos;
        return basic_string(c_str() + pos, n);
    }
    basic_string &erase(size_t pos = 0, size_t n = npos) {
        if (pos > n_) pos = n_;
        if (n > n_ - pos) n = n_ - pos;
        if (n) {
            memmove(p_ + pos, p_ + pos + n, (n_ - pos - n) * sizeof(C));
            n_ -= n;
            p_[n_] = 0;
        }
        return *this;
    }
    C *erase(const C *a, const C *b) {
        size_t i = (size_t)(a - c_str());
        erase(i, (size_t)(b - a));
        return p_ + i;
    }
    C *erase(const C *a) { return erase(a, a + 1); }
    basic_string &insert(size_t pos, const basic_string &s) {
        if (pos > n_) pos = n_;
        basic_string t = substr(0, pos);
        t += s;
        t.append(c_str() + pos, n_ - pos);
        return *this = move(t);
    }
    basic_string &insert(size_t pos, const C *s) { return insert(pos, basic_string(s)); }
    basic_string &insert(size_t pos, size_t n, C c) { return insert(pos, basic_string(n, c)); }
    basic_string &replace(size_t pos, size_t n, const basic_string &s) {
        if (pos > n_) pos = n_;
        if (n > n_ - pos) n = n_ - pos;
        basic_string t = substr(0, pos);
        t += s;
        t.append(c_str() + pos + n, n_ - pos - n);
        return *this = move(t);
    }

    size_t find(const C *s, size_t pos, size_t n) const {
        const C *d = c_str();
        if (n == 0) return pos <= n_ ? pos : npos;
        for (size_t i = pos; i + n <= n_; i++)
            if (d[i] == s[0] && !memcmp(d + i, s, n * sizeof(C))) return i;
        return npos;
    }
    size_t find(const basic_string &s, size_t pos = 0) const { return find(s.data(), pos, s.n_); }
    size_t find(const C *s, size_t pos = 0) const { return find(s, pos, len(s)); }
    size_t find(C c, size_t pos = 0) const { return find(&c, pos, 1); }
    size_t rfind(const C *s, size_t pos, size_t n) const {
        if (n > n_) return npos;
        size_t i = n_ - n < pos ? n_ - n : pos;
        const C *d = c_str();
        for (;; i--) {
            if (!memcmp(d + i, s, n * sizeof(C))) return i;
            if (i == 0) return npos;
        }
    }
    size_t rfind(const basic_string &s, size_t pos = npos) const { return rfind(s.data(), pos, s.n_); }
    size_t rfind(const C *s, size_t pos = npos) const { return rfind(s, pos, len(s)); }
    size_t rfind(C c, size_t pos = npos) const { return rfind(&c, pos, 1); }
    size_t find_first_of(const C *set, size_t pos = 0) const {
        for (size_t i = pos; i < n_; i++)
            for (const C *k = set; *k; k++)
                if (p_[i] == *k) return i;
        return npos;
    }
    size_t find_first_not_of(const C *set, size_t pos = 0) const {
        for (size_t i = pos; i < n_; i++) {
            bool in = false;
            for (const C *k = set; *k; k++) in |= p_[i] == *k;
            if (!in) return i;
        }
        return npos;
    }
    size_t find_last_not_of(const C *set, size_t pos = npos) const {
        if (!n_) return npos;
        for (size_t i = pos < n_ ? pos : n_ - 1;; i--) {
            bool in = false;
            for (const C *k = set; *k; k++) in |= p_[i] == *k;
            if (!in) return i;
            if (i == 0) return npos;
        }
    }
    size_t find_last_of(const C *set, size_t pos = npos) const {
        if (!n_) return npos;
        for (size_t i = pos < n_ ? pos : n_ - 1;; i--) {
            for (const C *k = set; *k; k++)
                if (p_[i] == *k) return i;
            if (i == 0) return npos;
        }
    }
    int compare(const basic_string &o) const {
        size_t n = n_ < o.n_ ? n_ : o.n_;
        for (size_t i = 0; i < n; i++)
            if (c_str()[i] != o.c_str()[i]) return c_str()[i] < o.c_str()[i] ? -1 : 1;
        return n_ < o.n_ ? -1 : n_ > o.n_ ? 1 : 0;
    }
    int compare(size_t pos, size_t n, const basic_string &o) const { return substr(pos, n).compare(o); }
    void swap(basic_string &o) noexcept {
        mini::swap(p_, o.p_);
        mini::swap(n_, o.n_);
        mini::swap(cap_, o.cap_);
    }

    friend bool operator==(const basic_string &a, const basic_string &b) {
        return a.n_ == b.n_ && (a.n_ == 0 || !memcmp(a.p_, b.p_, a.n_ * sizeof(C)));
    }
    friend bool operator==(const basic_string &a, const C *b) {
        size_t n = len(b);
        return a.n_ == n && (n == 0 || !memcmp(a.p_, b, n * sizeof(C)));
    }
    friend bool operator==(const C *a, const basic_string &b) { return b == a; }
    friend bool operator!=(const basic_string &a, const basic_string &b) { return !(a == b); }
    friend bool operator!=(const basic_string &a, const C *b) { return !(a == b); }
    friend bool operator!=(const C *a, const basic_string &b) { return !(b == a); }
    friend bool operator<(const basic_string &a, const basic_string &b) { return a.compare(b) < 0; }
    friend bool operator>(const basic_string &a, const basic_string &b) { return a.compare(b) > 0; }
    friend bool operator<=(const basic_string &a, const basic_string &b) { return a.compare(b) <= 0; }
    friend bool operator>=(const basic_string &a, const basic_string &b) { return a.compare(b) >= 0; }

    friend basic_string operator+(const basic_string &a, const basic_string &b) {
        basic_string t;
        t.reserve(a.n_ + b.n_);
        t += a;
        t += b;
        return t;
    }
    friend basic_string operator+(basic_string &&a, const basic_string &b) { return move(a += b); }
    friend basic_string operator+(const basic_string &a, const C *b) { return a + basic_string(b); }
    friend basic_string operator+(basic_string &&a, const C *b) { return move(a += b); }
    friend basic_string operator+(const C *a, const basic_string &b) { return basic_string(a) + b; }
    friend basic_string operator+(const basic_string &a, C b) { return a + basic_string(&b, 1); }
    friend basic_string operator+(basic_string &&a, C b) { return move(a += b); }
    friend basic_string operator+(C a, const basic_string &b) { return basic_string(&a, 1) + b; }
};

typedef basic_string<char> string;
typedef basic_string<wchar_t> wstring;

inline string to_string(int v) { char b[32]; snprintf(b, sizeof b, "%d", v); return b; }
inline string to_string(unsigned v) { char b[32]; snprintf(b, sizeof b, "%u", v); return b; }
inline string to_string(long v) { char b[32]; snprintf(b, sizeof b, "%ld", v); return b; }
inline string to_string(unsigned long v) { char b[32]; snprintf(b, sizeof b, "%lu", v); return b; }
inline string to_string(long long v) { char b[32]; snprintf(b, sizeof b, "%lld", v); return b; }
inline string to_string(unsigned long long v) { char b[32]; snprintf(b, sizeof b, "%llu", v); return b; }
inline string to_string(double v) { char b[352]; snprintf(b, sizeof b, "%f", v); return b; }
inline string to_string(float v) { return to_string((double)v); }

// ---------------------------------------------------------------- optional, pair

struct nullopt_t { constexpr explicit nullopt_t(int) {} };
constexpr nullopt_t nullopt{0};

template <class T> class optional {
    alignas(T) unsigned char buf_[sizeof(T)];
    bool has_ = false;
    T *ptr() { return reinterpret_cast<T *>(buf_); }
    const T *ptr() const { return reinterpret_cast<const T *>(buf_); }

public:
    optional() {}
    optional(nullopt_t) {}
    optional(const T &v) { new (mini_place(), buf_) T(v), has_ = true; }
    optional(T &&v) { new (mini_place(), buf_) T(move(v)), has_ = true; }
    template <class U, class = typename enable_if<not_self<optional, U>::value>::type,
              class = decltype(T(static_cast<U &&>(*(typename no_ref<U>::type *)nullptr)))>
    optional(U &&v) { new (mini_place(), buf_) T(forward<U>(v)), has_ = true; }
    optional(const optional &o) {
        if (o.has_) new (mini_place(), buf_) T(*o), has_ = true;
    }
    optional(optional &&o) {
        if (o.has_) new (mini_place(), buf_) T(move(*o)), has_ = true;
    }
    ~optional() { reset(); }
    optional &operator=(const optional &o) {
        if (this == &o) return *this;
        reset();
        if (o.has_) new (mini_place(), buf_) T(*o), has_ = true;
        return *this;
    }
    optional &operator=(optional &&o) {
        if (this == &o) return *this;
        reset();
        if (o.has_) new (mini_place(), buf_) T(move(*o)), has_ = true;
        return *this;
    }
    optional &operator=(nullopt_t) { reset(); return *this; }
    void reset() {
        if (has_) ptr()->~T(), has_ = false;
    }
    template <class... A> T &emplace(A &&...a) {
        reset();
        new (mini_place(), buf_) T(forward<A>(a)...), has_ = true;
        return *ptr();
    }
    bool has_value() const { return has_; }
    explicit operator bool() const { return has_; }
    T &operator*() { return *ptr(); }
    const T &operator*() const { return *ptr(); }
    T *operator->() { return ptr(); }
    const T *operator->() const { return ptr(); }
    T &value() { if (!has_) abort(); return *ptr(); }
    const T &value() const { if (!has_) abort(); return *ptr(); }
    template <class U> T value_or(U &&d) const { return has_ ? *ptr() : T(forward<U>(d)); }
    friend bool operator==(const optional &a, const optional &b) {
        return a.has_ == b.has_ && (!a.has_ || *a == *b);
    }
    friend bool operator!=(const optional &a, const optional &b) { return !(a == b); }
    friend bool operator==(const optional &a, nullopt_t) { return !a.has_; }
    friend bool operator!=(const optional &a, nullopt_t) { return a.has_; }
};

template <class A, class B> struct pair {
    A first;
    B second;
    pair() : first(), second() {}
    pair(const A &a, const B &b) : first(a), second(b) {}
    template <class X, class Y> pair(X &&a, Y &&b) : first(forward<X>(a)), second(forward<Y>(b)) {}
    bool operator==(const pair &o) const { return first == o.first && second == o.second; }
};
template <class A, class B> pair<A, B> make_pair(A a, B b) { return pair<A, B>(move(a), move(b)); }

// ---------------------------------------------------------------- map, set (sorted vectors)

template <class K, class V> class map {
    vector<pair<K, V>> v_;
    size_t lower(const K &k) const {
        size_t a = 0, b = v_.size();
        while (a < b) {
            size_t m = (a + b) / 2;
            if (v_[m].first < k) a = m + 1; else b = m;
        }
        return a;
    }

public:
    typedef pair<K, V> *iterator;
    typedef const pair<K, V> *const_iterator;
    size_t size() const { return v_.size(); }
    bool empty() const { return v_.empty(); }
    void clear() { v_.clear(); }
    iterator begin() { return v_.begin(); }
    iterator end() { return v_.end(); }
    const_iterator begin() const { return v_.begin(); }
    const_iterator end() const { return v_.end(); }
    iterator find(const K &k) {
        size_t i = lower(k);
        return i < v_.size() && !(k < v_[i].first) ? v_.begin() + i : end();
    }
    const_iterator find(const K &k) const {
        size_t i = lower(k);
        return i < v_.size() && !(k < v_[i].first) ? v_.begin() + i : end();
    }
    size_t count(const K &k) const { return find(k) != end(); }
    V &operator[](const K &k) {
        size_t i = lower(k);
        if (i < v_.size() && !(k < v_[i].first)) return v_[i].second;
        return v_.insert(v_.begin() + i, pair<K, V>(k, V()))->second;
    }
    V &at(const K &k) {
        iterator it = find(k);
        if (it == end()) abort();
        return it->second;
    }
    const V &at(const K &k) const {
        const_iterator it = find(k);
        if (it == end()) abort();
        return it->second;
    }
    size_t erase(const K &k) {
        iterator it = find(k);
        if (it == end()) return 0;
        v_.erase(it);
        return 1;
    }
};

template <class K> class set {
    vector<K> v_;
    size_t lower(const K &k) const {
        size_t a = 0, b = v_.size();
        while (a < b) {
            size_t m = (a + b) / 2;
            if (v_[m] < k) a = m + 1; else b = m;
        }
        return a;
    }

public:
    typedef const K *iterator;
    typedef const K *const_iterator;
    size_t size() const { return v_.size(); }
    bool empty() const { return v_.empty(); }
    void clear() { v_.clear(); }
    const K *begin() const { return v_.begin(); }
    const K *end() const { return v_.end(); }
    const K *find(const K &k) const {
        size_t i = lower(k);
        return i < v_.size() && !(k < v_[i]) ? v_.begin() + i : end();
    }
    size_t count(const K &k) const { return find(k) != end(); }
    bool insert(const K &k) {
        size_t i = lower(k);
        if (i < v_.size() && !(k < v_[i])) return false;
        v_.insert(v_.begin() + i, k);
        return true;
    }
    size_t erase(const K &k) {
        const K *it = find(k);
        if (it == end()) return 0;
        v_.erase(it);
        return 1;
    }
};

// ---------------------------------------------------------------- function

template <class S> class function;
template <class R, class... A> class function<R(A...)> {
    struct Ops {
        R (*call)(void *, A &&...);
        void *(*clone)(const void *);
        void (*drop)(void *);
    };
    template <class F> struct Box {
        static R call(void *f, A &&...a) { return (*static_cast<F *>(f))(forward<A>(a)...); }
        static void *clone(const void *f) { return new (mini_place(), grab(sizeof(F))) F(*static_cast<const F *>(f)); }
        static void drop(void *f) {
            static_cast<F *>(f)->~F();
            free(f);
        }
        static constexpr Ops ops = {call, clone, drop};
    };
    void *f_ = nullptr;
    const Ops *ops_ = nullptr;

public:
    function() {}
    function(decltype(nullptr)) {}
    template <class F, class = typename enable_if<not_self<function, F>::value>::type,
              class = decltype(static_cast<R>((*(typename no_ref<F>::type *)nullptr)(*(typename no_ref<A>::type *)nullptr...)))>
    function(F &&f) {
        typedef typename decay<F>::type FT;          // a const lambda is stored as a plain one
        f_ = new (mini_place(), grab(sizeof(FT))) FT(forward<F>(f));
        ops_ = &Box<FT>::ops;
    }
    function(const function &o) {
        if (o.f_) f_ = o.ops_->clone(o.f_), ops_ = o.ops_;
    }
    function(function &&o) noexcept : f_(o.f_), ops_(o.ops_) { o.f_ = nullptr, o.ops_ = nullptr; }
    ~function() {
        if (f_) ops_->drop(f_);
    }
    function &operator=(const function &o) {
        if (this != &o) {
            function t(o);
            swap(t);
        }
        return *this;
    }
    function &operator=(function &&o) noexcept {
        function t(move(o));
        swap(t);
        return *this;
    }
    function &operator=(decltype(nullptr)) {
        function t;
        swap(t);
        return *this;
    }
    void swap(function &o) noexcept {
        mini::swap(f_, o.f_);
        mini::swap(ops_, o.ops_);
    }
    explicit operator bool() const { return f_ != nullptr; }
    R operator()(A... a) const { return ops_->call(f_, forward<A>(a)...); }
};

}  // namespace mini
