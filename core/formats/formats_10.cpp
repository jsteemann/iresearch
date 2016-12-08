//
// IResearch search engine 
// 
// Copyright © 2016 by EMC Corporation, All Rights Reserved
// 
// This software contains the intellectual property of EMC Corporation or is licensed to
// EMC Corporation from third parties. Use of this software and the intellectual property
// contained therein is expressly limited to the terms and conditions of the License
// Agreement under which it is provided by or on behalf of EMC.
// 

#include "shared.hpp"

#include "formats_10.hpp"
#include "formats_burst_trie.hpp"
#include "format_utils.hpp"

#include "document/serializer.hpp"

#include "analysis/token_attributes.hpp"

#include "index/field_meta.hpp"
#include "index/index_meta.hpp"
#include "index/file_names.hpp"
#include "index/index_reader.hpp"

#include "store/store_utils.hpp"

#include "utils/bit_utils.hpp"
#include "utils/log.hpp"
#include "utils/timer_utils.hpp"
#include "utils/std.hpp"
#include "utils/bit_packing.hpp"
#include "utils/type_limits.hpp"
#include "formats.hpp"

#include <array>
#include <cassert>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <type_traits>
#include <deque>

NS_LOCAL

iresearch::bytes_ref read_compact(
    iresearch::index_input& in,
    const iresearch::decompressor& decompressor,
    iresearch::bstring& encode_buf,
    iresearch::bstring& decode_buf) {
  const auto size = iresearch::read_zvint(in);
  size_t buf_size = std::abs(size);

  // -ve to mark uncompressed
  if (size < 0) {
#ifdef IRESEARCH_DEBUG
    const auto read = in.read_bytes(&(decode_buf[0]), buf_size);
    assert(read == buf_size);
#else
    in.read_bytes(&(decode_buf[0]), buf_size);
#endif // IRESEARCH_DEBUG
    return decode_buf;
  }

  iresearch::oversize(encode_buf, buf_size);

#ifdef IRESEARCH_DEBUG
  const auto read = in.read_bytes(&(encode_buf[0]), buf_size);
  assert(read == buf_size);
#else
  in.read_bytes(&(encode_buf[0]), buf_size);
#endif // IRESEARCH_DEBUG

  buf_size = decompressor.deflate(
    reinterpret_cast<const char*>(encode_buf.c_str()), 
    buf_size,
    reinterpret_cast<char*>(&decode_buf[0]), 
    decode_buf.size()
  );

  if (!iresearch::type_limits<iresearch::type_t::address_t>::valid(buf_size)) {
    throw iresearch::index_error(); // corrupted index
  }

  return iresearch::bytes_ref(decode_buf.c_str(), buf_size);
}

void write_compact(
  iresearch::index_output& out,
  iresearch::compressor& compressor,
  const iresearch::byte_type* data,
  size_t size
) {
  // compressor can only handle size of int32_t, so can use the negative flag as a compression flag
  compressor.compress(reinterpret_cast<const char*>(data), size);

  if (compressor.size() < size) {
    assert(compressor.size() <= iresearch::integer_traits<int32_t>::const_max);
    iresearch::write_zvint(out, int32_t(compressor.size()));
    out.write_bytes(compressor.c_str(), compressor.size());

    return;
  }

  assert(size <= iresearch::integer_traits<int32_t>::const_max);
  iresearch::write_zvint(out, int32_t(0) - int32_t(size)); // -ve to mark uncompressed
  out.write_bytes(data, size);
}

NS_END

NS_ROOT
NS_BEGIN( version10 )

// ----------------------------------------------------------------------------
// --SECTION--                                             forward declarations 
// ----------------------------------------------------------------------------

template<typename T, typename M>
std::string file_name(M const& meta);

// ----------------------------------------------------------------------------
// --SECTION--                                                 helper functions 
// ----------------------------------------------------------------------------

NS_BEGIN( detail )
NS_LOCAL

template<typename InputIterator>
inline void delta_decode(InputIterator first, InputIterator last) {
  typedef typename std::iterator_traits<InputIterator>::value_type value_type;
  const auto second = first+1;
  std::transform(second, last, first, second, std::plus<value_type>());
}

inline void prepare_output(
    std::string& str,
    index_output::ptr& out,
    const flush_state& state,
    const string_ref& ext,
    const string_ref& format,
    const int32_t version) {
  assert( !out );

  file_name(str, state.name, ext);
  out = state.dir->create(str);
  format_utils::write_header(*out, format, version);
}

inline void prepare_input(
    std::string& str,
    index_input::ptr& in,
    const reader_state& state,
    const string_ref& ext,
    const string_ref& format,
    const int32_t min_ver,
    const int32_t max_ver ) {
  assert( !in );

  file_name(str, state.meta->name, ext);
  in = state.dir->open(str);
  format_utils::check_header(*in, format, min_ver, max_ver);
}

inline void skip_positions(index_input& in, uint32_t* enc_buf) {
  skip_block(in, postings_writer::BLOCK_SIZE, enc_buf);
}

inline void skip_payload(index_input& in, uint32_t* enc_buf) {
  const size_t size = in.read_vint();
  if (size) {
    skip_block(in, postings_writer::BLOCK_SIZE, enc_buf);
    in.seek(in.file_pointer() + size);
  }
}

inline void skip_offsets(index_input& in, uint32_t* enc_buf) {
  skip_block(in, postings_writer::BLOCK_SIZE, enc_buf);
  skip_block(in, postings_writer::BLOCK_SIZE, enc_buf);
}

NS_END // NS_LOCAL
  
struct skip_state {
  uint64_t doc_ptr{}; // pointer to the beginning of document block
  uint64_t pos_ptr{}; // pointer to the positions of the first document in a document block
  uint64_t pay_ptr{}; // pointer to the payloads of the first document in a document block
  size_t pend_pos{}; // positions to skip before new document block
  doc_id_t doc{ type_limits<type_t::doc_id_t>::invalid() }; // last document in a previous block 
  uint32_t pay_pos{}; // payload size to skip before in new document block 
}; // skip_state

struct doc_state {
  const index_input* pos_in;
  const index_input* pay_in;
  version10::term_meta* term_state;
  uint64_t* freq;
  uint64_t* enc_buf;
  uint64_t tail_start;
  size_t tail_length;
  version10::features features;
};

class pos_iterator;

class doc_iterator : public iresearch::doc_iterator {
 public:
  DECLARE_PTR(doc_iterator);

  DECLARE_FACTORY(doc_iterator);

  doc_iterator() NOEXCEPT
    : skip_levels_(1),
      skip_(postings_writer::BLOCK_SIZE, postings_writer::SKIP_N) {
    std::fill(docs_, docs_ + postings_writer::BLOCK_SIZE, type_limits<type_t::doc_id_t>::invalid());
  }

  void prepare(const flags& field,
               const iresearch::attributes &attrs,
               const flags &features,
               const index_input *doc_in,
               const index_input *pos_in,
               const index_input *pay_in);

  virtual doc_id_t seek(doc_id_t target) override {
    if (target <= doc_->value) {
      return doc_->value;
    }

    seek_to_block(target);
    return iresearch::seek(*this, target);
  }

  virtual doc_id_t value() const override {
    return doc_->value;
  }

  virtual const iresearch::attributes& attributes() const override {
    return attrs_;
  }

#if defined(_MSC_VER)
  #pragma warning( disable : 4706 )
#elif defined (__GNUC__)
  #pragma GCC diagnostic ignored "-Wparentheses"
#endif

  virtual bool next() override {
    if (cur_pos_ == term_state_.docs_count) {
      doc_->value = type_limits<type_t::doc_id_t>::eof();
      return false;
    }

    if (begin_ == end_) {
      refill();
    }

    doc_->value += *begin_;
    refresh();

    ++cur_pos_;
    ++begin_;
    ++doc_freq_;
    return true;
  }

#if defined(_MSC_VER)
  #pragma warning( default : 4706 )
#elif defined (__GNUC__)
  #pragma GCC diagnostic pop
#endif

 private:
  void seek_to_block(doc_id_t target);

  doc_id_t read_skip(skip_state& state, index_input& in) {
    state.doc = in.read_vint();
    state.doc_ptr += in.read_vlong();
    if (features_.position()) {
      state.pend_pos = in.read_vint();
      state.pos_ptr += in.read_vlong();
      const bool has_pay = features_.payload();
      if (has_pay || features_.offset()) {
        if (has_pay) {
          state.pay_pos = in.read_vint();
        }
        state.pay_ptr += in.read_vlong();
      }
    }
    return state.doc;
  }

  void read_end_block(uint64_t size) {
    if (features_.freq()) {
      for (uint64_t i = 0; i < size; ++i) {
        if (shift_unpack_64(doc_in_->read_vlong(), docs_[i])) {
          doc_freqs_[i] = 1;
        } else {
          doc_freqs_[i] = doc_in_->read_vint();
        }
      }
    } else {
      for (uint64_t i = 0; i < size; ++i) {
        docs_[i] = doc_in_->read_vlong();
      }
    }
  }

  void refill() {
    const auto left = term_state_.docs_count - cur_pos_;
    if (left >= postings_writer::BLOCK_SIZE) {
      // read doc deltas
      read_block(*doc_in_, postings_writer::BLOCK_SIZE, enc_buf_, docs_);

      if (features_.freq()) {
        read_block(
          *doc_in_,
          postings_writer::BLOCK_SIZE,
          reinterpret_cast<uint32_t*>(enc_buf_),
          doc_freqs_
        );
      }
      end_ = docs_ + postings_writer::BLOCK_SIZE;
    } else if (1U == term_state_.docs_count) {
      docs_[0] = term_state_.e_single_doc;
      if (term_freq_) {
        doc_freqs_[0] = static_cast<uint32_t>(term_freq_);
      }
      end_ = docs_ + 1;
    } else {
      read_end_block(left);
      end_ = docs_ + left;
    }

    begin_ = docs_;

    // if this is the initial doc_id then set it to min() for proper delta value
    if (!type_limits<type_t::doc_id_t>::valid(doc_->value)) {
      doc_->value = type_limits<type_t::doc_id_t>::min(); // next() will add delta
    }

    // postings_writer::begin_doc(doc_id_t, const frequency*) writes frequency as uint32_t
    doc_freq_ = reinterpret_cast<uint32_t*>(begin_ + postings_writer::BLOCK_SIZE);
  }

  // refreshes attributes
  inline void refresh();

  std::vector<skip_state> skip_levels_;
  skip_reader skip_;
  iresearch::attributes attrs_;
  uint64_t enc_buf_[postings_writer::BLOCK_SIZE];
  doc_id_t docs_[postings_writer::BLOCK_SIZE]; // doc deltas
  uint32_t doc_freqs_[postings_writer::BLOCK_SIZE];
  uint64_t cur_pos_{};
  doc_id_t* begin_{docs_+postings_writer::BLOCK_SIZE};
  doc_id_t* end_{docs_+postings_writer::BLOCK_SIZE};
  uint32_t* doc_freq_{}; // pointer into docs_ to the frequency attribute value for the current doc
  uint64_t term_freq_{}; /* total term frequency */
  pos_iterator* pos_{};
  document* doc_;
  frequency* freq_{};
  index_input::ptr doc_in_;
  version10::term_meta term_state_;
  features features_; /* field features */
}; // doc_iterator 

class mask_doc_iterator final: public doc_iterator {
 public:
  explicit mask_doc_iterator(const document_mask& mask) 
    : mask_(mask) {
  }

