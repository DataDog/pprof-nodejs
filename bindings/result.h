/**
 * Copyright 2021 Datadog Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <string>

template<typename V, typename E = std::string>
struct Result {
  bool is_ok = false;
  union {
    V value;
    E error;
  };

  explicit Result(const V& value) : is_ok(true), value(value) {}
  explicit Result(const E& error) : is_ok(false), error(error) {}

  Result(const Result& rhs) : is_ok(rhs.is_ok) {
    if (is_ok) {
      value = rhs.value;
    } else {
      error = rhs.error;
    }
  }

  ~Result() {
    if (is_ok) {
      value.~V();
    } else {
      error.~E();
    }
  }
};

template<typename V, typename E = std::string>
const Result<V, E> Ok(const V& value) {
  return Result<V, E>(value);
}

template <typename V, typename E = std::string>
const Result<V, E> Err(const E& error) {
  return Result<V, E>(error);
}