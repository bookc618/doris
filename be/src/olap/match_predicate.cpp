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

#include "olap/match_predicate.h"

#include <roaring/roaring.hh>

#include "exec/olap_utils.h"
#include "olap/field.h"
#include "olap/inverted_index_parser.h"
#include "olap/olap_common.h"
#include "olap/rowset/segment_v2/inverted_index_cache.h"
#include "olap/rowset/segment_v2/inverted_index_reader.h"
#include "olap/schema.h"
#include "olap/tablet_schema.h"
#include "olap/types.h"
#include "olap/utils.h"
#include "runtime/define_primitive_type.h"
#include "runtime/types.h"
#include "vec/common/assert_cast.h"
#include "vec/common/string_ref.h"
#include "vec/data_types/data_type.h"
#include "vec/data_types/data_type_array.h"

namespace doris {
#include "common/compile_check_begin.h"
MatchPredicate::MatchPredicate(uint32_t column_id, const std::string& value, MatchType match_type)
        : ColumnPredicate(column_id), _value(value), _match_type(match_type) {}

PredicateType MatchPredicate::type() const {
    return PredicateType::MATCH;
}

Status MatchPredicate::evaluate(const vectorized::IndexFieldNameAndTypePair& name_with_type,
                                InvertedIndexIterator* iterator, uint32_t num_rows,
                                roaring::Roaring* bitmap) const {
    if (iterator == nullptr) {
        return Status::OK();
    }
    if (_check_evaluate(iterator)) {
        return Status::Error<ErrorCode::INVERTED_INDEX_INVALID_PARAMETERS>(
                "phrase queries require setting support_phrase = true");
    }
    auto type = name_with_type.second;
    const std::string& name = name_with_type.first;
    std::shared_ptr<roaring::Roaring> roaring = std::make_shared<roaring::Roaring>();
    auto inverted_index_query_type = _to_inverted_index_query_type(_match_type);
    TypeDescriptor column_desc = type->get_type_as_type_descriptor();
    if (is_string_type(column_desc.type) ||
        (column_desc.type == TYPE_ARRAY && is_string_type(column_desc.children[0].type))) {
        StringRef match_value;
        auto length = _value.length();
        char* buffer = const_cast<char*>(_value.c_str());
        match_value.replace(buffer, int32_t(length)); //is it safe?
        RETURN_IF_ERROR(iterator->read_from_inverted_index(
                name, &match_value, inverted_index_query_type, num_rows, roaring));
    } else if (column_desc.type == TYPE_ARRAY &&
               is_numeric_type(
                       TabletColumn::get_field_type_by_type(column_desc.children[0].type))) {
        std::vector<char> buf(column_desc.children[0].len);
        const TypeInfo* type_info = get_scalar_type_info(
                TabletColumn::get_field_type_by_type(column_desc.children[0].type));
        RETURN_IF_ERROR(type_info->from_string(buf.data(), _value));
        RETURN_IF_ERROR(iterator->read_from_inverted_index(
                name, buf.data(), inverted_index_query_type, num_rows, roaring, true));
    }

    // mask out null_bitmap, since NULL cmp VALUE will produce NULL
    //  and be treated as false in WHERE
    // keep it after query, since query will try to read null_bitmap and put it to cache
    if (iterator->has_null()) {
        InvertedIndexQueryCacheHandle null_bitmap_cache_handle;
        RETURN_IF_ERROR(iterator->read_null_bitmap(&null_bitmap_cache_handle));
        std::shared_ptr<roaring::Roaring> null_bitmap = null_bitmap_cache_handle.get_bitmap();
        if (null_bitmap) {
            *bitmap -= *null_bitmap;
        }
    }

    *bitmap &= *roaring;
    return Status::OK();
}

InvertedIndexQueryType MatchPredicate::_to_inverted_index_query_type(MatchType match_type) const {
    auto ret = InvertedIndexQueryType::UNKNOWN_QUERY;
    switch (match_type) {
    case MatchType::MATCH_ANY:
        ret = InvertedIndexQueryType::MATCH_ANY_QUERY;
        break;
    case MatchType::MATCH_ALL:
        ret = InvertedIndexQueryType::MATCH_ALL_QUERY;
        break;
    case MatchType::MATCH_PHRASE:
        ret = InvertedIndexQueryType::MATCH_PHRASE_QUERY;
        break;
    case MatchType::MATCH_PHRASE_PREFIX:
        ret = InvertedIndexQueryType::MATCH_PHRASE_PREFIX_QUERY;
        break;
    case MatchType::MATCH_REGEXP:
        ret = InvertedIndexQueryType::MATCH_REGEXP_QUERY;
        break;
    case MatchType::MATCH_PHRASE_EDGE:
        ret = InvertedIndexQueryType::MATCH_PHRASE_EDGE_QUERY;
        break;
    default:
        DCHECK(false);
    }
    return ret;
}

bool MatchPredicate::_check_evaluate(InvertedIndexIterator* iterator) const {
    if (_match_type == MatchType::MATCH_PHRASE || _match_type == MatchType::MATCH_PHRASE_PREFIX ||
        _match_type == MatchType::MATCH_PHRASE_EDGE) {
        if (iterator->get_inverted_index_reader_type() == InvertedIndexReaderType::FULLTEXT &&
            get_parser_phrase_support_string_from_properties(iterator->get_index_properties()) ==
                    INVERTED_INDEX_PARSER_PHRASE_SUPPORT_NO) {
            return true;
        }
    }
    return false;
}

} // namespace doris
#include "common/compile_check_end.h"