  virtual bool next() override {
    while (doc_iterator::next()) {
      if (mask_.find(value()) == mask_.end()) {
        return true;
      }
    }

    return false;
  }

  virtual doc_id_t seek(doc_id_t target) override {
    auto doc = doc_iterator::seek(target);

    if ( mask_.find( target ) == mask_.end() ) {
      return doc;
    }

    next();

    return value();
  }

 private:
  const document_mask& mask_; /* excluded document ids */
}; // mask_doc_iterator

class pos_iterator : public position::impl {
 public:
  DECLARE_PTR(pos_iterator);

  pos_iterator() = default;

  pos_iterator(size_t reserve_attrs): position::impl(reserve_attrs) {
  }

  virtual void clear() {
    value_ = position::INVALID;
  }

  virtual bool next() override {
    if (0 == pend_pos_) {
      value_ = position::NO_MORE;
      return false;
    }

    const uint64_t freq = *freq_;
    if (pend_pos_ > freq) {
      skip(pend_pos_ - freq);
      pend_pos_ = freq;
    }

    if (buf_pos_ == postings_writer::BLOCK_SIZE) {
      refill();
      buf_pos_ = 0;
    }

    // TODO: make INVALID = 0, remove this
    if (value_ == position::INVALID) {
      value_ = 0;
    } 

    value_ += pos_deltas_[buf_pos_];
    read_attributes();
    ++buf_pos_;
    --pend_pos_;
    return true;
  }
  
  virtual uint32_t value() const { return value_; }

 protected:
  // prepares iterator to work
  virtual void prepare(const doc_state& state) {
    pos_in_ = state.pos_in->clone();
    pos_in_->seek(state.term_state->pos_start);
    freq_ = state.freq;
    features_ = state.features; 
    enc_buf_ = reinterpret_cast<uint32_t*>(state.enc_buf);
    tail_start_ = state.tail_start;
    tail_length_ = state.tail_length;  
  }

  // notifies iterator that doc iterator has skipped to a new block
  virtual void prepare(const skip_state& state) {
    pos_in_->seek(state.pos_ptr);
    pend_pos_ = state.pend_pos;
    buf_pos_ = postings_writer::BLOCK_SIZE;
  }

  virtual void read_attributes() { }

  virtual void refill() {
    if (pos_in_->file_pointer() == tail_start_) {
      uint32_t pay_size = 0;
      for (size_t i = 0; i < tail_length_; ++i) {
        if (features_.payload()) {
          if (shift_unpack_32(pos_in_->read_vint(), pos_deltas_[i])) {
            pay_size = pos_in_->read_vint();
          }
          if (pay_size) {
            pos_in_->seek(pos_in_->file_pointer() + pay_size);
          }
        } else {
          pos_deltas_[i] = pos_in_->read_vint();
        }

        if (features_.offset()) {
          uint32_t delta;
          if (shift_unpack_32(pos_in_->read_vint(), delta)) {
            pos_in_->read_vint();
          }
        }
      }
    } else {
      read_block(*pos_in_, postings_writer::BLOCK_SIZE, enc_buf_, pos_deltas_);
    }
  }

  virtual void skip(uint64_t count) {
    uint64_t left = postings_writer::BLOCK_SIZE - buf_pos_;
    if (count >= left) {
      count -= left;
      while (count >= postings_writer::BLOCK_SIZE) {
        // skip positions
        skip_positions(*pos_in_, enc_buf_);
        count -= postings_writer::BLOCK_SIZE;
      }
      refill();
      buf_pos_ = 0;
      left = postings_writer::BLOCK_SIZE;
    }

    if (count < left) {
      buf_pos_ += uint32_t(count);
    }
    clear();
    value_ = 0;
  }

  uint32_t pos_deltas_[postings_writer::BLOCK_SIZE]; /* buffer to store position deltas */
  const uint64_t* freq_; /* lenght of the posting list for a document */
  uint32_t* enc_buf_; /* auxillary buffer to decode data */
  uint64_t pend_pos_{}; /* how many positions "behind" we are */
  uint64_t tail_start_; /* file pointer where the last (vInt encoded) pos delta block is */
  size_t tail_length_; /* number of positions in the last (vInt encoded) pos delta block */
  uint32_t value_{ position::INVALID }; /* current position */
  uint32_t buf_pos_{ postings_writer::BLOCK_SIZE } ; /* current position in pos_deltas_ buffer */
  index_input::ptr pos_in_;
  features features_; /* field features */
 
 private:
  friend class doc_iterator;

  static pos_iterator::ptr make(const features& enabled);
}; // pos_iterator

class offs_pay_iterator final : public pos_iterator {
 public:  
  DECLARE_PTR( offs_pay_iterator );

  offs_pay_iterator():
    pos_iterator(2) { // offset + payload
    auto& attrs = this->attributes();
    offs_ = attrs.add<offset>();
    pay_ = attrs.add<payload>();
  }

  virtual void clear() override {
    pos_iterator::clear();
    offs_->clear();
    pay_->clear();
  }
  
 protected:
  virtual void prepare(const doc_state& state) override {
    pos_iterator::prepare(state);
    pay_in_ = state.pay_in->clone();
    pay_in_->seek(state.term_state->pay_start);
  }

  virtual void prepare(const skip_state& state) override {
    pos_iterator::prepare(state);
    pay_in_->seek(state.pay_ptr);
    pay_data_pos_ = state.pay_pos;
  }

  virtual void read_attributes() override {
    offs_->start += offs_start_deltas_[buf_pos_];
    offs_->end = offs_->start + offs_lengts_[buf_pos_];

    pay_->value = bytes_ref(
      pay_data_.c_str() + pay_data_pos_,
      pay_lengths_[buf_pos_]);
    pay_data_pos_ += pay_lengths_[buf_pos_];
  }

  virtual void skip(uint64_t count) override {
    uint64_t left = postings_writer::BLOCK_SIZE - buf_pos_;
    if (count >= left) {
      count -= left;
      // skip block by block
      while (count >= postings_writer::BLOCK_SIZE) {
        skip_positions(*pos_in_, enc_buf_);
        skip_payload(*pay_in_, enc_buf_);
        skip_offsets(*pay_in_, enc_buf_);
        count -= postings_writer::BLOCK_SIZE;
      }
      refill();
      buf_pos_ = 0;
      left = postings_writer::BLOCK_SIZE;
    }

    if (count < left) {
      // current payload start
      const auto begin = pay_lengths_ + buf_pos_;
      const auto end = begin + count;
      pay_data_pos_ = std::accumulate(begin, end, pay_data_pos_);
      buf_pos_ += uint32_t(count);
    }
    clear();
    value_ = 0;
  }

  virtual void refill() override {
    if (pos_in_->file_pointer() == tail_start_) {
      size_t pos = 0;

      for (size_t i = 0; i < tail_length_; ++i) {
        // read payloads
        if (shift_unpack_32(pos_in_->read_vint(), pos_deltas_[i])) {
          pay_lengths_[i] = pos_in_->read_vint();
        } else {
          assert(i);
          pay_lengths_[i] = pay_lengths_[i-1];
        }

        if (pay_lengths_[i]) {
          const auto size = pay_lengths_[i]; // length of current payload

          oversize(pay_data_, pos + size);

          #ifdef IRESEARCH_DEBUG
            const auto read = pos_in_->read_bytes(&(pay_data_[0]) + pos, size);
            assert(read == size);
          #else
            pos_in_->read_bytes(&(pay_data_[0]) + pos, size);
          #endif // IRESEARCH_DEBUG

          pos += size;
        }

        if (shift_unpack_32(pos_in_->read_vint(), offs_start_deltas_[i])) {
          offs_lengts_[i] = pos_in_->read_vint();
        } else {
          assert(i);
          offs_lengts_[i] = offs_lengts_[i - 1];
        }
      }
    } else {
      read_block(*pos_in_, postings_writer::BLOCK_SIZE, enc_buf_, pos_deltas_);

      // read payloads
      const uint32_t size = pay_in_->read_vint();
      if (size) {
        read_block(*pay_in_, postings_writer::BLOCK_SIZE, enc_buf_, pay_lengths_);
        oversize(pay_data_, size);

        #ifdef IRESEARCH_DEBUG
          const auto read = pay_in_->read_bytes(&(pay_data_[0]), size);
          assert(read == size);
        #else
          pay_in_->read_bytes(&(pay_data_[0]), size);
        #endif // IRESEARCH_DEBUG
      }

      // read offsets
      read_block(*pay_in_, postings_writer::BLOCK_SIZE, enc_buf_, offs_start_deltas_);
      read_block(*pay_in_, postings_writer::BLOCK_SIZE, enc_buf_, offs_lengts_);
    }
    pay_data_pos_ = 0;
  }

  index_input::ptr pay_in_;
  offset* offs_;
  payload* pay_;
  uint32_t offs_start_deltas_[postings_writer::BLOCK_SIZE]{}; /* buffer to store offset starts */
  uint32_t offs_lengts_[postings_writer::BLOCK_SIZE]{}; /* buffer to store offset lengths */
  uint32_t pay_lengths_[postings_writer::BLOCK_SIZE]{}; /* buffer to store payload lengths */
  size_t pay_data_pos_{}; /* current position in a payload buffer */
  bstring pay_data_; // buffer to store payload data
}; // pay_offs_iterator

class offs_iterator final : public pos_iterator {
 public:
  DECLARE_PTR(offs_iterator);

  offs_iterator():
    pos_iterator(1) { // offset
    auto& attrs = this->attributes();
    offs_ = attrs.add<offset>();
  }
  
  virtual void clear() override {
    pos_iterator::clear();
    offs_->clear();
  }
  
 protected:
  virtual void prepare(const doc_state& state) override {
    pos_iterator::prepare(state);
    pay_in_ = state.pay_in->clone();
    pay_in_->seek(state.term_state->pay_start);
  }
  
  virtual void prepare(const skip_state& state) override {
    pos_iterator::prepare(state);
    pay_in_->seek(state.pay_ptr);
  }

  virtual void read_attributes() override {
    offs_->start += offs_start_deltas_[buf_pos_];
    offs_->end = offs_->start + offs_lengts_[buf_pos_];
  }

  virtual void refill() override {
    if (pos_in_->file_pointer() == tail_start_) {
      uint32_t pay_size = 0;
      for (uint64_t i = 0; i < tail_length_; ++i) {
        /* skip payloads */
        if (features_.payload()) {
          if (shift_unpack_32(pos_in_->read_vint(), pos_deltas_[i])) {
            pay_size = pos_in_->read_vint();
          }
          if (pay_size) {
            pos_in_->seek(pos_in_->file_pointer() + pay_size);
          }
        } else {
          pos_deltas_[i] = pos_in_->read_vint();
        }

        /* read offsets */
        if (shift_unpack_32(pos_in_->read_vint(), offs_start_deltas_[i])) {
          offs_lengts_[i] = pos_in_->read_vint();
        } else {
          assert(i);
          offs_lengts_[i] = offs_lengts_[i - 1];
        }
      }
    } else {
      read_block(*pos_in_, postings_writer::BLOCK_SIZE, enc_buf_, pos_deltas_);

      // skip payload
      if (features_.payload()) {
        skip_payload(*pay_in_, enc_buf_);
      }

      // read offsets
      read_block(*pay_in_, postings_writer::BLOCK_SIZE, enc_buf_, offs_start_deltas_);
      read_block(*pay_in_, postings_writer::BLOCK_SIZE, enc_buf_, offs_lengts_);
    }
  }

