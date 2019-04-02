#pragma once
#include <cstddef>
#include <memory>
#include "sodium.h"

// sodium_allocator is a std::allocator that uses sodium_allocarray
// and sodium_free "under the hood".  The intention is to use it as
// the allocator for vectors and strings that may hold sensitive data.
// (See secret_sp in secret_manager.hpp).  Whether this actually serves
// any useful purpose, and whether it has a performance impact is TBD.
//
// FWIW, on CentOS7, sodium_allocarray and the kernel are coordinated
// enough that data allocated this way is not written out to core
// files.

// Issues:

//   - it's not clear what sodium_{allocarray,free} do when
//     they detect a problem.  Do they return NULL?  Do they
//     exit?
//   - They are a lot slower than malloc/new.
//   - they burn 3 or 4 pages of memory for every object.  BEWARE!

// Based on https://howardhinnant.github.io/allocator_boilerplate.html
// which contains an explicit disavowal of copyright and encourages
// copying.

#if SODIUM_LIBRARY_VERSION_MAJOR < 7
template <typename T>
using sodium_allocator = std::allocator<T>;

#else

template <class T>
class sodium_allocator
{
public:
    using value_type    = T;

//     using pointer       = value_type*;
//     using const_pointer = typename std::pointer_traits<pointer>::template
//                                                     rebind<value_type const>;
//     using void_pointer       = typename std::pointer_traits<pointer>::template
//                                                           rebind<void>;
//     using const_void_pointer = typename std::pointer_traits<pointer>::template
//                                                           rebind<const void>;

//     using difference_type = typename std::pointer_traits<pointer>::difference_type;
//     using size_type       = std::make_unsigned_t<difference_type>;

//     template <class U> struct rebind {typedef allocator<U> other;};

    sodium_allocator() noexcept {}  // not required, unless used
    template <class U> sodium_allocator(sodium_allocator<U> const&) noexcept {}

    value_type*  // Use pointer if pointer is not a value_type*
    allocate(std::size_t n)
    {
        return static_cast<value_type*>(sodium_allocarray(n,  sizeof(value_type)));
    }

    void
    deallocate(value_type* p, std::size_t) noexcept  // Use pointer if pointer is not a value_type*
    {
        sodium_free(p);
    }

//     value_type*
//     allocate(std::size_t n, const_void_pointer)
//     {
//         return allocate(n);
//     }

//     template <class U, class ...Args>
//     void
//     construct(U* p, Args&& ...args)
//     {
//         ::new(p) U(std::forward<Args>(args)...);
//     }

//     template <class U>
//     void
//     destroy(U* p) noexcept
//     {
//         p->~U();
//     }

//     std::size_t
//     max_size() const noexcept
//     {
//         return std::numeric_limits<size_type>::max();
//     }

//     allocator
//     select_on_container_copy_construction() const
//     {
//         return *this;
//     }

//     using propagate_on_container_copy_assignment = std::false_type;
//     using propagate_on_container_move_assignment = std::false_type;
//     using propagate_on_container_swap            = std::false_type;
//     using is_always_equal                        = std::is_empty<allocator>;
};

template <class T, class U>
bool
operator==(sodium_allocator<T> const&, sodium_allocator<U> const&) noexcept
{
    return true;
}

template <class T, class U>
bool
operator!=(sodium_allocator<T> const& x, sodium_allocator<U> const& y) noexcept
{
    return !(x == y);
}

#endif
