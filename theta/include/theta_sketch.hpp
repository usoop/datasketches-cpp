/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef THETA_SKETCH_HPP_
#define THETA_SKETCH_HPP_

#include "theta_update_sketch_base.hpp"
#include "compact_theta_sketch_parser.hpp"

namespace datasketches {

// forward declarations
template<typename A> class theta_sketch_alloc;
template<typename A> class update_theta_sketch_alloc;
template<typename A> class compact_theta_sketch_alloc;
template<typename A> class wrapped_compact_theta_sketch_alloc;

/// Theta sketch alias with default allocator
using theta_sketch = theta_sketch_alloc<std::allocator<uint64_t>>;
/// Update Theta sketch alias with default allocator
using update_theta_sketch = update_theta_sketch_alloc<std::allocator<uint64_t>>;
/// Compact Theta sketch alias with default allocator
using compact_theta_sketch = compact_theta_sketch_alloc<std::allocator<uint64_t>>;
/// Wrapped Compact Theta sketch alias with default allocator
using wrapped_compact_theta_sketch = wrapped_compact_theta_sketch_alloc<std::allocator<uint64_t>>;

/// Abstract base class for Theta sketch
template<typename Allocator = std::allocator<uint64_t>>
class base_theta_sketch_alloc {
public:

  virtual ~base_theta_sketch_alloc() = default;

  /**
   * @return allocator
   */
  virtual Allocator get_allocator() const = 0;

  /**
   * @return true if this sketch represents an empty set (not the same as no retained entries!)
   */
  virtual bool is_empty() const = 0;

  /**
   * @return estimate of the distinct count of the input stream
   */
  double get_estimate() const;

  /**
   * Returns the approximate lower error bound given a number of standard deviations.
   * This parameter is similar to the number of standard deviations of the normal distribution
   * and corresponds to approximately 67%, 95% and 99% confidence intervals.
   * @param num_std_devs number of Standard Deviations (1, 2 or 3)
   * @return the lower bound
   */
  double get_lower_bound(uint8_t num_std_devs) const;

  /**
   * Returns the approximate upper error bound given a number of standard deviations.
   * This parameter is similar to the number of standard deviations of the normal distribution
   * and corresponds to approximately 67%, 95% and 99% confidence intervals.
   * @param num_std_devs number of Standard Deviations (1, 2 or 3)
   * @return the upper bound
   */
  double get_upper_bound(uint8_t num_std_devs) const;

  /**
   * @return true if the sketch is in estimation mode (as opposed to exact mode)
   */
  bool is_estimation_mode() const;

  /**
   * @return theta as a fraction from 0 to 1 (effective sampling rate)
   */
  double get_theta() const;

  /**
   * @return theta as a positive integer between 0 and LLONG_MAX
   */
  virtual uint64_t get_theta64() const = 0;

  /**
   * @return the number of retained entries in the sketch
   */
  virtual uint32_t get_num_retained() const = 0;

  /**
   * @return hash of the seed that was used to hash the input
   */
  virtual uint16_t get_seed_hash() const = 0;

  /**
   * @return true if retained entries are ordered
   */
  virtual bool is_ordered() const = 0;

  /**
   * Provides a human-readable summary of this sketch as a string
   * @param print_items if true include the list of items retained by the sketch
   * @return sketch summary as a string
   */
  virtual string<Allocator> to_string(bool print_items = false) const;

protected:
  virtual void print_specifics(std::ostringstream& os) const = 0;
  virtual void print_items(std::ostringstream& os) const = 0;
};

/// Base class for the Theta Sketch, a generalization of the Kth Minimum Value (KMV) sketch.
template<typename Allocator = std::allocator<uint64_t>>
class theta_sketch_alloc: public base_theta_sketch_alloc<Allocator> {
public:
  using Entry = uint64_t;
  using ExtractKey = trivial_extract_key;
  using iterator = theta_iterator<Entry, ExtractKey>;
  using const_iterator = theta_const_iterator<Entry, ExtractKey>;

  virtual ~theta_sketch_alloc() = default;