  virtual void skip(uint64_t count) override {
    uint64_t left = postings_writer::BLOCK_SIZE - buf_pos_;
    if (count >= left) {
      count -= left;
      // skip block by block
      while (count >= postings_writer::BLOCK_SIZE) {
        skip_positions(*pos_in_, enc_buf_);
        if (features_.payload()) {
          skip_payload(*pay_in_, enc_buf_);
        }
        skip_offsets(*pay_in_, enc_buf_);
        count -= postings_writer::BLOCK_SIZE;
      }
      refill();
      buf_pos_ = 0;
      left = postings_writer::BLOCK_SIZE;
    }

    if (count < left) {
      buf_pos_ += uint32_t(count);
    }
    clear();
    value_ = 0;
  }

  index_input::ptr pay_in_;
  offset* offs_;
  uint32_t offs_start_deltas_[postings_writer::BLOCK_SIZE]; /* buffer to store offset starts */
  uint32_t offs_lengts_[postings_writer::BLOCK_SIZE]; /* buffer to store offset lengths */
}; // offs_iterator

class pay_iterator final : public pos_iterator {
 public:
  DECLARE_PTR(pay_iterator);

  pay_iterator():
    pos_iterator(1) { // payload
    auto& attrs = this->attributes();
    pay_ = attrs.add<payload>();
  }
  
  virtual void clear() override {
    pos_iterator::clear();
    pay_->clear();
  }
  
 protected:
  virtual void prepare(const doc_state& state) override {
    pos_iterator::prepare(state);
    pay_in_ = state.pay_in->clone();
    pay_in_->seek(state.term_state->pay_start);
  }

  virtual void prepare(const skip_state& state) override {
    pos_iterator::prepare(state);
    pay_in_->seek(state.pay_ptr);
    pay_data_pos_ = state.pay_pos;
  }

  virtual void read_attributes() override {
    pay_->value = bytes_ref( pay_data_.data() + pay_data_pos_,
                             pay_lengths_[buf_pos_] );
    pay_data_pos_ += pay_lengths_[buf_pos_];
  }

  virtual void skip(uint64_t count) override {
    uint64_t left = postings_writer::BLOCK_SIZE - buf_pos_;
    if (count >= left) {
      count -= left;
      // skip block by block
      while (count >= postings_writer::BLOCK_SIZE) {
        skip_positions(*pos_in_, enc_buf_);
        skip_payload(*pay_in_, enc_buf_);
        if (features_.offset()) {
          skip_offsets(*pay_in_, enc_buf_);
        }
        count -= postings_writer::BLOCK_SIZE;
      }
      refill();
      buf_pos_ = 0;
      left = postings_writer::BLOCK_SIZE;
    }

    if (count < left) {
      // current payload start 
      const auto begin = pay_lengths_ + buf_pos_;
      const auto end = begin + count;
      pay_data_pos_ = std::accumulate(begin, end, pay_data_pos_);
      buf_pos_ += uint32_t(count);
    }
    clear();
    value_ = 0;
  }

  virtual void refill() override {
    if (pos_in_->file_pointer() == tail_start_) {
      size_t pos = 0;

      for (uint64_t i = 0; i < tail_length_; ++i) {
        // read payloads
        if (shift_unpack_32(pos_in_->read_vint(), pos_deltas_[i])) {
          pay_lengths_[i] = pos_in_->read_vint();
        } else {
          assert(i);
          pay_lengths_[i] = pay_lengths_[i-1];
        }

        if (pay_lengths_[i]) {
          const auto size = pay_lengths_[i]; // current payload length

          oversize(pay_data_, pos + size);

          #ifdef IRESEARCH_DEBUG
            const auto read = pos_in_->read_bytes(&(pay_data_[0]) + pos, size);
            assert(read == size);
          #else
            pos_in_->read_bytes(&(pay_data_[0]) + pos, size);
          #endif // IRESEARCH_DEBUG

          pos += size;
        }

        // skip offsets
        if (features_.offset()) {
          uint32_t code;
          if (shift_unpack_32(pos_in_->read_vint(), code)) {
            pos_in_->read_vint();
          }
        }
      }
    } else {
      read_block(*pos_in_, postings_writer::BLOCK_SIZE, enc_buf_, pos_deltas_);

      /* read payloads */
      const uint32_t size = pay_in_->read_vint();
      if (size) {
        read_block(*pay_in_, postings_writer::BLOCK_SIZE, enc_buf_, pay_lengths_);
        oversize(pay_data_, size);

        #ifdef IRESEARCH_DEBUG
          const auto read = pay_in_->read_bytes(&(pay_data_[0]), size);
          assert(read == size);
        #else
          pay_in_->read_bytes(&(pay_data_[0]), size);
        #endif // IRESEARCH_DEBUG
      }

      // skip offsets
      if (features_.offset()) {
        skip_offsets(*pay_in_, enc_buf_);
      }
    }
    pay_data_pos_ = 0;
  }

  index_input::ptr pay_in_;
  payload* pay_;
  uint32_t pay_lengths_[postings_writer::BLOCK_SIZE]{}; /* buffer to store payload lengths */
  uint64_t pay_data_pos_{}; /* current postition in payload buffer */
  bstring pay_data_; // buffer to store payload data
}; // pay_iterator
  
/* static */ pos_iterator::ptr pos_iterator::make(const features& enabled) {
  switch (enabled) {
    case features::POS : 
      return pos_iterator::ptr(new pos_iterator()); 
    case features::POS_OFFS :
      return pos_iterator::ptr(new offs_iterator());
    case features::POS_PAY :
      return pos_iterator::ptr(new pay_iterator());
    case features::POS_OFFS_PAY :
      return pos_iterator::ptr(new offs_pay_iterator());
  }
  
  assert(false);
  return nullptr;
}

void doc_iterator::prepare( const flags& field,
                            const iresearch::attributes& attrs,
                            const flags& req,
                            const index_input* doc_in,
                            const index_input* pos_in,
                            const index_input* pay_in ) {
  features_ = features(field);

  // add mandatory attributes
  doc_ = attrs_.add<document>();

  // get state attribute
  assert(attrs.contains<version10::term_meta>());
  term_state_ = *attrs.get<version10::term_meta>();

  // init document stream
  if (term_state_.docs_count > 1) {
    if (!doc_in_) {
      doc_in_ = doc_in->clone();
    }
    doc_in_->seek(term_state_.doc_start);
  }

  // get enabled features:
  // find intersection between requested
  // and available features
  const features enabled = features(field & req);

  // term frequency attributes
  if (enabled.freq()) {
    assert(attrs.contains<frequency>());
    freq_ = attrs_.add<frequency>();
    term_freq_ = attrs.get<frequency>()->value;

    // position attribute 
    if (enabled.position()) {
      pos_iterator::ptr it = pos_iterator::make(enabled);

      doc_state state;
      state.pos_in = pos_in;
      state.pay_in = pay_in;
      state.term_state = &term_state_;
      state.freq = &freq_->value;
      state.features = features_;
      state.enc_buf = enc_buf_;
      if (term_freq_ < postings_writer::BLOCK_SIZE) {
        state.tail_start = term_state_.pos_start;
      } else if (term_freq_ == postings_writer::BLOCK_SIZE) {
        state.tail_start = type_limits<type_t::address_t>::invalid();
      } else {
        state.tail_start = term_state_.pos_start + term_state_.pos_end;
      }
      state.tail_length = term_freq_ % postings_writer::BLOCK_SIZE;
      it->prepare(state);

      // finish initialization
      position* pos = attrs_.add<position>();
      pos->prepare(pos_ = it.release());
    }
  }
}

/* inline */ void doc_iterator::refresh() {
  if (freq_) {
    freq_->value = *doc_freq_;

    if (pos_) {
      pos_->pend_pos_ += freq_->value;
      pos_->clear();
    }
  }
}

void doc_iterator::seek_to_block(doc_id_t target) {
  // check whether it make sense to use skip-list
  if (skip_levels_.front().doc < target && term_state_.docs_count > postings_writer::BLOCK_SIZE) {
    skip_state last; // where block starts
    size_t last_level = 0;

    // init skip writer in lazy fashion
    if (!skip_) {
      index_input::ptr skip_in = doc_in_->clone();
      skip_in->seek(term_state_.doc_start + term_state_.e_skip_start);

      skip_.prepare(
        std::move(skip_in),
        [&last, &last_level, this](size_t level, index_input& in) {
          auto& next = skip_levels_[level];

          if (last_level > level) {
            // move to the more granular level
            next = last;
          } else {
            // store previous step on the same level
            last = next;
          }

          last_level = level;

          if (in.eof()) {
            // stream exhausted
            return (next.doc = type_limits<type_t::doc_id_t>::eof());
          }

          return read_skip(next, in);
      });

      // initialize skip levels
      const auto num_levels = skip_.num_levels();
      if (num_levels) {
        skip_levels_.resize(num_levels);

        // since we store pointer deltas, add postings offset 
        auto& top = skip_levels_.back();
        top.doc_ptr = term_state_.doc_start;
        top.pos_ptr = term_state_.pos_start;
        top.pay_ptr = term_state_.pay_start;
      }
    }

    const size_t skipped = skip_.seek(target);
    if (skipped > cur_pos_) {
      doc_in_->seek(last.doc_ptr);
      doc_->value = last.doc;
      cur_pos_ = skipped;
      begin_ = end_; // will trigger refill in "next"
      if (pos_) {
        // notify positions
        pos_->prepare(last);
      }
    }
  }
}

NS_END // detail

// ----------------------------------------------------------------------------
// --SECTION--                                                index_meta_writer
// ----------------------------------------------------------------------------

const string_ref index_meta_writer::FORMAT_PREFIX = "segments_";
const string_ref index_meta_writer::FORMAT_PREFIX_TMP = "pending_segments_";
const string_ref index_meta_writer::FORMAT_NAME = "iresearch_10_index_meta";

template<>
std::string file_name<index_meta_reader, index_meta>(const index_meta& meta) {
  return file_name(index_meta_writer::FORMAT_PREFIX, meta.generation());
};

template<>
std::string file_name<index_meta_writer, index_meta>(const index_meta& meta) {
  return file_name(index_meta_writer::FORMAT_PREFIX_TMP, meta.generation());
};

std::string index_meta_writer::filename(const index_meta& meta) const {
  return file_name<index_meta_reader>(meta);
}

bool index_meta_writer::prepare(directory& dir, index_meta& meta) {
  if (meta_) {
    // prepare() was already called with no corresponding call to commit()
    return false;
  }

  prepare(meta); // prepare meta before generating filename

  auto seg_file = file_name<index_meta_writer>(meta);

  try {
    {
      index_output::ptr out = dir.create(seg_file);

      format_utils::write_header(*out, FORMAT_NAME, FORMAT_MAX);
      out->write_vlong(meta.generation());
      out->write_long(meta.counter());
      assert(meta.size() <= integer_traits<uint32_t>::const_max);
      out->write_vint(uint32_t(meta.size()));

      for (auto& segment : meta) {
        write_string(*out, segment.filename);
        write_string(*out, segment.meta.codec->type().name());
      }

      format_utils::write_footer(*out);
    } // important to close output here

    dir.sync(seg_file);
  } catch (const io_error& e) {
    IR_ERROR() << "Caught i/o error, reason: " << e.what();
    return false;
  }

  dir_ = &dir;
  meta_ = &meta;

  return true;
}

