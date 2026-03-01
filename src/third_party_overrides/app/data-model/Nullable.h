/*
 *
 *    Copyright (c) 2020-2021 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

// Local override of esp_matter Nullable.h to avoid GCC 14 std::optional equality
// build failures for non-comparable generated cluster enums.
#include <app/util/attribute-storage-null-handling.h>
#include <cstring>
#include <optional>
#include <type_traits>
#include <utility>

namespace chip {
namespace app {
namespace DataModel {

/**
 * NullNullable is an alias for NullOptional, for better readability.
 */
inline constexpr auto NullNullable = NullOptional;

/*
 * Dedicated type for nullable things, to differentiate them from optional
 * things.
 */
template <typename T>
struct Nullable : protected std::optional<T>
{
private:
    template <typename U, typename = void>
    struct HasEqualityOperator : std::false_type
    {};

    template <typename U>
    struct HasEqualityOperator<U, std::void_t<decltype(std::declval<const U &>() == std::declval<const U &>())>> : std::true_type
    {};

    template <typename U = T>
    static inline bool ValuesEqual(const U & lhs, const U & rhs)
    {
        if constexpr (HasEqualityOperator<U>::value)
        {
            return lhs == rhs;
        }
        else if constexpr (std::is_trivially_copyable<U>::value)
        {
            return std::memcmp(&lhs, &rhs, sizeof(U)) == 0;
        }
        else
        {
            return false;
        }
    }

public:

    //
    // The following 'using' statement is needed to make visible
    // all constructors of the base class within this derived class.
    //
    using std::optional<T>::optional;

    // Do NOT pull in optional::operator* or optional::operator->, because that
    // leads people to write code that looks like it should work, and compiles,
    // but does not do the right things with TLV encoding and decoding, when
    // nullable data model objects are involved.

    Nullable(NullOptionalType) : std::optional<T>(std::nullopt) {}

    // Some consumers need an easy way to determine our underlying type.
    using UnderlyingType = T;

    constexpr void SetNull() { std::optional<T>::reset(); }
    constexpr bool IsNull() const { return !std::optional<T>::has_value(); }

    template <class... Args>
    constexpr T & SetNonNull(Args &&... args)
    {
        return std::optional<T>::emplace(std::forward<Args>(args)...);
    }

    template <typename... Args>
    constexpr auto ValueOr(Args &&... args) const
    {
        return std::optional<T>::value_or(std::forward<Args>(args)...);
    }

    inline constexpr const T & Value() const { return std::optional<T>::value(); }
    inline T & Value() { return std::optional<T>::value(); }

    // For integer types, being nullable involves a range restriction.
    template <
        typename U = std::decay_t<T>,
        typename std::enable_if_t<(std::is_integral<U>::value && !std::is_same<U, bool>::value) || std::is_enum<U>::value, int> = 0>
    constexpr bool ExistingValueInEncodableRange() const
    {
        return NumericAttributeTraits<T>::CanRepresentValue(/* isNullable = */ true, Value());
    }

    // For all other types, all values are valid.
    template <typename U                     = std::decay_t<T>,
              typename std::enable_if_t<(!std::is_integral<U>::value || std::is_same<U, bool>::value) && !std::is_enum<U>::value,
                                        int> = 0>
    constexpr bool ExistingValueInEncodableRange() const
    {
        return true;
    }

    // Set the nullable to the `other` nullable, returning true if something actually changed.
    // This can be used to determine if changes occurred on assignment, so that reporting can be triggered
    // only on actual changes.
    constexpr bool Update(const Nullable<T> & other)
    {
        bool changed = *this != other;
        if (changed)
        {
            *this = other;
        }
        return changed;
    }

    // The only fabric-scoped objects in the spec are commands, events and structs inside lists, and none of those can be nullable.
    static constexpr bool kIsFabricScoped = false;

    inline bool operator==(const T & other) const
    {
        if (!this->has_value())
        {
            return false;
        }
        return ValuesEqual(this->value(), other);
    }
    inline bool operator!=(const T & other) const { return !(*this == other); }

    inline bool operator==(const Nullable<T> & other) const
    {
        if (this->has_value() != other.has_value())
        {
            return false;
        }
        if (!this->has_value())
        {
            return true;
        }
        return ValuesEqual(this->value(), other.value());
    }
    inline bool operator!=(const Nullable<T> & other) const { return !(*this == other); }
};

template <class T>
constexpr Nullable<std::decay_t<T>> MakeNullable(T && value)
{
    return Nullable<std::decay_t<T>>(std::in_place, std::forward<T>(value));
}

template <class T, class... Args>
constexpr Nullable<T> MakeNullable(Args &&... args)
{
    return Nullable<T>(std::in_place, std::forward<Args>(args)...);
}

} // namespace DataModel
} // namespace app
} // namespace chip
