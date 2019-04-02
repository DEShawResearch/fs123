#pragma once

#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <string>

namespace core123{
template <typename T>
struct named_ref_space;

template <typename T>
struct named_ref{
    named_ref() = delete;
    operator T&() {
        paranoid_check();
        return *m_ptr;
    }
    // const-ness is propagated through to the m_ptr.  I.e.,
    // if you define:
    //   const named_ref<Foo> f;
    // then you can't modify f by assigning to it, even though that
    // wouldn't actually change the value of m_ptr.
    operator const T&() const {
        paranoid_check();
        return *m_ptr;
    }
    // operator= isn't strictly necessary, but it allows you to write
    //   nri = 3
    // instead of:
    //   (int&)nri = 3;
    //
    // On the other hand, you don't need an assignment operator to write:
    //   nri += 3;
    // The T& conversion operator is sufficient.
    named_ref& operator=(const T& rhs){
        paranoid_check();
        *m_ptr = rhs;
        return *this;
    }
private:
    friend struct named_ref_space<T>;
    named_ref(T *p, std::nullptr_t) : m_ptr(p){
        if(p == nullptr)
            throw std::runtime_error("named_ref<T> constructor:  pointer argument must be non-NULL.");
    }
    void paranoid_check() const {
        if(m_ptr == nullptr)
            throw std::logic_error("named_ref<T>:  m_ptr is null.  Probable static initialization ordering error.  File-scoped statics are not guaranteed to be initialized before member functions in the same file are executed.  Consider making the *this named_ref function-scoped rather than file-scoped.");
    }

    T *m_ptr;
};

template <typename T>
struct named_ref_space{
    typedef std::map<std::string, T> named_ref_map_t;
    named_ref<T> declare(const std::string& name, const T& dflt = T{}){
        if(name.empty())
            throw std::runtime_error("named_int:declare(name):  name must be non-empty");
        std::lock_guard<std::mutex> lg(mtx);
        typename named_ref_map_t::iterator iter;
        std::tie(iter, std::ignore) = themap.insert(std::make_pair(name, dflt));
        // N.B.  nothing is ever deleted or erased from themap, so a
        // reference to the inserted value is never invalidated.  Therefore, 
        // the caller can safely hang onto this pointer forever.
        return named_ref<T>(&iter->second, nullptr);
    }
    // Return a *const* map even if *this is non-const, so that the
    // caller can't violate our assumption that nothing is ever
    // deleted from the map.
    const named_ref_map_t& getmap() const {
        return themap;
    }
private:
    std::mutex mtx;
    named_ref_map_t themap;
};
} // namespace core123