  /**
   * Iterator over hash values in this sketch.
   * @return begin iterator
   */
  virtual iterator begin() = 0;

  /**
   * Iterator pointing past the valid range.
   * Not to be incremented or dereferenced.
   * @return end iterator
   */
  virtual iterator end() = 0;

  /**
   * Const iterator over hash values in this sketch.
   * @return begin iterator
   */
  virtual const_iterator begin() const = 0;

  /**
   * Const iterator pointing past the valid range.
   * Not to be incremented or dereferenced.
   * @return end iterator
   */
  virtual const_iterator end() const = 0;

protected:
  virtual void print_items(std::ostringstream& os) const;
};

// forward declaration
template<typename A> class compact_theta_sketch_alloc;

/**
 * Update Theta sketch.
 * The purpose of this class is to build a Theta sketch from input data via the update() methods.
 * There is no constructor. Use builder instead.
 */
template<typename Allocator = std::allocator<uint64_t>>
class update_theta_sketch_alloc: public theta_sketch_alloc<Allocator> {
public:
  using Base = theta_sketch_alloc<Allocator>;
  using Entry = typename Base::Entry;
  using ExtractKey = typename Base::ExtractKey;
  using iterator = typename Base::iterator;
  using const_iterator = typename Base::const_iterator;
  using theta_table = theta_update_sketch_base<Entry, ExtractKey, Allocator>;
  using resize_factor = typename theta_table::resize_factor;

  // No constructor here. Use builder instead.
  class builder;

  /**
   * Copy constructor
   * @param other sketch to be copied
   */
  update_theta_sketch_alloc(const update_theta_sketch_alloc& other) = default;

  /**
   * Move constructor
   * @param other sketch to be moved
   */
  update_theta_sketch_alloc(update_theta_sketch_alloc&& other) noexcept = default;

  virtual ~update_theta_sketch_alloc() = default;

  /**
   * Copy assignment
   * @param other sketch to be copied
   * @return reference to this sketch
   */
  update_theta_sketch_alloc& operator=(const update_theta_sketch_alloc& other) = default;

  /**
   * Move assignment
   * @param other sketch to be moved
   * @return reference to this sketch
   */
  update_theta_sketch_alloc& operator=(update_theta_sketch_alloc&& other) = default;

  virtual Allocator get_allocator() const;
  virtual bool is_empty() const;
  virtual bool is_ordered() const;
  virtual uint16_t get_seed_hash() const;
  virtual uint64_t get_theta64() const;
  virtual uint32_t get_num_retained() const;

  /**
   * @return configured nominal number of entries in the sketch
   */
  uint8_t get_lg_k() const;

  /**
   * @return configured resize factor of the sketch
   */
  resize_factor get_rf() const;

  /**
   * Update this sketch with a given string.
   * @param value string to update the sketch with
   */
  void update(const std::string& value);

  /**
   * Update this sketch with a given unsigned 64-bit integer.
   * @param value uint64_t to update the sketch with
   */
  void update(uint64_t value);

  /**
   * Update this sketch with a given signed 64-bit integer.
   * @param value int64_t to update the sketch with
   */
  void update(int64_t value);

  /**
   * Update this sketch with a given unsigned 32-bit integer.
   * For compatibility with Java implementation.
   * @param value uint32_t to update the sketch with
   */
  void update(uint32_t value);

  /**
   * Update this sketch with a given signed 32-bit integer.
   * For compatibility with Java implementation.
   * @param value int32_t to update the sketch with
   */
  void update(int32_t value);

  /**
   * Update this sketch with a given unsigned 16-bit integer.
   * For compatibility with Java implementation.
   * @param value uint16_t to update the sketch with
   */
  void update(uint16_t value);

  /**
   * Update this sketch with a given signed 16-bit integer.
   * For compatibility with Java implementation.
   * @param value int16_t to update the sketch with
   */
  void update(int16_t value);

  /**
   * Update this sketch with a given unsigned 8-bit integer.
   * For compatibility with Java implementation.
   * @param value uint8_t to update the sketch with
   */
  void update(uint8_t value);

