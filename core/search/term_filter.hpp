////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 by EMC Corporation, All Rights Reserved
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is EMC Corporation
///
/// @author Andrey Abramov
////////////////////////////////////////////////////////////////////////////////

#ifndef IRESEARCH_TERM_FILTER_H
#define IRESEARCH_TERM_FILTER_H

#include "search/filter.hpp"
#include "utils/string.hpp"

NS_ROOT

class by_term;

////////////////////////////////////////////////////////////////////////////////
/// @struct by_term_options
/// @brief options for term filter
////////////////////////////////////////////////////////////////////////////////
struct IRESEARCH_API by_term_options : single_term_options<by_term> { };

//////////////////////////////////////////////////////////////////////////////
/// @class by_term 
/// @brief user-side term filter
//////////////////////////////////////////////////////////////////////////////
class IRESEARCH_API by_term : public filter_with_field<by_term_options> {
 public:
  DECLARE_FILTER_TYPE();
  DECLARE_FACTORY();

  by_term() = default;

  using filter::prepare;

  virtual filter::prepared::ptr prepare(
    const index_reader& rdr,
    const order::prepared& ord,
    boost_t boost,
    const attribute_view& ctx) const override;
}; // by_term

NS_END

#endif // IRESEARCH_TERM_FILTER_H
