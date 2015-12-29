/* -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
 * (c) 2015 Henner Zeller <h.zeller@acm.org>
 *
 * This file is part of BeagleG. http://github.com/hzeller/beagleg
 *
 * BeagleG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BeagleG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with BeagleG.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef _BEAGLEG_STRING_UTIL_H
#define _BEAGLEG_STRING_UTIL_H

#include <string>
#include <string.h>

#include <assert.h>
#include <stddef.h>

// A StringPiece essentially points at a chunk of data of a particular
// length. Pointer + length.
// It allows to have keep cheap substrings of strings without copy while still
// have a type-safe, length-aware piece of string.
class StringPiece {
public:
  typedef const char* iterator;
  StringPiece() : data_(NULL), len_(0) {}
  StringPiece(const std::string &s) : data_(s.data()), len_(s.length()) {}
  StringPiece(const char *str) : data_(str), len_(strlen(str)) {}

  StringPiece(const char *data, size_t len)
    : data_(data), len_(len) {}

  StringPiece substr(size_t pos, size_t len) const {
    assert(pos + len <= len_);
    return StringPiece(data_ + pos, len);
  }
  StringPiece substr(size_t pos) const {
    return substr(pos, length() - pos);
  }

  void assign(const char *data, size_t len) {
    data_ = data;
    len_ = len;
  }

  std::string ToString() const {
    return std::string(data_, len_);
  }

  const char operator[](size_t pos) const { return data_[pos]; }
  const char *data() const { return data_; }
  size_t length() const { return len_; }
  bool empty() const { return len_ == 0; }

  iterator begin() const { return data_; }
  iterator end() const { return data_ + len_; }

private:
  const char *data_;
  size_t len_;
};

// Trim StringPiece of whitespace font and back and returned trimmed
// string.
StringPiece TrimWhitespace(const StringPiece &s);

// Lowercase the string (simple ASCII) and return as newly allocated
// std::string
std::string ToLower(const StringPiece &in);

// Test if given StringPiece is prefix of the other.
bool HasPrefix(const StringPiece &s, const StringPiece &prefix);

// tolower, has_prefix
#endif // _BEAGLEG_STRING_UTIL_H
