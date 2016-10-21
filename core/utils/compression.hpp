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

#ifndef IRESEARCH_COMPRESSION_H
#define IRESEARCH_COMPRESSION_H

#include "string.hpp"
#include "noncopyable.hpp"

#include <memory>

NS_ROOT

class IRESEARCH_API compressor: public bytes_ref, private util::noncopyable {
 public:
  explicit compressor(unsigned int chunk_size);

  void compress(const char* src, size_t size);

  inline void compress(const bytes_ref& src) {
    compress(ref_cast<char>(src).c_str(), src.size());
  }

 private:
  IRESEARCH_API_PRIVATE_VARIABLES_BEGIN
  std::string buf_;
  std::shared_ptr<void> stream_; // hide internal LZ4 implementation
  IRESEARCH_API_PRIVATE_VARIABLES_END
}; // compressor

class IRESEARCH_API decompressor: public bytes_ref, private util::noncopyable {
 public:
  decompressor();

  explicit decompressor(unsigned int chunk_size);

  void block_size(size_t size);

  size_t block_size() const { return buf_.size(); }

  void decompress(const char* src, size_t size);

  inline void decompress(const bytes_ref& src) {
    decompress(ref_cast<char>(src).c_str(), src.size());
  }

 private:
  IRESEARCH_API_PRIVATE_VARIABLES_BEGIN
  std::string buf_;
  std::shared_ptr<void> stream_; // hide internal LZ4 implementation
  IRESEARCH_API_PRIVATE_VARIABLES_END
}; // decompressor

NS_END // NS_ROOT

#endif