void index_meta_writer::commit() {
  if (!meta_) {
    return;
  }

  auto src = file_name<index_meta_writer>(*meta_);
  auto dst = file_name<index_meta_reader>(*meta_);

  try {
    auto clear_pending = make_finally([this]{ meta_ = nullptr; });

    dir_->rename(src, dst);
    complete(*meta_);
    dir_ = nullptr;
  } catch ( ... ) {
    rollback();
    throw;
  }
}

void index_meta_writer::rollback() NOEXCEPT {
  if (!meta_) {
    return;
  }

  auto seg_file = file_name<index_meta_writer>(*meta_);

  try {
    dir_->remove(seg_file);
  } catch (...) {
    // suppress all errors 
  }

  dir_ = nullptr;
  meta_ = nullptr;
}

// ----------------------------------------------------------------------------
// --SECTION--                                                index_meta_reader
// ----------------------------------------------------------------------------

uint64_t parse_generation(const std::string& segments_file) {
  assert(iresearch::starts_with(segments_file, index_meta_writer::FORMAT_PREFIX));

  const char* gen_str = segments_file.c_str() + index_meta_writer::FORMAT_PREFIX.size();
  char* suffix;
  auto gen = std::strtoull(gen_str, &suffix, 10); // 10 for base-10

  return suffix[0] ? index_meta::INVALID_GEN : gen;
}

bool index_meta_reader::last_segments_file(const directory& dir, std::string& out) const {
  uint64_t max_gen = 0;
  directory::visitor_f visitor = [&out, &max_gen] (std::string& name) {
    if (iresearch::starts_with(name, index_meta_writer::FORMAT_PREFIX)) {
      const uint64_t gen = parse_generation(name);
      if (gen != index_meta::INVALID_GEN && gen > max_gen) {
        out = std::move(name);
        max_gen = gen;
      }
    }
    return true; // continue iteration
  };

  dir.visit(visitor);
  return max_gen > 0;
}

void index_meta_reader::read(
    const directory& dir,
    index_meta& meta,
    const string_ref& filename /*= string_ref::nil*/) {

  const std::string meta_file = filename.null()
    ? file_name<index_meta_reader>(meta)
    : static_cast<std::string>(filename);

  checksum_index_input<boost::crc_32_type> in(dir.open(meta_file));

  // check header
  format_utils::check_header(
    in,
    index_meta_writer::FORMAT_NAME,
    index_meta_writer::FORMAT_MIN,
    index_meta_writer::FORMAT_MAX
  );

  // read data from segments file
  auto gen = in.read_vlong();
  auto cnt = in.read_long();
  auto seg_count = in.read_vint();
  index_meta::index_segments_t segments(seg_count);

  for (size_t i = 0, count = segments.size(); i < count; ++i) {
    auto& segment = segments[i];

    segment.filename = read_string<std::string>(in);
    segment.meta.codec = formats::get(read_string<std::string>(in));

    auto reader = segment.meta.codec->get_segment_meta_reader();

    reader->read(dir, segment.meta, segment.filename);
  }

  format_utils::check_footer(in);
  complete(meta, gen, cnt, std::move(segments));
}

// ----------------------------------------------------------------------------
// --SECTION--                                              segment_meta_writer 
// ----------------------------------------------------------------------------

const string_ref segment_meta_writer::FORMAT_EXT = "sm";
const string_ref segment_meta_writer::FORMAT_NAME = "iresearch_10_segment_meta";

template<>
std::string file_name<segment_meta_writer, segment_meta>(const segment_meta& meta) {
  return iresearch::file_name(meta.name, meta.version, segment_meta_writer::FORMAT_EXT);
};

std::string segment_meta_writer::filename(const segment_meta& meta) const {
  return file_name<segment_meta_writer>(meta);
}

void segment_meta_writer::write(directory& dir, const segment_meta& meta) {
  auto meta_file = file_name<segment_meta_writer>(meta);
  index_output::ptr out(dir.create(meta_file));

  format_utils::write_header(*out, FORMAT_NAME, FORMAT_MAX);
  write_string(*out, meta.name);
  out->write_vlong(meta.version);
  out->write_vlong( meta.docs_count);
  write_strings( *out, meta.files );
  format_utils::write_footer(*out);
}

// ----------------------------------------------------------------------------
// --SECTION--                                              segment_meta_reader
// ----------------------------------------------------------------------------

void segment_meta_reader::read(
    const directory& dir,
    segment_meta& meta,
    const string_ref& filename /*= string_ref::nil*/) {

  const std::string meta_file = filename.null()
    ? file_name<segment_meta_writer>(meta)
    : static_cast<std::string>(filename);

  checksum_index_input<boost::crc_32_type> in(dir.open(meta_file));

  format_utils::check_header(
    in,
    segment_meta_writer::FORMAT_NAME,
    segment_meta_writer::FORMAT_MIN,
    segment_meta_writer::FORMAT_MAX
  );

  auto name = read_string<std::string>(in);
  auto version = in.read_vlong();
  int64_t count = in.read_vlong();
  if ( count < 0 ) {
    // corrupted index
    throw index_error();
  }

  meta.name = std::move(name);
  meta.version = version;
  meta.docs_count = count;
  meta.files = read_strings<segment_meta::file_set>( in );

  format_utils::check_footer(in);
}

// ----------------------------------------------------------------------------
// --SECTION--                                             document_mask_writer 
// ----------------------------------------------------------------------------

const string_ref document_mask_writer::FORMAT_NAME = "iresearch_10_doc_mask";
const string_ref document_mask_writer::FORMAT_EXT = "doc_mask";

template<>
std::string file_name<document_mask_writer, segment_meta>(segment_meta const& meta) {
  return iresearch::file_name(meta.name, meta.version, document_mask_writer::FORMAT_EXT);
};

document_mask_writer::~document_mask_writer() {}

std::string document_mask_writer::filename(const segment_meta& meta) const {
  return file_name<document_mask_writer>(meta);
}

void document_mask_writer::prepare(directory& dir, segment_meta const& meta) {
  out_ = dir.create(file_name<document_mask_writer>(meta));
}

void document_mask_writer::begin(uint32_t count) {
  format_utils::write_header(*out_, FORMAT_NAME, FORMAT_MAX);
  out_->write_vint(count);
}

void document_mask_writer::write(const doc_id_t& mask) {
  out_->write_vlong(mask);
}

void document_mask_writer::end() {
  format_utils::write_footer(*out_);
}

// ----------------------------------------------------------------------------
// --SECTION--                                             document_mask_reader 
// ----------------------------------------------------------------------------

document_mask_reader::~document_mask_reader() {}

bool document_mask_reader::prepare(directory const& dir, segment_meta const& meta) {
  auto in_name = file_name<document_mask_writer>(meta);

  if (!dir.exists(in_name)) {
    checksum_index_input<boost::crc_32_type> empty_in;

    in_.swap(empty_in);

    return false;
  }

  checksum_index_input<boost::crc_32_type> check_in(dir.open(in_name));

  in_.swap(check_in);

  return true;
}

uint32_t document_mask_reader::begin() {
  format_utils::check_header(
    in_,
    document_mask_writer::FORMAT_NAME,
    document_mask_writer::FORMAT_MIN,
    document_mask_writer::FORMAT_MAX
  );

  return in_.read_vint();
}

void document_mask_reader::read(doc_id_t& doc_id) {
  auto id = in_.read_vlong();

  static_assert(sizeof(doc_id_t) == sizeof(decltype(id)), "sizeof(doc_id) != sizeof(decltype(id))");
  doc_id = id;
}

void document_mask_reader::end() {
  format_utils::check_footer(in_);
}

// ----------------------------------------------------------------------------
// --SECTION--                                                field_meta_reader 
// ----------------------------------------------------------------------------

field_meta_reader::~field_meta_reader() {}

void field_meta_reader::read_segment_features() {
  feature_map_.clear();
  feature_map_.reserve(in.read_vlong());
  
  size_t count = feature_map_.capacity();
  while (count) {
    const auto name = read_string<std::string>(in); // read feature name
    const attribute::type_id* feature = attribute::type_id::get(name);
    if (feature) {
      feature_map_.emplace_back(feature);
    } else {
      IR_ERROR() << "unknown feature name '" << name << "'";
    }
    --count;
  }
}

void field_meta_reader::read_field_features(flags& features) {
  size_t count = in.read_vlong();
  while (count) {
    const size_t id = in.read_vlong(); // feature id
    if (id < feature_map_.size()) {
      features.add(*feature_map_[id]);
    } else {
      IR_ERROR() << "unknown feature id '" << id << "'";
    }
    --count;
  }
}

void field_meta_reader::prepare(const directory& dir, const string_ref& seg_name) {
  assert(!seg_name.null());

  checksum_index_input< boost::crc_32_type > check_in(
    dir.open(file_name(seg_name, field_meta_writer::FORMAT_EXT))
  );

  in.swap(check_in);
}

size_t field_meta_reader::begin() {
  format_utils::check_header(
    in,
    field_meta_writer::FORMAT_NAME,
    field_meta_writer::FORMAT_MIN,
    field_meta_writer::FORMAT_MAX
  );

  read_segment_features();
  return in.read_vint();
}

void field_meta_reader::read(iresearch::field_meta& meta) {
  meta.name = read_string<std::string>(in);

  const field_id id = in.read_vint();
  if (!type_limits<type_t::field_id_t>::valid(id)) {
    // corrupted index
    throw index_error();
  }

  meta.id = id;
  read_field_features(meta.features);
  meta.norm = static_cast<field_id>(read_zvlong(in));
}

void field_meta_reader::end() {
  format_utils::check_footer(in);
}

// ----------------------------------------------------------------------------
// --SECTION--                                                field_meta_writer
// ----------------------------------------------------------------------------

const string_ref field_meta_writer::FORMAT_NAME = "iresearch_10_field_meta";
const string_ref field_meta_writer::FORMAT_EXT = "fm";

field_meta_writer::~field_meta_writer() {}

void field_meta_writer::write_segment_features(const flags& features) {
  out->write_vlong(features.size());
  feature_map_.clear();
  feature_map_.reserve(features.size());
  for (const attribute::type_id* feature : features) {
    write_string(*out, feature->name());
    feature_map_.emplace(feature, feature_map_.size());
  }
}

void field_meta_writer::write_field_features(const flags& features) const {
  out->write_vlong(features.size());
  for (auto feature : features) {
    auto it = feature_map_.find(*feature);
    assert(it != feature_map_.end());
    out->write_vlong(it->second);
  }
}

void field_meta_writer::prepare(const flush_state& state) {
  assert(!state.name.null());
  assert(state.features);
  assert(state.dir);

  feature_map_.clear();
  out = state.dir->create(file_name(state.name, FORMAT_EXT));
  format_utils::write_header(*out, FORMAT_NAME, FORMAT_MAX);
  write_segment_features(*state.features);
  out->write_vint(static_cast<uint32_t>(state.fields_count));
}

void field_meta_writer::write(
    field_id id, 
    const std::string& name, 
    const flags& features,
    field_id norm) {
  write_string(*out, name);
  out->write_vint(static_cast<uint32_t>(id));
  write_field_features(features);
  write_zvlong(*out, norm);
}

void field_meta_writer::end() {
  format_utils::write_footer(*out);
  out.reset(); // ensure stream is closed
}

