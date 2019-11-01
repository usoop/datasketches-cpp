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

#ifndef CPC_SKETCH_IMPL_HPP_
#define CPC_SKETCH_IMPL_HPP_

#include <stdexcept>
#include <cmath>

#include "cpc_confidence.hpp"
#include "kxp_byte_lookup.hpp"
#include "inv_pow_2_tab.hpp"
#include "cpc_util.hpp"
#include "icon_estimator.hpp"

namespace datasketches {

template<typename A>
cpc_sketch_alloc<A>::cpc_sketch_alloc(uint8_t lg_k, uint64_t seed):
lg_k(lg_k),
seed(seed),
was_merged(false),
num_coupons(0),
surprising_value_table(2, 6 + lg_k),
sliding_window(),
window_offset(0),
first_interesting_column(0),
kxp(1 << lg_k),
hip_est_accum(0)
{
  if (lg_k < CPC_MIN_LG_K or lg_k > CPC_MAX_LG_K) {
    throw std::invalid_argument("lg_k must be >= " + std::to_string(CPC_MIN_LG_K) + " and <= " + std::to_string(CPC_MAX_LG_K) + ": " + std::to_string(lg_k));
  }
}

template<typename A>
uint8_t cpc_sketch_alloc<A>::get_lg_k() const {
  return lg_k;
}

template<typename A>
bool cpc_sketch_alloc<A>::is_empty() const {
  return num_coupons == 0;
}

template<typename A>
double cpc_sketch_alloc<A>::get_estimate() const {
  if (!was_merged) return get_hip_estimate();
  return get_icon_estimate();
}

template<typename A>
double cpc_sketch_alloc<A>::get_hip_estimate() const {
  return hip_est_accum;
}

template<typename A>
double cpc_sketch_alloc<A>::get_icon_estimate() const {
  return compute_icon_estimate(lg_k, num_coupons);
}

template<typename A>
double cpc_sketch_alloc<A>::get_lower_bound(unsigned kappa) const {
  if (kappa < 1 or kappa > 3) {
    throw std::invalid_argument("kappa must be 1, 2 or 3");
  }
  if (!was_merged) return get_hip_confidence_lb<A>(*this, kappa);
  return get_icon_confidence_lb<A>(*this, kappa);
}

template<typename A>
double cpc_sketch_alloc<A>::get_upper_bound(unsigned kappa) const {
  if (kappa < 1 or kappa > 3) {
    throw std::invalid_argument("kappa must be 1, 2 or 3");
  }
  if (!was_merged) return get_hip_confidence_ub<A>(*this, kappa);
  return get_icon_confidence_ub<A>(*this, kappa);
}

template<typename A>
void cpc_sketch_alloc<A>::update(const std::string& value) {
  if (value.empty()) return;
  update(value.c_str(), value.length());
}

template<typename A>
void cpc_sketch_alloc<A>::update(uint64_t value) {
  update(&value, sizeof(value));
}

template<typename A>
void cpc_sketch_alloc<A>::update(int64_t value) {
  update(&value, sizeof(value));
}

template<typename A>
void cpc_sketch_alloc<A>::update(uint32_t value) {
  update(static_cast<int32_t>(value));
}

template<typename A>
void cpc_sketch_alloc<A>::update(int32_t value) {
  update(static_cast<int64_t>(value));
}

template<typename A>
void cpc_sketch_alloc<A>::update(uint16_t value) {
  update(static_cast<int16_t>(value));
}

template<typename A>
void cpc_sketch_alloc<A>::update(int16_t value) {
  update(static_cast<int64_t>(value));
}

template<typename A>
void cpc_sketch_alloc<A>::update(uint8_t value) {
  update(static_cast<int8_t>(value));
}

template<typename A>
void cpc_sketch_alloc<A>::update(int8_t value) {
  update(static_cast<int64_t>(value));
}

template<typename A>
void cpc_sketch_alloc<A>::update(double value) {
  union {
    int64_t long_value;
    double double_value;
  } ldu;
  if (value == 0.0) {
    ldu.double_value = 0.0; // canonicalize -0.0 to 0.0
  } else if (std::isnan(value)) {
    ldu.long_value = 0x7ff8000000000000L; // canonicalize NaN using value from Java's Double.doubleToLongBits()
  } else {
    ldu.double_value = value;
  }
  update(&ldu, sizeof(ldu));
}

template<typename A>
void cpc_sketch_alloc<A>::update(float value) {
  update(static_cast<double>(value));
}

static inline uint32_t row_col_from_two_hashes(uint64_t hash0, uint64_t hash1, uint8_t lg_k) {
  if (lg_k > 26) throw std::logic_error("lg_k > 26");
  const uint64_t k = 1 << lg_k;
  uint8_t col = count_leading_zeros_in_u64(hash1); // 0 <= col <= 64
  if (col > 63) col = 63; // clip so that 0 <= col <= 63
  const uint32_t row = hash0 & (k - 1);
  uint32_t row_col = (row << 6) | col;
  // To avoid the hash table's "empty" value, we change the row of the following pair.
  // This case is extremely unlikely, but we might as well handle it.
  if (row_col == UINT32_MAX) row_col ^= 1 << 6;
  return row_col;
}

template<typename A>
void cpc_sketch_alloc<A>::update(const void* value, int size) {
  HashState hashes;
  MurmurHash3_x64_128(value, size, seed, hashes);
  row_col_update(row_col_from_two_hashes(hashes.h1, hashes.h2, lg_k));
}

template<typename A>
void cpc_sketch_alloc<A>::row_col_update(uint32_t row_col) {
  const uint8_t col = row_col & 63;
  if (col < first_interesting_column) return; // important speed optimization
  // window size is 0 until sketch is promoted from sparse to windowed
  if (sliding_window.size() == 0) {
    update_sparse(row_col);
  } else {
    update_windowed(row_col);
  }
}

template<typename A>
void cpc_sketch_alloc<A>::update_sparse(uint32_t row_col) {
  const uint64_t k = 1 << lg_k;
  const uint64_t c32pre = num_coupons << 5;
  if (c32pre >= 3 * k) throw std::logic_error("c32pre >= 3 * k"); // C < 3K/32, in other words flavor == SPARSE
  bool is_novel = surprising_value_table.maybe_insert(row_col);
  if (is_novel) {
    num_coupons++;
    update_hip(row_col);
    const uint64_t c32post = num_coupons << 5;
    if (c32post >= 3 * k) promote_sparse_to_windowed(); // C >= 3K/32
  }
}

// the flavor is HYBRID, PINNED, or SLIDING
template<typename A>
void cpc_sketch_alloc<A>::update_windowed(uint32_t row_col) {
  if (window_offset > 56) throw std::logic_error("wrong window offset");
  const uint64_t k = 1 << lg_k;
  const uint64_t c32pre = num_coupons << 5;
  if (c32pre < 3 * k) throw std::logic_error("c32pre < 3 * k"); // C < 3K/32, in other words flavor >= HYBRID
  const uint64_t c8pre = num_coupons << 3;
  const uint64_t w8pre = window_offset << 3;
  if (c8pre >= (27 + w8pre) * k) throw std::logic_error("c8pre is wrong"); // C < (K * 27/8) + (K * windowOffset)

  bool is_novel = false;
  const uint8_t col = row_col & 63;

  if (col < window_offset) { // track the surprising 0's "before" the window
    is_novel = surprising_value_table.maybe_delete(row_col); // inverted logic
  } else if (col < window_offset + 8) { // track the 8 bits inside the window
    if (col < window_offset) throw std::logic_error("col < window_offset");
    const uint32_t row = row_col >> 6;
    const uint8_t old_bits = sliding_window[row];
    const uint8_t new_bits = old_bits | (1 << (col - window_offset));
    if (new_bits != old_bits) {
      sliding_window[row] = new_bits;
      is_novel = true;
    }
  } else { // track the surprising 1's "after" the window
    if (col < window_offset + 8) throw std::logic_error("col < window_offset + 8");
    is_novel = surprising_value_table.maybe_insert(row_col); // normal logic
  }

  if (is_novel) {
    num_coupons++;
    update_hip(row_col);
    const uint64_t c8post = num_coupons << 3;
    if (c8post >= (27 + w8pre) * k) {
      move_window();
      if (window_offset < 1 or window_offset > 56) throw std::logic_error("wrong window offset");
      const uint64_t w8post = window_offset << 3;
      if (c8post >= (27 + w8post) * k) throw std::logic_error("c8pre is wrong"); // C < (K * 27/8) + (K * windowOffset)
    }
  }
}

// Call this whenever a new coupon has been collected.
template<typename A>
void cpc_sketch_alloc<A>::update_hip(uint32_t row_col) {
  const uint64_t k = 1 << lg_k;
  const uint8_t col = row_col & 63;
  const double one_over_p = static_cast<double>(k) / kxp;
  hip_est_accum += one_over_p;
  kxp -= INVERSE_POWERS_OF_2[col + 1]; // notice the "+1"
}

// In terms of flavor, this promotes SPARSE to HYBRID
template<typename A>
void cpc_sketch_alloc<A>::promote_sparse_to_windowed() {
  const uint64_t k = 1 << lg_k;
  const uint64_t c32 = num_coupons << 5;
  if (!(c32 == 3 * k or (lg_k == 4 and c32 > 3 * k))) throw std::logic_error("wrong c32");

  sliding_window.resize(k, 0); // zero the memory (because we will be OR'ing into it)

  u32_table<A> new_table(2, 6 + lg_k);

  const uint32_t* old_slots = surprising_value_table.get_slots();
  const size_t old_num_slots = 1 << surprising_value_table.get_lg_size();

  if (window_offset != 0) throw std::logic_error("window_offset != 0");

  for (size_t i = 0; i < old_num_slots; i++) {
    const uint32_t row_col = old_slots[i];
    if (row_col != UINT32_MAX) {
      const uint8_t col = row_col & 63;
      if (col < 8) {
        const size_t row = row_col >> 6;
        sliding_window[row] |= 1 << col;
      } else {
        // cannot use u32_table::must_insert(), because it doesn't provide for growth
        const bool is_novel = new_table.maybe_insert(row_col);
        if (!is_novel) throw std::logic_error("is_novel != true");
      }
    }
  }

  surprising_value_table = std::move(new_table);
}

template<typename A>
void cpc_sketch_alloc<A>::move_window() {
  const uint8_t new_offset = window_offset + 1;
  if (new_offset > 56) throw std::logic_error("new_offset > 56");
  if (new_offset != determine_correct_offset(lg_k, num_coupons)) throw std::logic_error("new_offset is wrong");

  if (sliding_window.size() == 0) throw std::logic_error("no sliding window");
  const uint64_t k = 1 << lg_k;

  // Construct the full-sized bit matrix that corresponds to the sketch
  uint64_t* bit_matrix = build_bit_matrix();

  // refresh the KXP register on every 8th window shift.
  if ((new_offset & 0x7) == 0) refresh_kxp(bit_matrix);

  surprising_value_table.clear(); // the new number of surprises will be about the same

  const uint64_t mask_for_clearing_window = (0xff << new_offset) ^ UINT64_MAX;
  const uint64_t mask_for_flipping_early_zone = (1 << new_offset) - 1;
  uint64_t all_surprises_ored = 0;

  for (size_t i = 0; i < k; i++) {
    uint64_t pattern = bit_matrix[i];
    sliding_window[i] = (pattern >> new_offset) & 0xff;
    pattern &= mask_for_clearing_window;
    // The following line converts surprising 0's to 1's in the "early zone",
    // (and vice versa, which is essential for this procedure's O(k) time cost).
    pattern ^= mask_for_flipping_early_zone;
    all_surprises_ored |= pattern; // a cheap way to recalculate first_interesting_column
    while (pattern != 0) {
      const uint8_t col = count_trailing_zeros_in_u64(pattern);
      pattern = pattern ^ (1 << col); // erase the 1
      const uint32_t row_col = (i << 6) | col;
      const bool is_novel = surprising_value_table.maybe_insert(row_col);
      if (!is_novel) throw std::logic_error("is_novel != true");
    }
  }

  AllocU64().deallocate(bit_matrix, k);
  window_offset = new_offset;

  first_interesting_column = count_trailing_zeros_in_u64(all_surprises_ored);
  if (first_interesting_column > new_offset) first_interesting_column = new_offset; // corner case
}

// The KXP register is a double with roughly 50 bits of precision, but
// it might need roughly 90 bits to track the value with perfect accuracy.
// Therefore we recalculate KXP occasionally from the sketch's full bitmatrix
// so that it will reflect changes that were previously outside the mantissa.
template<typename A>
void cpc_sketch_alloc<A>::refresh_kxp(const uint64_t* bit_matrix) {
  const uint64_t k = 1 << lg_k;

  // for improved numerical accuracy, we separately sum the bytes of the U64's
  double byte_sums[8]; // allocating on the stack
  std::fill(byte_sums, &byte_sums[8], 0);

  for (size_t i = 0; i < k; i++) {
    uint64_t word = bit_matrix[i];
    for (unsigned j = 0; j < 8; j++) {
      const uint8_t byte = word & 0xff;
      byte_sums[j] += KXP_BYTE_TABLE[byte];
      word >>= 8;
    }
  }

  double total = 0.0;
  for (int j = 7; j >= 0; j--) { // the reverse order is important
    const double factor = INVERSE_POWERS_OF_2[8 * j]; // pow (256.0, (-1.0 * ((double) j)));
    total += factor * byte_sums[j];
  }

  kxp = total;
}

template<typename A>
void cpc_sketch_alloc<A>::to_stream(std::ostream& os) const {
  os << "### CPC sketch summary:" << std::endl;
  os << "   lg_k           : " << std::to_string(lg_k) << std::endl;
  os << "   seed hash      : " << std::hex << compute_seed_hash(seed) << std::dec << std::endl;
  os << "   C              : " << num_coupons << std::endl;
  os << "   flavor         : " << determine_flavor() << std::endl;
  os << "   merged         : " << (was_merged ? "true" : "false") << std::endl;
  if (!was_merged) {
    os << "   HIP estimate   : " << hip_est_accum << std::endl;
    os << "   kxp            : " << kxp << std::endl;
  }
  os << "   intresting col : " << std::to_string(first_interesting_column) << std::endl;
  os << "   table entries  : " << surprising_value_table.get_num_items() << std::endl;
  os << "   window         : " << (sliding_window.size() == 0 ? "not " : "") <<  "allocated" << std::endl;
  if (sliding_window.size() > 0) {
    os << "   window offset  : " << std::to_string(window_offset) << std::endl;
  }
  os << "### End sketch summary" << std::endl;
}

template<typename A>
void cpc_sketch_alloc<A>::serialize(std::ostream& os) const {
  compressed_state compressed;
  compressed.table_data_words = 0;
  compressed.table_num_entries = 0;
  compressed.window_data_words = 0;
  get_compressor<A>().compress(*this, compressed);
  const bool has_hip = !was_merged;
  const bool has_table = compressed.table_data_ptr != nullptr;
  const bool has_window = compressed.window_data_ptr != nullptr;
  const uint8_t preamble_ints = get_preamble_ints(num_coupons, has_hip, has_table, has_window);
  os.write((char*)&preamble_ints, sizeof(preamble_ints));
  const uint8_t serial_version = SERIAL_VERSION;
  os.write((char*)&serial_version, sizeof(serial_version));
  const uint8_t family = FAMILY;
  os.write((char*)&family, sizeof(family));
  os.write((char*)&lg_k, sizeof(lg_k));
  os.write((char*)&first_interesting_column, sizeof(first_interesting_column));
  const uint8_t flags_byte(
    (1 << flags::IS_COMPRESSED)
    | (has_hip ? 1 << flags::HAS_HIP : 0)
    | (has_table ? 1 << flags::HAS_TABLE : 0)
    | (has_window ? 1 << flags::HAS_WINDOW : 0)
  );
  os.write((char*)&flags_byte, sizeof(flags_byte));
  const uint16_t seed_hash(compute_seed_hash(seed));
  os.write((char*)&seed_hash, sizeof(seed_hash));
  if (!is_empty()) {
    os.write((char*)&num_coupons, sizeof(num_coupons));
    if (has_table and has_window) {
      // if there is no window it is the same as number of coupons
      os.write((char*)&compressed.table_num_entries, sizeof(compressed.table_num_entries));
      // HIP values can be in two different places in the sequence of fields
      // this is the first HIP decision point
      if (has_hip) write_hip(os);
    }
    if (has_table) {
      os.write((char*)&compressed.table_data_words, sizeof(compressed.table_data_words));
    }
    if (has_window) {
      os.write((char*)&compressed.window_data_words, sizeof(compressed.window_data_words));
    }
    // this is the second HIP decision point
    if (has_hip and !(has_table and has_window)) write_hip(os);
    if (has_window) {
      os.write((char*)compressed.window_data_ptr.get(), compressed.window_data_words * sizeof(uint32_t));
    }
    if (has_table) {
      os.write((char*)compressed.table_data_ptr.get(), compressed.table_data_words * sizeof(uint32_t));
    }
  }
}

template<typename A>
std::pair<void_ptr_with_deleter, const size_t> cpc_sketch_alloc<A>::serialize(unsigned header_size_bytes) const {
  compressed_state compressed;
  compressed.table_data_words = 0;
  compressed.table_num_entries = 0;
  compressed.window_data_words = 0;
  get_compressor<A>().compress(*this, compressed);
  const bool has_hip = !was_merged;
  const bool has_table = compressed.table_data_ptr != nullptr;
  const bool has_window = compressed.window_data_ptr != nullptr;
  const uint8_t preamble_ints = get_preamble_ints(num_coupons, has_hip, has_table, has_window);
  const size_t size = header_size_bytes + (preamble_ints + compressed.table_data_words + compressed.window_data_words) * sizeof(uint32_t);
  void_ptr_with_deleter data_ptr(
      AllocU8().allocate(size),
      [size](void* ptr) { AllocU8().deallocate(static_cast<uint8_t*>(ptr), size); }
  );
  char* ptr = static_cast<char*>(data_ptr.get()) + header_size_bytes;
  ptr += copy_to_mem(ptr, &preamble_ints, sizeof(preamble_ints));
  const uint8_t serial_version = SERIAL_VERSION;
  ptr += copy_to_mem(ptr, &serial_version, sizeof(serial_version));
  const uint8_t family = FAMILY;
  ptr += copy_to_mem(ptr, &family, sizeof(family));
  ptr += copy_to_mem(ptr, &lg_k, sizeof(lg_k));
  ptr += copy_to_mem(ptr, &first_interesting_column, sizeof(first_interesting_column));
  const uint8_t flags_byte(
    (1 << flags::IS_COMPRESSED)
    | (has_hip ? 1 << flags::HAS_HIP : 0)
    | (has_table ? 1 << flags::HAS_TABLE : 0)
    | (has_window ? 1 << flags::HAS_WINDOW : 0)
  );
  ptr += copy_to_mem(ptr, &flags_byte, sizeof(flags_byte));
  const uint16_t seed_hash = compute_seed_hash(seed);
  ptr += copy_to_mem(ptr, &seed_hash, sizeof(seed_hash));
  if (!is_empty()) {
    ptr += copy_to_mem(ptr, &num_coupons, sizeof(num_coupons));
    if (has_table and has_window) {
      // if there is no window it is the same as number of coupons
      ptr += copy_to_mem(ptr, &compressed.table_num_entries, sizeof(compressed.table_num_entries));
      // HIP values can be in two different places in the sequence of fields
      // this is the first HIP decision point
      if (has_hip) ptr += copy_hip_to_mem(ptr);
    }
    if (has_table) {
      ptr += copy_to_mem(ptr, &compressed.table_data_words, sizeof(compressed.table_data_words));
    }
    if (has_window) {
      ptr += copy_to_mem(ptr, &compressed.window_data_words, sizeof(compressed.window_data_words));
    }
    // this is the second HIP decision point
    if (has_hip and !(has_table and has_window)) ptr += copy_hip_to_mem(ptr);
    if (has_window) {
      ptr += copy_to_mem(ptr, compressed.window_data_ptr.get(), compressed.window_data_words * sizeof(uint32_t));
    }
    if (has_table) {
      ptr += copy_to_mem(ptr, compressed.table_data_ptr.get(), compressed.table_data_words * sizeof(uint32_t));
    }
  }
  if (ptr != static_cast<char*>(data_ptr.get()) + size) throw std::logic_error("serialized size mismatch");
  return std::make_pair(std::move(data_ptr), size);
}

template<typename A>
cpc_sketch_alloc<A> cpc_sketch_alloc<A>::deserialize(std::istream& is, uint64_t seed) {
  uint8_t preamble_ints;
  is.read((char*)&preamble_ints, sizeof(preamble_ints));
  uint8_t serial_version;
  is.read((char*)&serial_version, sizeof(serial_version));
  uint8_t family_id;
  is.read((char*)&family_id, sizeof(family_id));
  uint8_t lg_k;
  is.read((char*)&lg_k, sizeof(lg_k));
  uint8_t first_interesting_column;
  is.read((char*)&first_interesting_column, sizeof(first_interesting_column));
  uint8_t flags_byte;
  is.read((char*)&flags_byte, sizeof(flags_byte));
  uint16_t seed_hash;
  is.read((char*)&seed_hash, sizeof(seed_hash));
  const bool has_hip = flags_byte & (1 << flags::HAS_HIP);
  const bool has_table = flags_byte & (1 << flags::HAS_TABLE);
  const bool has_window = flags_byte & (1 << flags::HAS_WINDOW);
  compressed_state compressed;
  compressed.table_data_words = 0;
  compressed.table_num_entries = 0;
  compressed.window_data_words = 0;
  uint32_t num_coupons = 0;
  double kxp = 0;
  double hip_est_accum = 0;
  if (has_table or has_window) {
    is.read((char*)&num_coupons, sizeof(num_coupons));
    if (has_table and has_window) {
      is.read((char*)&compressed.table_num_entries, sizeof(compressed.table_num_entries));
      if (has_hip) {
        is.read((char*)&kxp, sizeof(kxp));
        is.read((char*)&hip_est_accum, sizeof(hip_est_accum));
      }
    }
    if (has_table) {
      is.read((char*)&compressed.table_data_words, sizeof(compressed.table_data_words));
    }
    if (has_window) {
      is.read((char*)&compressed.window_data_words, sizeof(compressed.window_data_words));
    }
    if (has_hip and !(has_table and has_window)) {
      is.read((char*)&kxp, sizeof(kxp));
      is.read((char*)&hip_est_accum, sizeof(hip_est_accum));
    }
    if (has_window) {
      const size_t len = compressed.window_data_words;
      compressed.window_data_ptr = u32_ptr_with_deleter(AllocU32().allocate(len), [len] (uint32_t* ptr) { AllocU32().deallocate(ptr, len); });
      is.read((char*)compressed.window_data_ptr.get(), compressed.window_data_words * sizeof(uint32_t));
    }
    if (has_table) {
      const size_t len = compressed.table_data_words;
      compressed.table_data_ptr = u32_ptr_with_deleter(AllocU32().allocate(len), [len] (uint32_t* ptr) { AllocU32().deallocate(ptr, len); });
      is.read((char*)compressed.table_data_ptr.get(), compressed.table_data_words * sizeof(uint32_t));
    }
    if (!has_window) compressed.table_num_entries = num_coupons;
  }

  uint8_t expected_preamble_ints = get_preamble_ints(num_coupons, has_hip, has_table, has_window);
  if (preamble_ints != expected_preamble_ints) {
    throw std::invalid_argument("Possible corruption: preamble ints: expected "
        + std::to_string(expected_preamble_ints) + ", got " + std::to_string(preamble_ints));
  }
  if (serial_version != SERIAL_VERSION) {
    throw std::invalid_argument("Possible corruption: serial version: expected "
        + std::to_string(SERIAL_VERSION) + ", got " + std::to_string(serial_version));
  }
  if (family_id != FAMILY) {
    throw std::invalid_argument("Possible corruption: family: expected "
        + std::to_string(FAMILY) + ", got " + std::to_string(family_id));
  }
  if (seed_hash != compute_seed_hash(seed)) {
    throw std::invalid_argument("Incompatible seed hashes: " + std::to_string(seed_hash) + ", "
        + std::to_string(compute_seed_hash(seed)));
  }
  uncompressed_state<A> uncompressed;
  get_compressor<A>().uncompress(compressed, uncompressed, lg_k, num_coupons);
  return cpc_sketch_alloc(lg_k, num_coupons, first_interesting_column, std::move(uncompressed.table),
      std::move(uncompressed.window), has_hip, kxp, hip_est_accum, seed);
}

template<typename A>
cpc_sketch_alloc<A> cpc_sketch_alloc<A>::deserialize(const void* bytes, size_t size, uint64_t seed) {
  const char* ptr = static_cast<const char*>(bytes);
  uint8_t preamble_ints;
  ptr += copy_from_mem(ptr, &preamble_ints, sizeof(preamble_ints));
  uint8_t serial_version;
  ptr += copy_from_mem(ptr, &serial_version, sizeof(serial_version));
  uint8_t family_id;
  ptr += copy_from_mem(ptr, &family_id, sizeof(family_id));
  uint8_t lg_k;
  ptr += copy_from_mem(ptr, &lg_k, sizeof(lg_k));
  uint8_t first_interesting_column;
  ptr += copy_from_mem(ptr, &first_interesting_column, sizeof(first_interesting_column));
  uint8_t flags_byte;
  ptr += copy_from_mem(ptr, &flags_byte, sizeof(flags_byte));
  uint16_t seed_hash;
  ptr += copy_from_mem(ptr, &seed_hash, sizeof(seed_hash));
  const bool has_hip = flags_byte & (1 << flags::HAS_HIP);
  const bool has_table = flags_byte & (1 << flags::HAS_TABLE);
  const bool has_window = flags_byte & (1 << flags::HAS_WINDOW);
  compressed_state compressed;
  compressed.table_data_words = 0;
  compressed.table_num_entries = 0;
  compressed.window_data_words = 0;
  uint32_t num_coupons = 0;
  double kxp = 0;
  double hip_est_accum = 0;
  if (has_table or has_window) {
    ptr += copy_from_mem(ptr, &num_coupons, sizeof(num_coupons));
    if (has_table and has_window) {
      ptr += copy_from_mem(ptr, &compressed.table_num_entries, sizeof(compressed.table_num_entries));
      if (has_hip) {
        ptr += copy_from_mem(ptr, &kxp, sizeof(kxp));
        ptr += copy_from_mem(ptr, &hip_est_accum, sizeof(hip_est_accum));
      }
    }
    if (has_table) {
      ptr += copy_from_mem(ptr, &compressed.table_data_words, sizeof(compressed.table_data_words));
    }
    if (has_window) {
      ptr += copy_from_mem(ptr, &compressed.window_data_words, sizeof(compressed.window_data_words));
    }
    if (has_hip and !(has_table and has_window)) {
      ptr += copy_from_mem(ptr, &kxp, sizeof(kxp));
      ptr += copy_from_mem(ptr, &hip_est_accum, sizeof(hip_est_accum));
    }
    if (has_window) {
      const size_t len = compressed.window_data_words;
      compressed.window_data_ptr = u32_ptr_with_deleter(AllocU32().allocate(len), [len] (uint32_t* ptr) { AllocU32().deallocate(ptr, len); });
      ptr += copy_from_mem(ptr, compressed.window_data_ptr.get(), compressed.window_data_words * sizeof(uint32_t));
    }
    if (has_table) {
      const size_t len = compressed.table_data_words;
      compressed.table_data_ptr = u32_ptr_with_deleter(AllocU32().allocate(len), [len] (uint32_t* ptr) { AllocU32().deallocate(ptr, len); });
      ptr += copy_from_mem(ptr, compressed.table_data_ptr.get(), compressed.table_data_words * sizeof(uint32_t));
    }
    if (!has_window) compressed.table_num_entries = num_coupons;
  }
  if (ptr != static_cast<const char*>(bytes) + size) throw std::logic_error("deserialized size mismatch");

  uint8_t expected_preamble_ints = get_preamble_ints(num_coupons, has_hip, has_table, has_window);
  if (preamble_ints != expected_preamble_ints) {
    throw std::invalid_argument("Possible corruption: preamble ints: expected "
        + std::to_string(expected_preamble_ints) + ", got " + std::to_string(preamble_ints));
  }
  if (serial_version != SERIAL_VERSION) {
    throw std::invalid_argument("Possible corruption: serial version: expected "
        + std::to_string(SERIAL_VERSION) + ", got " + std::to_string(serial_version));
  }
  if (family_id != FAMILY) {
    throw std::invalid_argument("Possible corruption: family: expected "
        + std::to_string(FAMILY) + ", got " + std::to_string(family_id));
  }
  if (seed_hash != compute_seed_hash(seed)) {
    throw std::invalid_argument("Incompatible seed hashes: " + std::to_string(seed_hash) + ", "
        + std::to_string(compute_seed_hash(seed)));
  }
  uncompressed_state<A> uncompressed;
  get_compressor<A>().uncompress(compressed, uncompressed, lg_k, num_coupons);
  return cpc_sketch_alloc(lg_k, num_coupons, first_interesting_column, std::move(uncompressed.table),
      std::move(uncompressed.window), has_hip, kxp, hip_est_accum, seed);
}

template<typename A>
uint32_t cpc_sketch_alloc<A>::get_num_coupons() const {
  return num_coupons;
}

template<typename A>
bool cpc_sketch_alloc<A>::validate() const {
  uint64_t* bit_matrix = build_bit_matrix();
  const uint64_t num_bits_set = count_bits_set_in_matrix(bit_matrix, 1 << lg_k);
  AllocU64().deallocate(bit_matrix, 1 << lg_k);
  return num_bits_set == num_coupons;
}

template<typename A>
cpc_sketch_alloc<A>::cpc_sketch_alloc(uint8_t lg_k, uint32_t num_coupons, uint8_t first_interesting_column,
    u32_table<A>&& table, window_type&& window, bool has_hip, double kxp, double hip_est_accum, uint64_t seed):
lg_k(lg_k),
seed(seed),
was_merged(!has_hip),
num_coupons(num_coupons),
surprising_value_table(std::move(table)),
sliding_window(std::move(window)),
window_offset(determine_correct_offset(lg_k, num_coupons)),
first_interesting_column(first_interesting_column),
kxp(kxp),
hip_est_accum(hip_est_accum)
{}

template<typename A>
uint8_t cpc_sketch_alloc<A>::get_preamble_ints(uint32_t num_coupons, bool has_hip, bool has_table, bool has_window) {
  uint8_t preamble_ints = 2;
  if (num_coupons > 0) {
    preamble_ints += 1; // number of coupons
    if (has_hip) {
      preamble_ints += 4; // HIP
    }
    if (has_table) {
      preamble_ints += 1; // table data length
      // number of values (if there is no window it is the same as number of coupons)
      if (has_window) {
        preamble_ints += 1;
      }
    }
    if (has_window) {
      preamble_ints += 1; // window length
    }
  }
  return preamble_ints;
}

template<typename A>
typename cpc_sketch_alloc<A>::flavor cpc_sketch_alloc<A>::determine_flavor() const {
  return determine_flavor(lg_k, num_coupons);
}

template<typename A>
typename cpc_sketch_alloc<A>::flavor cpc_sketch_alloc<A>::determine_flavor(uint8_t lg_k, uint64_t c) {
  const uint64_t k = 1 << lg_k;
  const uint64_t c2 = c << 1;
  const uint64_t c8 = c << 3;
  const uint64_t c32 = c << 5;
  if (c == 0)      return EMPTY;    //    0  == C <    1
  if (c32 < 3 * k) return SPARSE;   //    1  <= C <   3K/32
  if (c2 < k)      return HYBRID;   // 3K/32 <= C <   K/2
  if (c8 < 27 * k) return PINNED;   //   K/2 <= C < 27K/8
  else             return SLIDING;  // 27K/8 <= C
}

template<typename A>
uint8_t cpc_sketch_alloc<A>::determine_correct_offset(uint8_t lg_k, uint64_t c) {
  const uint64_t k = 1 << lg_k;
  const int64_t tmp = static_cast<int64_t>(c << 3) - static_cast<int64_t>(19 * k); // 8C - 19K
  if (tmp < 0) return 0;
  return tmp >> (lg_k + 3); // tmp / 8K
}

template<typename A>
uint64_t* cpc_sketch_alloc<A>::build_bit_matrix() const {
  const size_t k = 1 << lg_k;
  if (window_offset > 56) throw std::logic_error("offset > 56");
  uint64_t* matrix = AllocU64().allocate(k);

  // Fill the matrix with default rows in which the "early zone" is filled with ones.
  // This is essential for the routine's O(k) time cost (as opposed to O(C)).
  const uint64_t default_row = (1 << window_offset) - 1;
  std::fill(matrix, &matrix[k], default_row);

  if (num_coupons == 0) return matrix;

  if (sliding_window.size() > 0) { // In other words, we are in window mode, not sparse mode
    for (size_t i = 0; i < k; i++) { // set the window bits, trusting the sketch's current offset
      matrix[i] |= static_cast<uint64_t>(sliding_window[i]) << window_offset;
    }
  }

  const uint32_t* slots = surprising_value_table.get_slots();
  const size_t num_slots = 1 << surprising_value_table.get_lg_size();
  for (size_t i = 0; i < num_slots; i++) {
    const uint32_t row_col = slots[i];
    if (row_col != UINT32_MAX) {
      const uint8_t col = row_col & 63;
      const size_t row = row_col >> 6;
      // Flip the specified matrix bit from its default value.
      // In the "early" zone the bit changes from 1 to 0.
      // In the "late" zone the bit changes from 0 to 1.
      matrix[row] ^= 1 << col;
    }
  }
  return matrix;
}

template<typename A>
void cpc_sketch_alloc<A>::write_hip(std::ostream& os) const {
  os.write((char*)&kxp, sizeof(kxp));
  os.write((char*)&hip_est_accum, sizeof(hip_est_accum));
}

template<typename A>
size_t cpc_sketch_alloc<A>::copy_hip_to_mem(void* dst) const {
  memcpy(dst, &kxp, sizeof(kxp));
  memcpy(static_cast<char*>(dst) + sizeof(kxp), &hip_est_accum, sizeof(hip_est_accum));
  return sizeof(kxp) + sizeof(hip_est_accum);
}

template<typename A>
size_t cpc_sketch_alloc<A>::copy_to_mem(void* dst, const void* src, size_t size) {
  memcpy(dst, src, size);
  return size;
}

template<typename A>
size_t cpc_sketch_alloc<A>::copy_from_mem(const void* src, void* dst, size_t size) {
  memcpy(dst, src, size);
  return size;
}

} /* namespace datasketches */

#endif
