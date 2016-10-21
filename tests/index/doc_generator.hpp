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

#ifndef IRESEARCH_DOCUMENT_GENERATOR_H
#define IRESEARCH_DOCUMENT_GENERATOR_H

#include "analysis/token_streams.hpp"
#include "document/serializer.hpp"
#include "utils/iterator.hpp"
#include "store/store_utils.hpp"
#include "json_parser.hpp"

#include <fstream>
#include <atomic>
#include <functional>

#include <boost/filesystem.hpp>

namespace iresearch {

struct data_output;
class flags;
class token_stream;

}

namespace tests {

namespace fs = ::boost::filesystem;
namespace ir = iresearch;

//////////////////////////////////////////////////////////////////////////////
/// @class ifield
/// @brief base interface for all fields
//////////////////////////////////////////////////////////////////////////////
struct ifield : ir::serializer {
  DECLARE_PTR(ifield);
  virtual ~ifield() {};

  virtual ir::string_ref name() const = 0;
  virtual float_t boost() const = 0;
  virtual bool write(ir::data_output& out) const = 0;
  virtual ir::token_stream* get_tokens() const = 0;
  virtual const ir::flags& features() const = 0;
  virtual const ir::serializer* serializer() const = 0;
}; // ifield

//////////////////////////////////////////////////////////////////////////////
/// @class field 
/// @brief base class for field implementations
//////////////////////////////////////////////////////////////////////////////
class field_base : public ifield {
 public:
  field_base() = default;
  virtual ~field_base();

  field_base(field_base&& rhs);
  field_base& operator=(field_base&& rhs);

  field_base(const field_base&) = default;
  field_base& operator=(const field_base&) = default;

  ir::string_ref name() const { return name_; }
  void name(std::string&& name) { name_ = std::move(name); }
  void name(const std::string& name) { name_ = name; }

  void boost(float_t value) { boost_ = value; }
  float_t boost() const { return boost_; }

  bool indexed() const { return indexed_; }
  void indexed(bool value) { indexed_ = value; }
  
  bool stored() const { return stored_; }
  void stored(bool value) { stored_ = value; }

  const ir::serializer* serializer() const { return stored_ ? this : nullptr; }

 private:
  std::string name_;
  float_t boost_{ 1.f };
  bool indexed_{ true };
  bool stored_{ true };
}; // field_base

//////////////////////////////////////////////////////////////////////////////
/// @class long_field 
/// @brief provides capabilities for storing & indexing int64_t values 
//////////////////////////////////////////////////////////////////////////////
class long_field: public field_base {
 public:
  typedef int64_t value_t;

  long_field() = default;

  void value(value_t value) { value_ = value; }
  value_t value() const { return value_; }
  bool write(ir::data_output& out) const override;
  ir::token_stream* get_tokens() const;
  const ir::flags& features() const;

 private:
  mutable ir::numeric_token_stream stream_;
  int64_t value_{};
}; // long_field 

//////////////////////////////////////////////////////////////////////////////
/// @class long_field 
/// @brief provides capabilities for storing & indexing int32_t values 
//////////////////////////////////////////////////////////////////////////////
class int_field: public field_base {
 public:
  typedef int32_t value_t;

  int_field() = default;

  void value(value_t value) { value_ = value; }
  value_t value() const { return value_; }

  bool write(ir::data_output& out) const override;
  ir::token_stream* get_tokens() const;
  const ir::flags& features() const;

 private:
  mutable ir::numeric_token_stream stream_;
  int32_t value_{};
}; // int_field 

//////////////////////////////////////////////////////////////////////////////
/// @class long_field 
/// @brief provides capabilities for storing & indexing double_t values 
//////////////////////////////////////////////////////////////////////////////
class double_field: public field_base {
 public:
  typedef double_t value_t;

  double_field() = default;

  void value(value_t value) { value_ = value; }
  value_t value() const { return value_; }

  bool write(ir::data_output& out) const override;
  ir::token_stream* get_tokens() const;
  const ir::flags& features() const;

 private:
  mutable ir::numeric_token_stream stream_;
  double_t value_{};
}; // double_field

//////////////////////////////////////////////////////////////////////////////
/// @class long_field 
/// @brief provides capabilities for storing & indexing double_t values 
//////////////////////////////////////////////////////////////////////////////
class float_field: public field_base {
 public:
  typedef float_t value_t;

  float_field() = default;

  void value(value_t value) { value_ = value; }
  value_t value() const { return value_; }

  bool write(ir::data_output& out) const override;
  ir::token_stream* get_tokens() const;
  const ir::flags& features() const;