  /**
   * Update this sketch with a given signed 8-bit integer.
   * For compatibility with Java implementation.
   * @param value int8_t to update the sketch with
   */
  void update(int8_t value);

  /**
   * Update this sketch with a given double-precision floating point value.
   * For compatibility with Java implementation.
   * @param value double to update the sketch with
   */
  void update(double value);

  /**
   * Update this sketch with a given floating point value.
   * For compatibility with Java implementation.
   * @param value float to update the sketch with
   */
  void update(float value);

  /**
   * Update this sketch with given data of any type.
   * This is a "universal" update that covers all cases above,
   * but may produce different hashes.
   * Be very careful to hash input values consistently using the same approach
   * both over time and on different platforms
   * and while passing sketches between C++ environment and Java environment.
   * Otherwise two sketches that should represent overlapping sets will be disjoint
   * For instance, for signed 32-bit values call update(int32_t) method above,
   * which does widening conversion to int64_t, if compatibility with Java is expected
   * @param data pointer to the data
   * @param length of the data in bytes
   */
  void update(const void* data, size_t length);

  /**
   * Remove retained entries in excess of the nominal size k (if any)
   */
  void trim();

  /**
   * Reset the sketch to the initial empty state
   */
  void reset();

  /**
   * Converts this sketch to a compact sketch (ordered or unordered).
   * @param ordered optional flag to specify if an ordered sketch should be produced
   * @return compact sketch
   */
  compact_theta_sketch_alloc<Allocator> compact(bool ordered = true) const;

  virtual iterator begin();
  virtual iterator end();
  virtual const_iterator begin() const;
  virtual const_iterator end() const;

private:
  theta_table table_;

  // for builder
  update_theta_sketch_alloc(uint8_t lg_cur_size, uint8_t lg_nom_size, resize_factor rf, float p,
      uint64_t theta, uint64_t seed, const Allocator& allocator);

  virtual void print_specifics(std::ostringstream& os) const;
};

/**
 * Compact Theta sketch.
 * This is an immutable form of the Theta sketch, the form that can be serialized and deserialized.
 */
template<typename Allocator = std::allocator<uint64_t>>
class compact_theta_sketch_alloc: public theta_sketch_alloc<Allocator> {
public:
  using Base = theta_sketch_alloc<Allocator>;
  using iterator = typename Base::iterator;
  using const_iterator = typename Base::const_iterator;
  using AllocBytes = typename std::allocator_traits<Allocator>::template rebind_alloc<uint8_t>;
  using vector_bytes = std::vector<uint8_t, AllocBytes>;

  static const uint8_t UNCOMPRESSED_SERIAL_VERSION = 3;
  static const uint8_t COMPRESSED_SERIAL_VERSION = 4;
  static const uint8_t SKETCH_TYPE = 3;

  // Instances of this type can be obtained:
  // - by compacting an update_theta_sketch_alloc
  // - as a result of a set operation
  // - by deserializing a previously serialized compact sketch

  /**
   * Copy constructor.
   * Constructs a compact sketch from any other type of Theta sketch
   * @param other sketch to be constructed from
   * @param ordered if true make the resulting sketch ordered
   */
  template<typename Other>
  compact_theta_sketch_alloc(const Other& other, bool ordered);

  /**
   * Copy constructor
   * @param other sketch to be copied
   */
  compact_theta_sketch_alloc(const compact_theta_sketch_alloc& other) = default;

  /**
   * Move constructor
   * @param other sketch to be moved
   */
  compact_theta_sketch_alloc(compact_theta_sketch_alloc&& other) noexcept = default;

  virtual ~compact_theta_sketch_alloc() = default;

  /**
   * Copy assignment
   * @param other sketch to be copied
   * @return reference to this sketch
   */
  compact_theta_sketch_alloc& operator=(const compact_theta_sketch_alloc& other) = default;

  /**
   * Move assignment
   * @param other sketch to be moved
   * @return reference to this sketch
   */
  compact_theta_sketch_alloc& operator=(compact_theta_sketch_alloc&& other) = default;

