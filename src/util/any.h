// Copyright (c) 2023-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_UTIL_ANY_H
#define TKN_UTIL_ANY_H

#include <any>

namespace util {

/**
 * Helper function to access the contained object of a std::any instance.
 * Returns a pointer to the object if passed instance has a value and the type
 * matches, nullptr otherwise.
 */
template<typename T>
T* AnyPtr(const std::any& any) noexcept
{
    T* const* ptr = std::any_cast<T*>(&any);
    return ptr ? *ptr : nullptr;
}

} // namespace util

#endif // TKN_UTIL_ANY_H
