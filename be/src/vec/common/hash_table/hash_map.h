// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
// This file is copied from
// https://github.com/ClickHouse/ClickHouse/blob/master/src/Common/HashTable/HashMap.h
// and modified by Doris

#pragma once

#include "vec/common/hash_table/hash.h"
#include "vec/common/hash_table/hash_table.h"

namespace doris {
/** NOTE HashMap could only be used for memmoveable (position independent) types.
  * Example: std::string is not position independent in libstdc++ with C++11 ABI or in libc++.
  * Also, key in hash table must be of type, that zero bytes is compared equals to zero key.
  */

struct NoInitTag {};

/// A pair that does not initialize the elements, if not needed.
template <typename First, typename Second>
struct PairNoInit {
    First first;
    Second second;

    PairNoInit() {}

    template <typename First_>
    PairNoInit(First_&& first_, NoInitTag) : first(std::forward<First_>(first_)) {}

    template <typename First_, typename Second_>
    PairNoInit(First_&& first_, Second_&& second_)
            : first(std::forward<First_>(first_)), second(std::forward<Second_>(second_)) {}
};

template <typename Key, typename TMapped, typename Hash, typename TState = HashTableNoState>
struct HashMapCell {
    using Mapped = TMapped;
    using State = TState;

    using value_type = PairNoInit<Key, Mapped>;
    using mapped_type = Mapped;
    using key_type = Key;

    value_type value;

    HashMapCell() = default;
    HashMapCell(const Key& key_, const State&) : value(key_, NoInitTag()) {}
    HashMapCell(const Key& key_, const Mapped& mapped_) : value(key_, mapped_) {}
    HashMapCell(const value_type& value_, const State&) : value(value_) {}

    const Key& get_first() const { return value.first; }
    Mapped& get_second() { return value.second; }
    const Mapped& get_second() const { return value.second; }

    const value_type& get_value() const { return value; }

    static const Key& get_key(const value_type& value) { return value.first; }
    Mapped& get_mapped() { return value.second; }
    const Mapped& get_mapped() const { return value.second; }

    bool key_equals(const Key& key_) const { return value.first == key_; }
    bool key_equals(const Key& key_, size_t /*hash_*/) const { return value.first == key_; }
    bool key_equals(const Key& key_, size_t /*hash_*/, const State& /*state*/) const {
        return value.first == key_;
    }

    void set_hash(size_t /*hash_value*/) {}
    size_t get_hash(const Hash& hash) const { return hash(value.first); }

    bool is_zero(const State& state) const { return is_zero(value.first, state); }
    static bool is_zero(const Key& key, const State& /*state*/) { return ZeroTraits::check(key); }

    /// Set the key value to zero.
    void set_zero() { ZeroTraits::set(value.first); }

    /// Do I need to store the zero key separately (that is, can a zero key be inserted into the hash table).
    static constexpr bool need_zero_value_storage = true;

    void set_mapped(const value_type& value_) { value.second = value_.second; }
};

template <typename Key, typename Mapped, typename Hash, typename State>
ALWAYS_INLINE inline auto lookup_result_get_key(HashMapCell<Key, Mapped, Hash, State>* cell) {
    return &cell->get_first();
}

template <typename Key, typename Mapped, typename Hash, typename State>
ALWAYS_INLINE inline auto lookup_result_get_mapped(HashMapCell<Key, Mapped, Hash, State>* cell) {
    return &cell->get_second();
}

template <typename Key, typename TMapped, typename Hash, typename TState = HashTableNoState>
struct HashMapCellWithSavedHash : public HashMapCell<Key, TMapped, Hash, TState> {
    using Base = HashMapCell<Key, TMapped, Hash, TState>;

    size_t saved_hash;

    using Base::Base;

    bool key_equals(const Key& key_) const { return this->value.first == key_; }
    bool key_equals(const Key& key_, size_t hash_) const {
        return saved_hash == hash_ && this->value.first == key_;
    }
    bool key_equals(const Key& key_, size_t hash_, const typename Base::State&) const {
        return key_equals(key_, hash_);
    }

    void set_hash(size_t hash_value) { saved_hash = hash_value; }
    size_t get_hash(const Hash& /*hash_function*/) const { return saved_hash; }
};

template <typename Key, typename Mapped, typename Hash, typename State>
ALWAYS_INLINE inline auto lookup_result_get_key(
        HashMapCellWithSavedHash<Key, Mapped, Hash, State>* cell) {
    return &cell->get_first();
}

template <typename Key, typename Mapped, typename Hash, typename State>
ALWAYS_INLINE inline auto lookup_result_get_mapped(
        HashMapCellWithSavedHash<Key, Mapped, Hash, State>* cell) {
    return &cell->get_second();
}

template <typename Key, typename Cell, typename Hash = DefaultHash<Key>,
          typename Grower = HashTableGrower<>, typename Allocator = Allocator<true, true> >
class HashMapTable : public HashTable<Key, Cell, Hash, Grower, Allocator> {
public:
    using Self = HashMapTable;
    using Base = HashTable<Key, Cell, Hash, Grower, Allocator>;

    using key_type = Key;
    using value_type = typename Cell::value_type;
    using mapped_type = typename Cell::Mapped;

    using LookupResult = typename Base::LookupResult;

    using HashTable<Key, Cell, Hash, Grower, Allocator>::HashTable;

    /// Call func(Mapped &) for each hash map element.
    template <typename Func>
    void for_each_mapped(Func&& func) {
        for (auto& v : *this) func(v.get_second());
    }

    mapped_type& ALWAYS_INLINE operator[](Key x) {
        typename HashMapTable::LookupResult it;
        bool inserted;
        this->emplace(x, it, inserted);

        /** It may seem that initialization is not necessary for POD-types (or __has_trivial_constructor),
          *  since the hash table memory is initially initialized with zeros.
          * But, in fact, an empty cell may not be initialized with zeros in the following cases:
          * - ZeroValueStorage (it only zeros the key);
          * - after resizing and moving a part of the cells to the new half of the hash table, the old cells also have only the key to zero.
          *
          * On performance, there is almost always no difference, due to the fact that it->second is usually assigned immediately
          *  after calling `operator[]`, and since `operator[]` is inlined, the compiler removes unnecessary initialization.
          *
          * Sometimes due to initialization, the performance even grows. This occurs in code like `++map[key]`.
          * When we do the initialization, for new cells, it's enough to make `store 1` right away.
          * And if we did not initialize, then even though there was zero in the cell,
          *  the compiler can not guess about this, and generates the `load`, `increment`, `store` code.
          */
        if (inserted) new (lookup_result_get_mapped(it)) mapped_type();

        return *lookup_result_get_mapped(it);
    }

    template <typename MappedType>
    char* get_null_key_data() {
        return nullptr;
    }
    bool has_null_key_data() const { return false; }
};

} // namespace doris