  virtual Allocator get_allocator() const;
  virtual bool is_empty() const;
  virtual bool is_ordered() const;
  virtual uint64_t get_theta64() const;
  virtual uint32_t get_num_retained() const;
  virtual uint16_t get_seed_hash() const;

  /**
   * Computes maximum serialized size in bytes
   * @param lg_k nominal number of entries in the sketch
   */
  static size_t get_max_serialized_size_bytes(uint8_t lg_k);

  /**
   * Computes size in bytes required to serialize the current state of the sketch.
   * Computing compressed size is expensive. It takes iterating over all retained hashes,
   * and the actual serialization will have to look at them again.
   * @param compressed if true compressed size is returned (if applicable)
   */
  size_t get_serialized_size_bytes(bool compressed = false) const;

  /**
   * This method serializes the sketch into a given stream in a binary form
   * @param os output stream
   */
  void serialize(std::ostream& os) const;

  /**
   * This method serializes the sketch as a vector of bytes.
   * An optional header can be reserved in front of the sketch.
   * It is an uninitialized space of a given size.
   * This header is used in Datasketches PostgreSQL extension.
   * @param header_size_bytes space to reserve in front of the sketch
   */
  vector_bytes serialize(unsigned header_size_bytes = 0) const;

  /**
   * This method serializes the sketch into a given stream in a compressed binary form.
   * Compression is applied to ordered sketches except empty and single item.
   * For unordered, empty and single item sketches this method is equivalent to serialize()
   * @param os output stream
   */
  void serialize_compressed(std::ostream& os) const;

  /**
   * This method serializes the sketch as a vector of bytes.
   * An optional header can be reserved in front of the sketch.
   * It is an uninitialized space of a given size.
   * This header is used in Datasketches PostgreSQL extension.
   * Compression is applied to ordered sketches except empty and single item.
   * For unordered, empty and single item sketches this method is equivalent to serialize()
   * @param header_size_bytes space to reserve in front of the sketch
   */
  vector_bytes serialize_compressed(unsigned header_size_bytes = 0) const;

  virtual iterator begin();
  virtual iterator end();
  virtual const_iterator begin() const;
  virtual const_iterator end() const;

  /**
   * This method deserializes a sketch from a given stream.
   * @param is input stream
   * @param seed the seed for the hash function that was used to create the sketch
   * @param allocator instance of an Allocator
   * @return an instance of the sketch
   */
  static compact_theta_sketch_alloc deserialize(std::istream& is,
      uint64_t seed = DEFAULT_SEED, const Allocator& allocator = Allocator());

  /**
   * This method deserializes a sketch from a given array of bytes.
   * @param bytes pointer to the array of bytes
   * @param size the size of the array
   * @param seed the seed for the hash function that was used to create the sketch
   * @param allocator instance of an Allocator
   * @return an instance of the sketch
   */
  static compact_theta_sketch_alloc deserialize(const void* bytes, size_t size,
      uint64_t seed = DEFAULT_SEED, const Allocator& allocator = Allocator());

private:
  enum flags { IS_BIG_ENDIAN, IS_READ_ONLY, IS_EMPTY, IS_COMPACT, IS_ORDERED };

  bool is_empty_;
  bool is_ordered_;
  uint16_t seed_hash_;
  uint64_t theta_;
  std::vector<uint64_t, Allocator> entries_;

  uint8_t get_preamble_longs(bool compressed) const;
  bool is_suitable_for_compression() const;
  uint8_t compute_entry_bits() const;
  uint8_t get_num_entries_bytes() const;
  size_t get_compressed_serialized_size_bytes(uint8_t entry_bits, uint8_t num_entries_bytes) const;
  void serialize_version_4(std::ostream& os) const;
  vector_bytes serialize_version_4(unsigned header_size_bytes = 0) const;