NS_BEGIN(columns)

class meta_writer final : public iresearch::column_meta_writer {
 public:
  static const string_ref FORMAT_NAME;
  static const string_ref FORMAT_EXT;

  static const int32_t FORMAT_MIN = 0;
  static const int32_t FORMAT_MAX = FORMAT_MIN;

  virtual bool prepare(directory& dir, const string_ref& filename) override;
  virtual void write(const std::string& name, field_id id) override;
  virtual void flush() override;

 private:
  index_output::ptr out_;
}; // meta_writer 

const string_ref meta_writer::FORMAT_NAME = "iresearch_10_columnmeta";
const string_ref meta_writer::FORMAT_EXT = "cm";

bool meta_writer::prepare(directory& dir, const string_ref& name) {
  std::string filename;
  file_name(filename, name, FORMAT_EXT);

  out_ = dir.create(filename);
  format_utils::write_header(*out_, FORMAT_NAME, FORMAT_MAX);

  return true;
}
  
void meta_writer::write(const std::string& name, field_id id) {
  write_zvint(*out_, id);
  write_string(*out_, name);
}

void meta_writer::flush() {
  write_zvint(*out_, type_limits<type_t::field_id_t>::invalid());
  format_utils::write_footer(*out_);
  out_.reset();
}

class meta_reader final : public iresearch::column_meta_reader {
 public:
  virtual bool prepare(const directory& dir, const string_ref& filename) override;
  virtual bool read(column_meta& column) override;

 private:
  checksum_index_input<boost::crc_32_type> in_;
}; // meta_writer 

bool meta_reader::prepare(const directory& dir, const string_ref& name) {
  const auto filename = file_name(name, meta_writer::FORMAT_EXT);

  if (!dir.exists(filename)) {
    // TODO: directory::open should return nullptr
    // instead of throwing exception
    return false;
  }

  checksum_index_input< boost::crc_32_type > check_in(dir.open(filename));
  
  format_utils::check_header(
    check_in, 
    meta_writer::FORMAT_NAME,
    meta_writer::FORMAT_MIN,
    meta_writer::FORMAT_MAX
  );

  in_.swap(check_in);
  return true;
}

bool meta_reader::read(column_meta& column) {
  const auto id = static_cast<field_id>(read_zvint(in_));
  if (!type_limits<type_t::field_id_t>::valid(id)) {
    format_utils::check_footer(in_);
    return false;
  }

  column.id = id;
  column.name = read_string<std::string>(in_);
  return true;
}

// ----------------------------------------------------------------------------
// --SECTION--                                                 Format constants 
// ----------------------------------------------------------------------------

// |Header|
// |Compressed block #0|
// |Compressed block #1|
// |Compressed block #2|
// |Compressed block #1| <-- Columnstore data blocks
// |Compressed block #1|
// |Compressed block #1|
// |Compressed block #2|
// ...
// |Bloom Filter|
// |Last block #0 key|Block #0 offset|
// |Last block #1 key|Block #1 offset| <-- Columnstore blocks index
// |Last block #2 key|Block #2 offset|
// ...
// |Bloom filter offset|
// |Footer|

const size_t INDEX_BLOCK_SIZE = 1 << 10;
const size_t MAX_DATA_BLOCK_SIZE = 1 << 12;
const size_t DATA_BLOCK_SIZE = 1 << 13;

//////////////////////////////////////////////////////////////////////////////
/// @class writer 
//////////////////////////////////////////////////////////////////////////////
class writer final : public iresearch::columnstore_writer {
 public:
  static const int32_t FORMAT_MIN = 0;
  static const int32_t FORMAT_MAX = FORMAT_MIN;
  
  static const string_ref FORMAT_NAME;
  static const string_ref FORMAT_EXT;

  virtual bool prepare(directory& dir, const string_ref& name) override;
  virtual column_t push_column() override;
  virtual void flush() override;

 private:
  struct column : util::noncopyable {
     explicit column() {
       blocks_index_writer.prepare(blocks_index.stream);
       block_header_writer.prepare(block_header.stream);
     }

     memory_output blocks_index; // blocks index
     memory_output block_header; // current block header
     compressing_index_writer block_header_writer{ INDEX_BLOCK_SIZE };
     compressing_index_writer blocks_index_writer{ INDEX_BLOCK_SIZE };
     bytes_output block_buf{ MAX_DATA_BLOCK_SIZE }; // current block data buffer
     doc_id_t begin{ type_limits<type_t::doc_id_t>::invalid() }; // current block first key
     doc_id_t end{ type_limits<type_t::doc_id_t>::invalid() }; // current column max key
     bool empty{ true }; // index empty
     bool block_empty{ true }; // block empty
   };

   void flush_block(column& col);

   std::deque<column> columns_; // pointers remain valid
   compressor comp_{ MAX_DATA_BLOCK_SIZE };
   index_output::ptr data_out_;
   std::string filename_;
   directory* dir_;
}; // writer

const string_ref writer::FORMAT_NAME = "iresearch_10_columnstore";
const string_ref writer::FORMAT_EXT = "cs";

void writer::flush_block(column& col) {
  // where block starts
  const auto block_offset = data_out_->file_pointer();

  // write block header 
  auto& stream = col.block_header.stream;
  auto& writer = col.block_header_writer;

  writer.finish();
  stream.flush();
  col.block_header.file >> *data_out_;

  // reset writer & stream
  writer.prepare(stream);
  stream.reset();

  // write compressed block data
  auto& block_buf = col.block_buf;
  if (!block_buf.empty()) {
    write_zvlong(*data_out_, block_buf.size() - MAX_DATA_BLOCK_SIZE);
    write_compact(*data_out_, comp_, block_buf.c_str(), block_buf.size());
    block_buf.reset(); // reset buffer stream after flushing

    // write last block key & where block starts
    col.blocks_index_writer.write(col.begin, block_offset);
  }

  col.begin = type_limits<type_t::doc_id_t>::eof();
  col.block_empty = true;
}

bool writer::prepare(directory& dir, const string_ref& name) {
  columns_.clear();

  dir_ = &dir;
  file_name(filename_, name, FORMAT_EXT);

  data_out_ = dir.create(filename_);
  format_utils::write_header(*data_out_, FORMAT_NAME, FORMAT_MAX);

  return true;
}

columnstore_writer::column_t writer::push_column() {
  columns_.emplace_back();
  auto& column = columns_.back();

  return std::make_pair(
    columns_.size() - 1, // field id
    [&column, this] (doc_id_t doc, const serializer& writer) {
      assert(column.empty || doc >= column.end);

      auto& stream = column.block_buf;
      auto& block_header = column.block_header_writer;

      const auto ptr = stream.size();
      if (!writer.write(stream)) {
        // nothing has been written, reset to previous
        stream.reset(ptr);
        return false;
      }

      // prevent values from the same document 
      // to overcome block boundaries
      if (column.end != doc || column.empty) {
        if (column.block_empty) {
          column.begin = doc;
          column.block_empty = false;
        }

        block_header.write(column.end = doc, ptr);
        column.empty = false;
        if (stream.size() >= DATA_BLOCK_SIZE) {
          flush_block(column);
        }
      }

      return true;
  });
}

void writer::flush() {
  // remove all empty columns from tail
  while (!columns_.empty()) {
    auto& column = columns_.back();
    if (column.empty) {
      columns_.pop_back();
    } else {
      break;
    }
  }
  
  // remove file if there is no data to write
  if (columns_.empty()) {
    data_out_.reset();
    dir_->remove(filename_);
    return;
  }
  
  // flush all remain data including possible empty columns among filled columns
  for (auto& column : columns_) {
    // flush remain blocks
    flush_block(column);

    // finish column blocks index
    column.blocks_index_writer.finish();
    column.blocks_index.stream.flush();
  }

  const auto block_index_ptr = data_out_->file_pointer(); // where blocks index start
  data_out_->write_vlong(columns_.size()); // number of columns
  for (auto& column : columns_) {
    data_out_->write_vlong(column.end); // max key
    column.blocks_index.file >> *data_out_; // column blocks index
  }
  data_out_->write_long(block_index_ptr);
  format_utils::write_footer(*data_out_);
  
  data_out_.reset();
}

//////////////////////////////////////////////////////////////////////////////
/// @class reader 
//////////////////////////////////////////////////////////////////////////////

NS_LOCAL

iresearch::columnstore_reader::values_reader_f INVALID_COLUMN = 
  [] (doc_id_t, const iresearch::columnstore_reader::value_reader_f&) {
    return false;
  };

NS_END

class reader final : public iresearch::columnstore_reader, util::noncopyable {
 public:
  virtual bool prepare(const reader_state& state) override;
  virtual values_reader_f values(field_id field) const override;
  virtual bool visit(field_id field, const raw_reader_f& reader) const override;
  
 private:
  typedef compressed_index<uint64_t> block_index_t;
  
  struct block : util::noncopyable {
    block() = default;
    block(block&& other)
      : index(std::move(other.index)),
        data(std::move(other.data)) {
    }

    compressed_index<uint64_t> index; // block index
    bstring data; // decompressed data block
  };
  
  typedef std::pair<
    uint64_t, // block data offset
    block* // pointer to cached block
  > block_ref_t;
  
  typedef compressed_index<block_ref_t> blocks_index_t;
  
  struct read_context {
    std::deque<block> cached_blocks;
    bstring encode_buf; // 'read_compact' requires a temporary buffer
    decompressor decomp; // decompressor
    index_input::ptr stream; // input stream
    std::mutex lock;
  };

  static bool read_block(
    index_input& in, 
    read_context& ctx,
    block& block
  );
  
  // visits block entries
  static bool visit(const block& block, const raw_reader_f& reader, bytes_ref_input& stream);

  // reset value stream
  static void reset(
    bytes_ref_input& stream,
    const block& block,
    const block_index_t::iterator& vbegin, // where value starts
    const block_index_t::iterator& vend, // where value ends
    const block_index_t::iterator& end); // where index ends
  
  mutable read_context ctx_;
  std::vector<blocks_index_t> columns_;
}; // reader
  
/*static*/ bool reader::read_block(
    index_input& in, 
    read_context& ctx,
    block& block) {
  // read block index
  static auto visitor = [] (uint64_t& target, uint64_t value) {
    target = value;
  };
  
  if (!block.index.read(in, type_limits<type_t::doc_id_t>::eof(), visitor)) {
    // unable to read block index
    return false;
  }
  
  // ensure we have enough space to store uncompressed data
  block.data.resize(MAX_DATA_BLOCK_SIZE + read_zvlong(in));

  // read block data
  read_compact(in, ctx.decomp, ctx.encode_buf, block.data); 
  
  return true;
}

