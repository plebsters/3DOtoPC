// state_ar.h - internal: the archive save states are written to and read from.
// One code path does both: each field is passed to the archive, which copies
// it out (saving) or in (loading). Structures are handled in savestate.cpp.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tdort {

struct StateAr {
    bool saving = true;
    std::vector<uint8_t> buf;
    size_t pos = 0;
    bool bad = false;                     // loading ran past the end: the file is damaged

    void raw(void* p, size_t n) {
        if (saving) { const uint8_t* b = static_cast<const uint8_t*>(p); buf.insert(buf.end(), b, b + n); return; }
        if (bad || pos + n > buf.size()) { bad = true; std::memset(p, 0, n); return; }
        std::memcpy(p, &buf[pos], n); pos += n;
    }
    size_t count(size_t n) {
        uint64_t v = n; raw(&v, sizeof v);
        if (!saving && v > buf.size()) { bad = true; v = 0; }
        return size_t(v);
    }

    template <class T> void operator()(T& v) {
        if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) raw(&v, sizeof v);
        else io_struct(v);
    }
    template <class T, size_t N> void operator()(T (&a)[N]) { for (auto& x : a) (*this)(x); }
    template <class T, size_t N> void operator()(std::array<T, N>& a) { for (auto& x : a) (*this)(x); }
    void operator()(std::string& s) {
        size_t n = count(s.size());
        if (!saving) s.assign(n, '\0');
        if (n) raw(&s[0], n);
    }
    template <class A, class B> void operator()(std::pair<A, B>& p) { (*this)(p.first); (*this)(p.second); }
    template <class T> void operator()(std::vector<T>& v) {
        size_t n = count(v.size());
        if (!saving) v.assign(n, T{});
        if constexpr (std::is_arithmetic_v<T>) { if (n) raw(v.data(), n * sizeof(T)); }
        else for (auto& x : v) (*this)(x);
    }
    template <class T> void operator()(std::deque<T>& v) {
        size_t n = count(v.size());
        if (!saving) { v.clear(); v.resize(n); }
        for (auto& x : v) (*this)(x);
    }
    template <class K, class V, class C> void operator()(std::map<K, V, C>& m) {
        size_t n = count(m.size());
        if (saving) { for (auto& kv : m) { K k = kv.first; (*this)(k); (*this)(kv.second); } return; }
        m.clear();
        for (size_t i = 0; i < n && !bad; ++i) { K k{}; (*this)(k); (*this)(m[k]); }
    }
    template <class K, class C> void operator()(std::set<K, C>& s) {
        size_t n = count(s.size());
        if (saving) { for (K k : s) (*this)(k); return; }
        s.clear();
        for (size_t i = 0; i < n && !bad; ++i) { K k{}; (*this)(k); s.insert(k); }
    }
    template <class K> void operator()(std::unordered_set<K>& s) {
        size_t n = count(s.size());
        if (saving) { for (K k : s) (*this)(k); return; }
        s.clear();
        for (size_t i = 0; i < n && !bad; ++i) { K k{}; (*this)(k); s.insert(k); }
    }

    template <class T> void io_struct(T& v);   // savestate.cpp
};

} // namespace tdort