  static compact_theta_sketch_alloc deserialize_v1(uint8_t preamble_longs, std::istream& is, uint64_t seed, const Allocator& allocator);
  static compact_theta_sketch_alloc deserialize_v2(uint8_t preamble_longs, std::istream& is, uint64_t seed, const Allocator& allocator);
  static compact_theta_sketch_alloc deserialize_v3(uint8_t preamble_longs, std::istream& is, uint64_t seed, const Allocator& allocator);
  static compact_theta_sketch_alloc deserialize_v4(uint8_t preamble_longs, std::istream& is, uint64_t seed, const Allocator& allocator);

  virtual void print_specifics(std::ostringstream& os) const;

  template<typename E, typename EK, typename P, typename S, typename CS, typename A> friend class theta_union_base;
  template<typename E, typename EK, typename P, typename S, typename CS, typename A> friend class theta_intersection_base;
  template<typename E, typename EK, typename CS, typename A> friend class theta_set_difference_base;
  compact_theta_sketch_alloc(bool is_empty, bool is_ordered, uint16_t seed_hash, uint64_t theta, std::vector<uint64_t, Allocator>&& entries);
};

/// Update Theta sketch builder
template<typename Allocator>
class update_theta_sketch_alloc<Allocator>::builder: public theta_base_builder<builder, Allocator> {
public:
    /**
     * Constructor
     * @param allocator
     */
    builder(const Allocator& allocator = Allocator());
    /// @return instance of Update Theta sketch
    update_theta_sketch_alloc build() const;
};

/**
 * Wrapped Compact Theta sketch.
 * This is to wrap a buffer containing a serialized compact sketch and use it in a set operation avoiding some cost of deserialization.
 * It does not take the ownership of the buffer.
 */
template<typename Allocator = std::allocator<uint64_t>>
class wrapped_compact_theta_sketch_alloc: public base_theta_sketch_alloc<Allocator> {
public:
  class const_iterator;

  Allocator get_allocator() const;
  bool is_empty() const;
  bool is_ordered() const;
  uint64_t get_theta64() const;
  uint32_t get_num_retained() const;
  uint16_t get_seed_hash() const;

  /**
   * Const iterator over hash values in this sketch.
   * @return begin iterator
   */
  const_iterator begin() const;

  /**
   * Const iterator pointing past the valid range.
   * Not to be incremented or dereferenced.
   * @return end iterator
   */
  const_iterator end() const;

  /**
   * This method wraps a serialized compact sketch as an array of bytes.
   * @param bytes pointer to the array of bytes
   * @param size the size of the array
   * @param seed the seed for the hash function that was used to create the sketch
   * @param dump_on_error if true prints hex dump of the input
   * @return an instance of the sketch
   */
  static const wrapped_compact_theta_sketch_alloc wrap(const void* bytes, size_t size, uint64_t seed = DEFAULT_SEED, bool dump_on_error = false);

protected:
  virtual void print_specifics(std::ostringstream& os) const;
  virtual void print_items(std::ostringstream& os) const;

private:
  using data_type = compact_theta_sketch_parser<true>::compact_theta_sketch_data;
  data_type data_;

  wrapped_compact_theta_sketch_alloc(const data_type& data);
};

template<typename Allocator>
class wrapped_compact_theta_sketch_alloc<Allocator>::const_iterator {
public:
  using iterator_category = std::input_iterator_tag;
  using value_type = const uint64_t;
  using difference_type = void;
  using pointer = value_type*;
  using reference = uint64_t;

  const_iterator(const void* ptr, uint8_t entry_bits, uint32_t num_entries, uint32_t index);
  const_iterator& operator++();
  const_iterator operator++(int);
  bool operator==(const const_iterator& other) const;
  bool operator!=(const const_iterator& other) const;
  reference operator*() const;
  pointer operator->() const;

private:
  const void* ptr_;
  uint8_t entry_bits_;
  uint32_t num_entries_;
  uint32_t index_;
  uint64_t previous_;
  bool is_block_mode_;
  uint8_t offset_;
  uint64_t buffer_[8];

  inline void unpack1();
  inline void unpack8();
};

} /* namespace datasketches */

#include "theta_sketch_impl.hpp"

#endif