bool reader::prepare(const reader_state& state) {
  auto& name = state.meta->name;
  auto& dir = *state.dir;

  std::string filename;
  file_name(filename, name, writer::FORMAT_EXT);
  if (!dir.exists(filename)) {
    // TODO: directory::open should return nullptr
    // instead of throwing exception
    return false;
  }
  
  // open columstore stream
  auto stream = dir.open(filename);
  
  // check header
  format_utils::check_header(
    *stream, 
    writer::FORMAT_NAME, 
    writer::FORMAT_MIN, 
    writer::FORMAT_MAX
  );
  
  // since columns data are too large
  // it is too costly to verify checksum of
  // the entire file. here we perform cheap
  // error detection which could recognize
  // some forms of corruption. */
  format_utils::read_checksum(*stream);

  // seek to data start
  stream->seek(stream->length() - format_utils::FOOTER_LEN - sizeof(uint64_t));
  stream->seek(stream->read_long()); // seek to blocks index
    
  static auto visitor = [] (block_ref_t& block, uint64_t v) {
    block.first = v;
  };
   
  std::vector<blocks_index_t> columns(stream->read_vlong());
  for (size_t i = 0, size = columns.size(); i < size; ++i) {
    auto& column = columns[i];

    const auto max = stream->read_vlong(); // last valid key
    if (!column.read(*stream, max + 1, visitor)) {
      IR_ERROR() << "Unable to load blocks index for column id=" << columns.size() - 1;
      return false;
    }
  }

  // noexcept
  columns_ = std::move(columns);
  ctx_.stream = std::move(stream);

  return true;
}
  
void reader::reset(
    bytes_ref_input& stream,
    const block& block, 
    const block_index_t::iterator& vbegin, // value begin
    const block_index_t::iterator& vend,  // value end
    const block_index_t::iterator& end) { // end of the block
  const auto start_offset = vbegin->second;
  const auto end_offset = end == vend ? block.data.size() : vend->second;
  assert(end_offset >= start_offset);

  stream.reset(
    block.data.c_str() + start_offset,
    end_offset - start_offset
  );
}

reader::values_reader_f reader::values(field_id field) const {
  if (field >= columns_.size()) {
    // can't find attribute with the specified name
    return INVALID_COLUMN;
  }
  
  auto& column = columns_[field];
  bytes_ref_input block_in;

  return[this, &column, block_in] (doc_id_t doc, const value_reader_f& reader) mutable {
    const auto it = column.lower_bound(doc); // looking for data block

    if (it == column.end()) {
      // there is no block suitable for id equals to 'doc' in this column
      return false;
    }

    auto& block_ref = it->second;
    auto& cached = block_ref.second;

    if (!cached) {
      SCOPED_LOCK(ctx_.lock);
      if (!cached) {
        auto& stream = *ctx_.stream;
        stream.seek(block_ref.first);

        // add cached entry
        ctx_.cached_blocks.emplace_back();
        auto& block = ctx_.cached_blocks.back();

        if (!read_block(stream, ctx_, block)) {
          // unable to load block
          return false;
        }
        
        // mark block as loaded
        const_cast<reader::block*>(cached) = &block;
      }
    }
        
    auto& block = *block_ref.second;
    const auto vbegin = block.index.find(doc); // value begin
    const auto end = block.index.end(); // block end
        
    if (vbegin == end) {
      // there is no document with id equals to 'doc' in this block
      return false;
    }
   
    auto vend = vbegin; // value end
    std::advance(vend, 1);
    
    reset(block_in, block, vbegin, vend, end);

    return reader(block_in);
  };
}

bool reader::visit(const reader::block& block, const reader::raw_reader_f& reader, bytes_ref_input& stream) {
  auto vbegin = block.index.begin();
  auto vend = vbegin;
  const auto end = block.index.end();
  for (; vbegin != end; ++vbegin) {
    reset(stream, block, vbegin, ++vend, end);
    if (!reader(vbegin->first, stream)) {
      return false;
    }
  }

  return true;
}
  
bool reader::visit(field_id field, const raw_reader_f& reader) const {
  if (field >= columns_.size()) {
    // can't find attribute with the specified id
    return false;
  }

  auto& column = columns_[field];
  bytes_ref_input block_in;
  block_ref_t block_ref;
  reader::block block; // do not cache blocks during visiting

  for (auto& bucket : column) {
    block_ref = bucket.second;
    auto& cached = block_ref.second;

    if (!cached) {
      SCOPED_LOCK(ctx_.lock);
      if (!cached) {
        // hasn't been cached yet, use temporary block
        auto& stream = *ctx_.stream;
        stream.seek(block_ref.first);

        if (!read_block(stream, ctx_, block)) {
          return false;
        }

        block_ref.second = &block;
      }
    }

    // visit block
    if (!visit(*cached, reader, block_in)) {
      return false;
    }
  }

  return true;
}

NS_END // columns

// ----------------------------------------------------------------------------
// --SECTION--                                             stored_fields_writer 
// ----------------------------------------------------------------------------

const string_ref stored_fields_writer::FIELDS_EXT = "fd";
const string_ref stored_fields_writer::FORMAT_FIELDS = "iresearch_10_stored_fields_fields";
const string_ref stored_fields_writer::INDEX_EXT = "fx";

stored_fields_writer::stored_fields_writer(uint32_t buf_size,
                                           uint32_t block_size)
  : index(block_size),
    compres(buf_size),
    seg_buf_(buf_size),
    last_offset_(0),
    doc_base(0),
    num_buffered_docs(0),
    buf_size(buf_size),
    num_blocks_(0),
    num_incomplete_blocks_(0) {
}

void stored_fields_writer::flush() {
  index.write(doc_base, fields_out->file_pointer());

  // write header
  fields_out->write_vint(doc_base);
  fields_out->write_vint(num_buffered_docs);
  write_packed(*fields_out, bodies_, num_buffered_docs);
  write_packed(*fields_out, headers_, num_buffered_docs);

  // write compressed data
  write_compact(*fields_out, compres, seg_buf_.c_str(), seg_buf_.size());

  // determine max block size
  block_max_size_ = std::max(
    block_max_size_, 
    static_cast<uint32_t>(seg_buf_.size())
  );

  // reset
  seg_buf_.reset();
  last_offset_ = 0;
  doc_base += num_buffered_docs;
  num_buffered_docs = 0;
  ++num_blocks_;
}

void stored_fields_writer::end(const serializer* header) {
  const auto header_offset = seg_buf_.size();

  if (header_offset == last_offset_) {
    // nothing has been written
    return;
  }
  
  // write document header 
  if (header) {
    header->write(seg_buf_);
  }

  // write document header length 
  headers_[num_buffered_docs] = static_cast<uint32_t>(seg_buf_.size() - header_offset); 
  // write documen length
  bodies_[num_buffered_docs] = static_cast<uint32_t>(seg_buf_.size() - last_offset_);

  last_offset_ = seg_buf_.size();
  ++num_buffered_docs;

  if (num_buffered_docs >= MAX_BUFFERED_DOCS || last_offset_ >= buf_size) {
    flush();
  }
}

void stored_fields_writer::prepare(directory& dir, const string_ref& seg_name) {
  assert(!seg_name.null());

  std::string name;
 
  // prepare stored fields data stream
  file_name(name, seg_name, FIELDS_EXT);
  fields_out = dir.create(name);
  format_utils::write_header(*fields_out, FORMAT_FIELDS, FORMAT_MAX);
  fields_out->write_vint(packed::VERSION);

  // prepare stored fields index
  file_name(name, seg_name, INDEX_EXT);
  index_out_ = dir.create(name);  
  format_utils::write_header(
    *index_out_, 
    compressing_index_writer::FORMAT_NAME, 
    compressing_index_writer::FORMAT_MAX
  );
  index.prepare(*index_out_);
}

bool stored_fields_writer::write(const serializer& writer) {
  REGISTER_TIMER_DETAILED();
  const auto ptr = seg_buf_.size();
  if (!writer.write(seg_buf_)) {
    // reset to previous
    seg_buf_.reset(ptr);
    return false;
  }
  return true;
}

void stored_fields_writer::finish() {
  if (num_buffered_docs > 0) {
    flush();
    ++num_incomplete_blocks_;
  }
 
  // finish data stream 
  format_utils::write_footer(*fields_out);

  // finish index stream
  index.finish(); // finish index
  index_out_->write_vint(num_blocks_); // total number of blocks
  index_out_->write_vint(num_incomplete_blocks_); // total number of incomplete blocks
  index_out_->write_vint(block_max_size_); // size of the biggest block
  format_utils::write_footer(*index_out_);

  index_out_.reset();
  fields_out.reset();
}

void stored_fields_writer::reset() {
  last_offset_ = 0;
  doc_base = 0;
  num_buffered_docs = 0;
  num_blocks_ = 0;
  num_incomplete_blocks_ = 0;
  seg_buf_.reset();
}

// ----------------------------------------------------------------------------
// --SECTION--                                             stored_fields_reader 
// ----------------------------------------------------------------------------

void stored_fields_reader::compressing_document_reader::load_block(uint64_t block_ptr) {
  // seek to block start pointer
  fields_in_->seek(block_ptr);

  // read header
  base_ = fields_in_->read_vint(); // base doc_id
  size_ = fields_in_->read_vint(); // number of documents in block
  
  // read document lengths
  read_packed(*fields_in_, offsets_, size_);
  // convert lengths into offsets
  detail::delta_decode(offsets_, offsets_ + size_);

  // read header lengths
  read_packed(*fields_in_, headers_, size_);
  
  // ensure that we have enough space to store uncompressed data
  data_.resize(block_max_size_);
  
  // read data block (documents can't overcome block boundaries)
  view_ = read_compact(*fields_in_, decomp_, buf_, data_);
}

// expects 0-based doc id's
bool stored_fields_reader::compressing_document_reader::visit(
    doc_id_t doc, 
    uint64_t start_ptr, 
    const visitor_f& visitor) {

  auto doc_offset = [this](doc_id_t idx) {
    return 0 == idx ? 0 : offsets_[idx - 1];
  };

  if (!contains(doc)) {
    load_block(start_ptr); 
  }
  assert(contains(doc));

  const doc_id_t idx = doc - base_;
  const uint32_t begin = doc_offset(idx); // where document begins 
  const uint32_t end = doc_offset(idx+1); // where document ends
  const uint32_t header = headers_[idx]; // header length
  assert(end >= (begin + header));

  // reset streams
  header_.reset(view_.c_str() + end - header, header);
  body_.reset(view_.c_str() + begin, end - begin - header);

  // visit document
  while (!body_.eof()) {
    if (!visitor(header_, body_)) {
      return false;
    }
  }

  return true;
}

void stored_fields_reader::compressing_document_reader::prepare(
    const directory& dir,
    const std::string& name,
    uint32_t num_blocks, 
    uint32_t num_incomplete_blocks, 
    uint32_t block_max_size) {
  fields_in_ = dir.open(name);

  format_utils::check_header(
    *fields_in_,
    stored_fields_writer::FORMAT_FIELDS,
    stored_fields_writer::FORMAT_MIN,
    stored_fields_writer::FORMAT_MAX
  );

  const uint32_t version = fields_in_->read_vint();
  if (version != packed::VERSION) {
    // invalid packed version
    throw index_error();
  }

  // Since field values are too large
  // it is too costly to verify checksum of
  // the entire file. Here we perform cheap
  // error detection which could recognize
  // some forms of corruption
  format_utils::read_checksum(*fields_in_);
  
  num_blocks_ = num_blocks;
  num_incomplete_blocks_ = num_incomplete_blocks;
  block_max_size_ = block_max_size;
}

bool stored_fields_reader::visit(doc_id_t doc, const visitor_f& visitor) {
  const auto offset = index_.lower_bound(doc);

  if (index_.end() == offset) {
    // there is no such document with the specified id 
    return false;
  }

  return docs_.visit(doc, offset->second, visitor);
}

