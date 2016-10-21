//
// IResearch search engine 
// 
// Copyright � 2016 by EMC Corporation, All Rights Reserved
// 
// This software contains the intellectual property of EMC Corporation or is licensed to
// EMC Corporation from third parties. Use of this software and the intellectual property
// contained therein is expressly limited to the terms and conditions of the License
// Agreement under which it is provided by or on behalf of EMC.
// 

#ifndef IRESEARCH_FIELD_DATA_H
#define IRESEARCH_FIELD_DATA_H

#include "field_meta.hpp"
#include "postings.hpp"

#include "index/iterators.hpp"

#include "utils/block_pool.hpp"
#include "utils/memory.hpp"
#include "utils/noncopyable.hpp"

#include <vector>
#include <tuple>
#include <unordered_map>

NS_ROOT

class token_stream;
class analyzer;
struct offset;
struct payload;
class format;
struct directory;

NS_LOCAL

typedef block_pool< byte_type > byte_block_pool;
typedef block_pool< size_t > int_block_pool;

const size_t INT_BLOCK_SIZE = 1 << 13;
const size_t BYTE_BLOCK_SIZE = 1 << 15;

NS_END

NS_BEGIN( detail ) 
class term_iterator;
class doc_iterator;
NS_END

/* TODO: since actual string data stores in global ids
 * it looks like we should not to create additional copy
 * of field name. It is implicity interned by global ids */
class IRESEARCH_API field_data : util::noncopyable {
 public:
  DECLARE_PTR(field_data);

  field_data(
    const string_ref& name,
    size_t id,
    byte_block_pool::inserter* byte_writer,
    int_block_pool::inserter* int_writer
  );

  ~field_data();

  const field_meta& meta() const { return meta_; }

  bool invert(token_stream* tokens, const flags& features, float_t boost, doc_id_t id);
  term_iterator::ptr iterator() const;

 private:
  friend class detail::term_iterator;
  friend class detail::doc_iterator;
  friend class fields_data;

  void init(const doc_id_t& doc_id);

  void new_term(posting& p, doc_id_t did, const payload* pay, const offset* offs);

  void add_term(posting& p, doc_id_t did, const payload* pay, const offset* offs);

  void write_prox(posting& p, int_block_pool::iterator& where,
                  uint32_t prox, const payload* pay);

  void write_offset(posting& p, int_block_pool::iterator& where,
                    const offset* offs);

  field_meta meta_;
  postings terms_;
  byte_block_pool::inserter* byte_writer_;
  int_block_pool::inserter* int_writer_;
  doc_id_t last_doc_;
  uint32_t pos_;
  uint32_t last_pos_;
  uint32_t len_;
  uint32_t num_overlap_;
  uint32_t offs_;
  uint32_t last_start_offs_;
  uint32_t max_term_freq_;
  uint32_t unq_term_cnt_;
  float_t boost_;
};

struct flush_state;

class IRESEARCH_API fields_data: util::noncopyable {
 public:
  typedef std::unordered_map<hashed_string_ref, field_data> fields_map;

  fields_data();

  field_data& get(const string_ref& name);
  size_t size() const { return fields_.size(); }
  fields_data& operator+=(const flags& features) {
    features_ |= features;
    return *this;
  }
  const flags& features() { return features_; }
  void flush(flush_state& state);
  void reset();

 private:
  IRESEARCH_API_PRIVATE_VARIABLES_BEGIN
  fields_map fields_;
  byte_block_pool byte_pool_;
  byte_block_pool::inserter byte_writer_;
  int_block_pool int_pool_;
  int_block_pool::inserter int_writer_;
  flags features_;
  IRESEARCH_API_PRIVATE_VARIABLES_END
};

NS_END

#endif