 private:
  mutable ir::numeric_token_stream stream_;
  float_t value_{};
}; // float_field 

//////////////////////////////////////////////////////////////////////////////
/// @class binary_field 
/// @brief provides capabilities for storing & indexing binary values 
//////////////////////////////////////////////////////////////////////////////
class binary_field: public field_base {
 public:
  binary_field() = default;

  const ir::bstring& value() const { return value_; }
  void value(const ir::bytes_ref& value) { value_ = value; }
  void value(ir::bstring&& value) { value_ = std::move(value); }

  template<typename Iterator>
  void value(Iterator first, Iterator last) {
    value_ = bytes(first, last);
  }
  
  bool write(ir::data_output& out) const override;
  ir::token_stream* get_tokens() const;
  const ir::flags& features() const;

 private:
  mutable ir::string_token_stream stream_;
  ir::bstring value_;
}; // binary_field

/* -------------------------------------------------------------------
* document 
* ------------------------------------------------------------------*/

class document {
 public:
  typedef std::vector<ifield::ptr> fields_t;
  typedef ir::ptr_iterator<fields_t::const_iterator> const_iterator;
  typedef ir::ptr_iterator<fields_t::iterator> iterator;

  document() = default;
  document( document&& rhs );

  virtual ~document();
  
  document& operator=( document&& rhs );

  size_t size() const { return fields_.size(); }
  void clear() { fields_.clear(); }

  /* Adds field into document. Document become the owner of the field. */
  void add(ifield* fld) { fields_.emplace_back( fld ); }
  bool contains( const ir::string_ref& name ) const;
  ifield* get(const ir::string_ref& name) const;
  std::vector<const ifield*> find(const ir::string_ref& name) const;
  void remove( const ir::string_ref& name );

  ifield& back() const {
    return *fields_.back();
  }

  template<typename T>
  T& back() const {
    typedef typename std::enable_if<
      std::is_base_of<tests::ifield, T>::value, T
     >::type type;
      
    return static_cast<type&>(*fields_.back());
  }
  
  template<typename T>
  T& get(size_t i) const {
    typedef typename std::enable_if<
      std::is_base_of<tests::ifield, T>::value, T
     >::type type;
      
    return static_cast<type&>(*fields_[i]);
  }
  
  template<typename T>
  T* get(const ir::string_ref& name) const {
    typedef typename std::enable_if<
      std::is_base_of<tests::ifield, T>::value, T
     >::type type;
      
    return static_cast<type*>(get(name));
  }

  iterator begin() { return iterator(fields_.begin()); }
  iterator end() { return iterator(fields_.end()); }

  const_iterator begin() const { return const_iterator(fields_.begin()); }
  const_iterator end() const { return const_iterator(fields_.end()); }
    
 protected:
  fields_t fields_;
}; // document
  
/* -------------------------------------------------------------------
* GENERATORS 
* ------------------------------------------------------------------*/

/* Base class for document generators */
struct doc_generator_base {
  DECLARE_PTR( doc_generator_base );
  DECLARE_FACTORY( doc_generator_base );

  virtual const tests::document* next() = 0;
  virtual void reset() = 0;

  virtual ~doc_generator_base() { }
};

/* Generates documents from UTF-8 encoded file 
 * with strings of the following format:
 * <title>;<date>:<body> */
class delim_doc_generator : public doc_generator_base {
 public:
  struct doc_template : document {
    virtual void init() = 0;
    virtual void value(size_t idx, const std::string& value) = 0;
    virtual void end() {}
    virtual void reset() {} 
  }; // doc_template

  delim_doc_generator(
    const fs::path& file, 
    doc_template& doc,
    uint32_t delim = 0x0009);

  virtual const tests::document* next() override;
  virtual void reset() override;

 private:
  std::string str_;
  std::ifstream ifs_;  
  doc_template* doc_;
  uint32_t delim_;
}; // delim_doc_generator

/* Generates documents from json file based on type of JSON value */
class json_doc_generator: public doc_generator_base {
 public:
  typedef std::function<void(
    tests::document&,
    const std::string&,
    const tests::json::json_value&)
  > factory_f;

  json_doc_generator(
    const fs::path& file,
    const factory_f& factory);

  json_doc_generator(json_doc_generator&& rhs);

  virtual const tests::document* next() override;
  virtual void reset() override;

 private:
  json_doc_generator(const json_doc_generator&) = delete;

  std::vector<document> docs_;
  std::vector<document>::const_iterator prev_;
  std::vector<document>::const_iterator next_;
}; // json_doc_generator

} // tests

#endif