void stored_fields_reader::prepare( const reader_state& state ) {
  std::string buf;

  // init index reader
  file_name(buf, state.meta->name, stored_fields_writer::INDEX_EXT);
  checksum_index_input< boost::crc_32_type > in(state.dir->open(buf));
  format_utils::check_header(
    in,
    compressing_index_writer::FORMAT_NAME,
    compressing_index_writer::FORMAT_MIN,
    compressing_index_writer::FORMAT_MAX
  );

  static auto visitor = [] (uint64_t& target, uint64_t value) {
    target = value;
  };

  index_.read(in, state.meta->docs_count, visitor);
  const uint32_t num_blocks = in.read_vint(); // total # of complete blocks
  const uint32_t num_incomplete_blocks = in.read_vint(); // total # of incomplete blocks
  if (num_incomplete_blocks > num_blocks) {
    // invalid block count
    throw index_error();
  }
  const uint32_t block_max_size = in.read_vint(); // size of the biggest block
  format_utils::check_footer(in);

  // init document reader
  file_name(buf, state.meta->name, stored_fields_writer::FIELDS_EXT);
  docs_.prepare(*state.dir, buf, num_blocks, num_incomplete_blocks, block_max_size);

  fields_ = state.fields;
}

// ----------------------------------------------------------------------------
// --SECTION--                                                  postings_writer 
// ----------------------------------------------------------------------------

const string_ref postings_writer::TERMS_FORMAT_NAME = "iresearch_10_postings_terms";

const string_ref postings_writer::DOC_FORMAT_NAME = "iresearch_10_postings_documents";
const string_ref postings_writer::DOC_EXT = "doc";

const string_ref postings_writer::POS_FORMAT_NAME = "iresearch_10_postings_positions";
const string_ref postings_writer::POS_EXT = "pos";

const string_ref postings_writer::PAY_FORMAT_NAME = "iresearch_10_postings_payloads";
const string_ref postings_writer::PAY_EXT = "pay";

void postings_writer::doc_stream::flush(uint64_t* buf, bool freq) {
  write_block(*out, deltas, BLOCK_SIZE, buf);

  if (freq) {
    write_block(*out, freqs.get(), BLOCK_SIZE, reinterpret_cast<uint32_t*>(buf));
  }
}

void postings_writer::pos_stream::flush(uint32_t* comp_buf) {
  write_block(*out, this->buf, BLOCK_SIZE, comp_buf);  
  size = 0;
}

/* postings_writer::pay_stream */

void postings_writer::pay_stream::payload(uint32_t i, const bytes_ref& pay) {
  if (!pay.empty()) {
    pay_buf_.append(pay.c_str(), pay.size());
  }

  pay_sizes[i] = static_cast<uint32_t>(pay.size());
}

void postings_writer::pay_stream::offsets(
    uint32_t i, uint32_t start_offset, uint32_t end_offset) {
  assert(start_offset >= last && start_offset <= end_offset);

  offs_start_buf[i] = start_offset - last;
  offs_len_buf[i] = end_offset - start_offset;
  last = start_offset;
}

void postings_writer::pay_stream::flush_payload(uint32_t* buf) {
  out->write_vint(static_cast<uint32_t>(pay_buf_.size()));
  if (pay_buf_.empty()) {
    return;
  }
  write_block(*out, pay_sizes, BLOCK_SIZE, buf);  
  out->write_bytes(pay_buf_.c_str(), pay_buf_.size());
  pay_buf_.clear();
}

void postings_writer::pay_stream::flush_offsets(uint32_t* buf) {
  write_block(*out, offs_start_buf, BLOCK_SIZE, buf);  
  write_block(*out, offs_len_buf, BLOCK_SIZE, buf);  
}

postings_writer::postings_writer(bool volatile_attributes)
  : skip_(BLOCK_SIZE, SKIP_N), volatile_attributes_(volatile_attributes) {
}

postings_writer::~postings_writer() { }

void postings_writer::prepare(index_output& out, const iresearch::flush_state& state) {
  assert(!state.name.null());

  // reset writer state
  attrs_.clear();
  docs_count = 0;

  std::string name;

  // prepare document stream
  detail::prepare_output(name, doc.out, state, DOC_EXT, DOC_FORMAT_NAME, FORMAT_MAX);

  auto& features = *state.features;
  if (features.check<frequency>()) {
    // prepare frequency stream 
    doc.freqs = memory::make_unique<uint32_t[]>(BLOCK_SIZE);
    std::memset(doc.freqs.get(), 0, sizeof(uint32_t) * BLOCK_SIZE);
  }

  if (features.check< position >()) {
    // prepare proximity stream
    pos_ = memory::make_unique< pos_stream >();
    detail::prepare_output(name, pos_->out, state, POS_EXT, POS_FORMAT_NAME, FORMAT_MAX);

    if (features.check< payload >() || features.check< offset >()) {
      // prepare payload stream
      pay_ = memory::make_unique< pay_stream >();
      detail::prepare_output(name, pay_->out, state, PAY_EXT, PAY_FORMAT_NAME, FORMAT_MAX);
    }
  }

  skip_.prepare(
    MAX_SKIP_LEVELS, state.doc_count,
    [this](size_t i, index_output& out) {
      write_skip(i, out);
  });

  /* write postings format name */
  format_utils::write_header(out, TERMS_FORMAT_NAME, TERMS_FORMAT_MAX);
  /* write postings block size */
  out.write_vint(BLOCK_SIZE);

  /* prepare documents bitset */
  docs_.reset(state.doc_count);
  attrs_.add<version10::documents>()->value = &docs_;
}

void postings_writer::begin_field(const iresearch::flags& field) {
  features_ = version10::features(field);
  docs_.clear();
  last_state.clear();
}

void postings_writer::begin_block() {  
  /* clear state in order to write
   * absolute address of the first
   * entry in the block */
  last_state.clear();
}

#if defined(_MSC_VER)
  #pragma warning( disable : 4706 )
#elif defined (__GNUC__)
  #pragma GCC diagnostic ignored "-Wparentheses"
#endif

void postings_writer::write(doc_iterator& docs, iresearch::attributes& attrs) {
  REGISTER_TIMER_DETAILED();
  auto& freq = docs.attributes().get<frequency>();
  auto& pos = freq ? docs.attributes().get<position>() : attribute_ref<position>::nil();
  const offset* offs = nullptr;
  const payload* pay = nullptr;
  frequency* tfreq = nullptr;
  version10::term_meta* meta = attrs.add<version10::term_meta>();

  if (freq) {
    if (pos && !volatile_attributes_) {
      offs = pos->get< offset >();
      pay = pos->get< payload >();
    }

    tfreq = attrs.add< frequency >();
  }

  begin_term();

  while (docs.next()) {
    auto did = docs.value();

    assert(type_limits<type_t::doc_id_t>::valid(did));
    begin_doc(did, freq);
    docs_.set(did - type_limits<type_t::doc_id_t>::min());

    if (pos) {
      if (volatile_attributes_) {
        offs = pos->get<offset>();
        pay = pos->get<payload>();
      }

      while ( pos->next() ) {
        add_position( pos->value(), offs, pay );
      }
    }

    ++meta->docs_count;
    if (tfreq) {
      tfreq->value += freq->value;
    }

    end_doc();
  }

  end_term(*meta, tfreq);
}

#if defined(_MSC_VER)
  #pragma warning( default : 4706 )
#elif defined (__GNUC__)
  #pragma GCC diagnostic pop
#endif

void postings_writer::begin_term() {
  doc.start = doc.out->file_pointer();
  std::fill_n(doc.skip_ptr, MAX_SKIP_LEVELS, doc.start);
  if (features_.position()) {
    assert(pos_);
    pos_->start = pos_->out->file_pointer();
    std::fill_n(pos_->skip_ptr, MAX_SKIP_LEVELS, pos_->start);
    if (features_.payload() || features_.offset()) {
      assert(pay_);
      pay_->start = pay_->out->file_pointer();
      std::fill_n(pay_->skip_ptr, MAX_SKIP_LEVELS, pay_->start);
    }
  }

  doc.last = type_limits<type_t::doc_id_t>::min(); // for proper delta of 1st id
  doc.block_last = type_limits<type_t::doc_id_t>::invalid();
  skip_.reset();
}

void postings_writer::begin_doc(doc_id_t id, const frequency* freq) {
  if (type_limits<type_t::doc_id_t>::valid(doc.block_last) && 0 == doc.size) {
    skip_.skip(docs_count);
  }

  if (id < doc.last) {
    // docs out of order
    throw index_error();
  }

  doc.doc(id - doc.last);
  if (freq) {
    doc.freq(static_cast<uint32_t>(freq->value));
  }

  doc.next(id);
  if (doc.full()) {
    doc.flush(buf, freq != nullptr);
  }

  if (pos_) pos_->last = 0;
  if (pay_) pay_->last = 0;

  ++docs_count;
}

void postings_writer::add_position(uint32_t pos, const offset* offs, const payload* pay) {
  assert(!offs || offs->start <= offs->end);
  assert(pos_); /* at least positions stream should be created */

  pos_->pos(pos - pos_->last);
  if (pay) pay_->payload(pos_->size, pay->value);
  if (offs) pay_->offsets(pos_->size, offs->start, offs->end);

  pos_->next(pos);

  if (pos_->full()) {
    auto* buf32 = reinterpret_cast<uint32_t*>(buf);

    pos_->flush(buf32);

    if (pay) {
      pay_->flush_payload(buf32);
    }

    if (offs) {
      pay_->flush_offsets(buf32);
    }
  }
}

void postings_writer::end_doc() {
  if ( doc.full() ) {
    doc.block_last = doc.last;
    doc.end = doc.out->file_pointer();
    if ( pos_ ) {
      assert( pos_ );
      pos_->end = pos_->out->file_pointer();
      // documents stream is full, but positions stream is not
      // save number of positions to skip before the next block
      pos_->block_last = pos_->size;       
      if ( pay_ ) {
        pay_->end = pay_->out->file_pointer();
        pay_->block_last = pay_->pay_buf_.size();
      }
    }

    doc.size = 0;
  }
}

void postings_writer::end_term(
    version10::term_meta& meta,
    const frequency* tfreq) {
  if (docs_count == 0) {
    return; // no documents to write
  }

  if (1 == meta.docs_count) {
    meta.e_single_doc = doc.deltas[0];
  } else {
    /* write remaining documents using
     * variable length encoding */
    data_output& out = *doc.out;
    for (uint32_t i = 0; i < doc.size; ++i) {
      const uint32_t doc_delta = doc.deltas[i];

      if (!features_.freq()) {
        out.write_vint(doc_delta);
      } else {
        assert(doc.freqs);
        const uint32_t freq = doc.freqs[i];
        if (1 == freq) {
          out.write_vint(shift_pack_32(doc_delta, true));
        } else {
          out.write_vint(shift_pack_32(doc_delta, false));
          out.write_vint(freq);
        }
      }
    }
  }

  meta.pos_end = type_limits<type_t::address_t>::invalid();

  /* write remaining position using
   * variable length encoding */
  if (features_.position()) {

    if (tfreq->value > BLOCK_SIZE) {
      meta.pos_end = pos_->out->file_pointer() - pos_->start;
    }

    if (pos_->size > 0) {
      data_output& out = *pos_->out;
      uint32_t last_pay_size = integer_traits<uint32_t>::const_max;
      uint32_t last_offs_len = integer_traits<uint32_t>::const_max;
      uint32_t pay_buf_start = 0;
      for (uint32_t i = 0; i < pos_->size; ++i) {
        const uint32_t pos_delta = pos_->buf[i];
        if (features_.payload()) {
          const uint32_t size = pay_->pay_sizes[i];
          if (last_pay_size != size) {
            last_pay_size = size;
            out.write_vint(shift_pack_32(pos_delta, true));
            out.write_vint(size);
          } else {
            out.write_vint(shift_pack_32(pos_delta, false));
          }

          if (size != 0) {
            out.write_bytes(pay_->pay_buf_.c_str() + pay_buf_start, size);
            pay_buf_start += size;
          }
        } else {
          out.write_vint(pos_delta);
        }

        if (features_.offset()) {
          const uint32_t pay_offs_delta = pay_->offs_start_buf[i];
          const uint32_t len = pay_->offs_len_buf[i];
          if (len == last_offs_len) {
            out.write_vint(shift_pack_32(pay_offs_delta, false));
          } else {
            out.write_vint(shift_pack_32(pay_offs_delta, true));
            out.write_vint(len);
            last_offs_len = len;
          }
        }
      }

      if (features_.payload()) {
        pay_->pay_buf_.clear();
      }
    }
  }

  /* if we have flushed at least
   * one block there was buffered
   * skip data, so we need to flush it*/
  if (docs_count > BLOCK_SIZE) {
    //const uint64_t start = doc.out->file_pointer();
    meta.e_skip_start = doc.out->file_pointer() - doc.start;
    skip_.flush(*doc.out);
  }

  docs_count = 0;
  doc.size = 0;
  doc.last = 0;
  meta.doc_start = doc.start;

  if (pos_) {
    pos_->size = 0;
    meta.pos_start = pos_->start;
  }

  if (pay_) {
    //pay_->buf_size = 0;
    pay_->pay_buf_.clear();
    pay_->last = 0;
    meta.pay_start = pay_->start;
  }
}

void postings_writer::write_skip(size_t level, index_output& out) {
  const uint32_t doc_delta = doc.block_last; //- doc.skip_doc[level];
  const uint64_t doc_ptr = doc.out->file_pointer();

  out.write_vint(doc_delta);
  out.write_vlong(doc_ptr - doc.skip_ptr[level]);

  doc.skip_doc[level] = doc.block_last;
  doc.skip_ptr[level] = doc_ptr;

  if (features_.position()) {
    assert(pos_);

    const uint64_t pos_ptr = pos_->out->file_pointer();

    out.write_vint(pos_->block_last);
    out.write_vlong(pos_ptr - pos_->skip_ptr[level]);

    pos_->skip_ptr[level] = pos_ptr;

    if (features_.payload() || features_.offset()) {
      assert(pay_);

      if (features_.payload()) {
        out.write_vint(static_cast<uint32_t>(pay_->block_last));
      }

      const uint64_t pay_ptr = pay_->out->file_pointer();

      out.write_vlong(pay_ptr - pay_->skip_ptr[level]);
      pay_->skip_ptr[level] = pay_ptr;
    }
  }
}

void postings_writer::encode(data_output& out, const iresearch::attributes& attrs) {
  const version10::term_meta* meta = attrs.get<term_meta>();
  const frequency* tfreq = attrs.get<frequency>();

  out.write_vlong(meta->docs_count);
  if (tfreq) {
    assert(tfreq->value >= meta->docs_count);
    out.write_vlong(tfreq->value - meta->docs_count);
  }

  out.write_vlong(meta->doc_start - last_state.doc_start);
  if (features_.position()) {
    out.write_vlong(meta->pos_start - last_state.pos_start);
    if (type_limits<type_t::address_t>::valid(meta->pos_end)) {
      out.write_vlong(meta->pos_end);
    }
    if (features_.payload() || features_.offset()) {
      out.write_vlong(meta->pay_start - last_state.pay_start);
    }
  }

  if (1U == meta->docs_count || meta->docs_count > postings_writer::BLOCK_SIZE) {
    out.write_vlong(meta->e_skip_start);
  }

  last_state = *meta;
}

void postings_writer::end() {
  format_utils::write_footer(*doc.out);
  doc.out.reset(); // ensure stream is closed

  if (pos_) {
    format_utils::write_footer(*pos_->out);
    pos_->out.reset(); // ensure stream is closed
  }

  if (pay_) {
    format_utils::write_footer(*pay_->out);
    pay_->out.reset(); // ensure stream is closed
  }
}

// ----------------------------------------------------------------------------
// --SECTION--                                                  postings_reader 
// ----------------------------------------------------------------------------

void postings_reader::prepare( index_input& in, const reader_state& state ) {
  std::string buf;
 
  /* prepare document input */
  detail::prepare_input( 
    buf, doc_in_, state,
    postings_writer::DOC_EXT, 
    postings_writer::DOC_FORMAT_NAME, 
    postings_writer::FORMAT_MIN, 
    postings_writer::FORMAT_MAX 
  );
    
  /* Since terms doc postings too large
   * it is too costly to verify checksum of
   * the entire file. Here we perform cheap
   * error detection which could recognize
   * some forms of corruption. */
  format_utils::read_checksum(*doc_in_);
  docs_mask_ = state.docs_mask;

  auto& features = state.fields->features();
  if (features.check<position>()) {
    /* prepare positions input */
    detail::prepare_input(
      buf, pos_in_, state,
      postings_writer::POS_EXT,
      postings_writer::POS_FORMAT_NAME,
      postings_writer::FORMAT_MIN,
      postings_writer::FORMAT_MAX
    );
    
    /* Since terms pos postings too large
     * it is too costly to verify checksum of
     * the entire file. Here we perform cheap
     * error detection which could recognize
     * some forms of corruption. */
    format_utils::read_checksum(*pos_in_);

    if (features.check<payload>() || features.check<offset>()) {
      /* prepare positions input */
      detail::prepare_input(
        buf, pay_in_, state,
        postings_writer::PAY_EXT,
        postings_writer::PAY_FORMAT_NAME,
        postings_writer::FORMAT_MIN,
        postings_writer::FORMAT_MAX
      );

      /* Since terms pos postings too large
       * it is too costly to verify checksum of
       * the entire file. Here we perform cheap
       * error detection which could recognize
       * some forms of corruption. */
      format_utils::read_checksum(*pay_in_);
    }
  }

  /* check postings format */
  format_utils::check_header(in,
    postings_writer::TERMS_FORMAT_NAME, 
    postings_writer::TERMS_FORMAT_MIN, 
    postings_writer::TERMS_FORMAT_MAX 
  );

  const uint64_t block_size = in.read_vlong();
  if ( block_size != postings_writer::BLOCK_SIZE ) {
    /* invalid block size */
    throw index_error();
  }
}

void postings_reader::decode( 
    data_input& in, 
    const flags& meta, 
    attributes& attrs) {
  version10::term_meta* tmeta = attrs.add<version10::term_meta>();
  frequency* tfreq = attrs.get<frequency>();

  assert(tmeta);

  tmeta->docs_count = in.read_vlong();
  if (tfreq) {
    tfreq->value = tmeta->docs_count + in.read_vlong();
  }

  tmeta->doc_start += in.read_vlong();
  if (tfreq && tfreq->value && meta.check<position>()) {
    tmeta->pos_start += in.read_vlong();

    tmeta->pos_end = tfreq->value > postings_writer::BLOCK_SIZE
        ? in.read_vlong()
        : type_limits<type_t::address_t>::invalid();

    if (meta.check<payload>() || meta.check<offset>()) {
      tmeta->pay_start += in.read_vlong();
    }
  }

  if (1U == tmeta->docs_count || tmeta->docs_count > postings_writer::BLOCK_SIZE) {
    tmeta->e_skip_start = in.read_vlong();
  }
}

doc_iterator::ptr postings_reader::iterator(
    const flags& field,
    const attributes& attrs,
    const flags& req ) {
  detail::doc_iterator::ptr it = !docs_mask_ || docs_mask_->empty()
    ? detail::doc_iterator::make<detail::doc_iterator>() 
    : detail::doc_iterator::make<detail::mask_doc_iterator>( *docs_mask_ );

  it->prepare( 
    field, attrs, req, 
    doc_in_.get(), pos_in_.get(), pay_in_.get() 
  );

  return std::move(it);
}

// ----------------------------------------------------------------------------
// --SECTION--                                                           format 
// ----------------------------------------------------------------------------

format::format(): iresearch::format(format::type()) {}

index_meta_writer::ptr format::get_index_meta_writer() const  {
  return iresearch::index_meta_writer::make<index_meta_writer>();
}

index_meta_reader::ptr format::get_index_meta_reader() const {
  static auto reader = iresearch::index_meta_reader::make<index_meta_reader>();
  // can reuse stateless reader
  return reader;
}

segment_meta_writer::ptr format::get_segment_meta_writer() const {
  static auto writer = iresearch::segment_meta_writer::make<segment_meta_writer>();
  // can reuse stateless writer 
  return writer;
}

segment_meta_reader::ptr format::get_segment_meta_reader() const {
  static auto reader = iresearch::segment_meta_reader::make<segment_meta_reader>();
  // can reuse stateless writer 
  return reader;
}

document_mask_writer::ptr format::get_document_mask_writer() const {
  return iresearch::document_mask_writer::make<document_mask_writer>();
}

document_mask_reader::ptr format::get_document_mask_reader() const {
  return iresearch::document_mask_reader::make<document_mask_reader>();
}

field_meta_reader::ptr format::get_field_meta_reader() const {
  return iresearch::field_meta_reader::make< field_meta_reader >();
}

field_meta_writer::ptr format::get_field_meta_writer() const {
  return iresearch::field_meta_writer::make< field_meta_writer >();
}

field_writer::ptr format::get_field_writer(bool volatile_attributes /*=false*/) const {
  return iresearch::field_writer::make<burst_trie::field_writer>(
    iresearch::postings_writer::make<version10::postings_writer>(volatile_attributes)
  );
}

field_reader::ptr format::get_field_reader() const  {
  return iresearch::field_reader::make<iresearch::burst_trie::field_reader>(
    iresearch::postings_reader::make<version10::postings_reader>()
  );
}

stored_fields_writer::ptr format::get_stored_fields_writer() const {
  const uint32_t STORED_FIELDS_BUFFER_SIZE = 32768;
  const uint32_t STORED_FIELDS_INDEX_BLOCK_SIZE = 1024;

  return iresearch::stored_fields_writer::make< stored_fields_writer >(
    STORED_FIELDS_BUFFER_SIZE,
    STORED_FIELDS_INDEX_BLOCK_SIZE
  );
}

stored_fields_reader::ptr format::get_stored_fields_reader() const {
  return iresearch::stored_fields_reader::make< stored_fields_reader >();
}
  
column_meta_writer::ptr format::get_column_meta_writer() const {
  return iresearch::column_meta_writer::ptr(new columns::meta_writer());
}

column_meta_reader::ptr format::get_column_meta_reader() const {
  return iresearch::column_meta_reader::ptr(new columns::meta_reader());
}

columnstore_writer::ptr format::get_columnstore_writer() const {
  return columnstore_writer::ptr(new columns::writer());
}

columnstore_reader::ptr format::get_columnstore_reader() const {
  return columnstore_reader::ptr(new columns::reader());
}

DEFINE_FORMAT_TYPE_NAMED(iresearch::version10::format, "1_0");
REGISTER_FORMAT( iresearch::version10::format );
DEFINE_FACTORY_SINGLETON(format);

NS_END /* version10 */
NS_END /* root */

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
