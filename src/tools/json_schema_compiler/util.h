// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_JSON_SCHEMA_COMPILER_UTIL_H_
#define TOOLS_JSON_SCHEMA_COMPILER_UTIL_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/values.h"

namespace json_schema_compiler {

namespace util {

// Populates the item |out| from the value |from|. These are used by template
// specializations of |Get(Optional)ArrayFromList|.
bool PopulateItem(const base::Value& from, std::unique_ptr<base::Value>* out);

bool PopulateItem(const base::Value& from, int* out);
bool PopulateItem(const base::Value& from, int* out, base::string16* error);
bool PopulateItem(const base::Value& from, bool* out);
bool PopulateItem(const base::Value& from, bool* out, base::string16* error);
bool PopulateItem(const base::Value& from, double* out);
bool PopulateItem(const base::Value& from, double* out, base::string16* error);
bool PopulateItem(const base::Value& from, std::string* out);
bool PopulateItem(const base::Value& from,
                  std::string* out,
                  base::string16* error);
bool PopulateItem(const base::Value& from, std::vector<char>* out);
bool PopulateItem(const base::Value& from,
                  std::vector<char>* out,
                  base::string16* error);
bool PopulateItem(const base::Value& from,
                  std::unique_ptr<base::Value>* out,
                  base::string16* error);
bool PopulateItem(const base::Value& from, std::unique_ptr<base::Value>* out);

// This template is used for types generated by tools/json_schema_compiler.
template <class T>
bool PopulateItem(const base::Value& from, std::unique_ptr<T>* out) {
  const base::DictionaryValue* dict = nullptr;
  if (!from.GetAsDictionary(&dict))
    return false;
  std::unique_ptr<T> obj(new T());
  if (!T::Populate(*dict, obj.get()))
    return false;
  *out = std::move(obj);
  return true;
}

// This template is used for types generated by tools/json_schema_compiler.
template <class T>
bool PopulateItem(const base::Value& from, T* out) {
  const base::DictionaryValue* dict = nullptr;
  if (!from.GetAsDictionary(&dict))
    return false;
  T obj;
  if (!T::Populate(*dict, &obj))
    return false;
  *out = std::move(obj);
  return true;
}

// This template is used for types generated by tools/json_schema_compiler with
// error generation enabled.
template <class T>
bool PopulateItem(const base::Value& from,
                  std::unique_ptr<T>* out,
                  base::string16* error) {
  const base::DictionaryValue* dict = nullptr;
  if (!from.GetAsDictionary(&dict))
    return false;
  std::unique_ptr<T> obj(new T());
  if (!T::Populate(*dict, obj.get(), error))
    return false;
  *out = std::move(obj);
  return true;
}

// This template is used for types generated by tools/json_schema_compiler with
// error generation enabled.
template <class T>
bool PopulateItem(const base::Value& from, T* out, base::string16* error) {
  const base::DictionaryValue* dict = nullptr;
  if (!from.GetAsDictionary(&dict))
    return false;
  T obj;
  if (!T::Populate(*dict, &obj, error))
    return false;
  *out = std::move(obj);
  return true;
}

// Populates |out| with |list|. Returns false if there is no list at the
// specified key or if the list has anything other than |T|.
template <class T>
bool PopulateArrayFromList(const base::ListValue& list, std::vector<T>* out) {
  out->clear();
  T item;
  for (const base::Value* value : list) {
    if (!PopulateItem(*value, &item))
      return false;
    // T might not be movable, but in that case it should be copyable, and this
    // will still work.
    out->push_back(std::move(item));
  }

  return true;
}

// Populates |out| with |list|. Returns false and sets |error| if there is no
// list at the specified key or if the list has anything other than |T|.
template <class T>
bool PopulateArrayFromList(const base::ListValue& list,
                           std::vector<T>* out,
                           base::string16* error) {
  out->clear();
  T item;
  for (const base::Value* value : list) {
    if (!PopulateItem(*value, &item, error))
      return false;
    out->push_back(std::move(item));
  }

  return true;
}

// Creates a new vector containing |list| at |out|. Returns
// true on success or if there is nothing at the specified key. Returns false
// if anything other than a list of |T| is at the specified key.
template <class T>
bool PopulateOptionalArrayFromList(const base::ListValue& list,
                                   std::unique_ptr<std::vector<T>>* out) {
  out->reset(new std::vector<T>());
  if (!PopulateArrayFromList(list, out->get())) {
    out->reset();
    return false;
  }
  return true;
}

template <class T>
bool PopulateOptionalArrayFromList(const base::ListValue& list,
                                   std::unique_ptr<std::vector<T>>* out,
                                   base::string16* error) {
  out->reset(new std::vector<T>());
  if (!PopulateArrayFromList(list, out->get(), error)) {
    out->reset();
    return false;
  }
  return true;
}

// Appends a Value newly created from |from| to |out|. These used by template
// specializations of |Set(Optional)ArrayToList|.
void AddItemToList(const int from, base::ListValue* out);
void AddItemToList(const bool from, base::ListValue* out);
void AddItemToList(const double from, base::ListValue* out);
void AddItemToList(const std::string& from, base::ListValue* out);
void AddItemToList(const std::vector<char>& from, base::ListValue* out);
void AddItemToList(const std::unique_ptr<base::Value>& from,
                   base::ListValue* out);
void AddItemToList(const std::unique_ptr<base::DictionaryValue>& from,
                   base::ListValue* out);

// This template is used for types generated by tools/json_schema_compiler.
template <class T>
void AddItemToList(const std::unique_ptr<T>& from, base::ListValue* out) {
  out->Append(from->ToValue());
}

// This template is used for types generated by tools/json_schema_compiler.
template <class T>
void AddItemToList(const T& from, base::ListValue* out) {
  out->Append(from.ToValue());
}

// Set |out| to the the contents of |from|. Requires PopulateItem to be
// implemented for |T|.
template <class T>
void PopulateListFromArray(const std::vector<T>& from, base::ListValue* out) {
  out->Clear();
  for (const T& item : from)
    AddItemToList(item, out);
}

// Set |out| to the the contents of |from| if |from| is not null. Requires
// PopulateItem to be implemented for |T|.
template <class T>
void PopulateListFromOptionalArray(const std::unique_ptr<std::vector<T>>& from,
                                   base::ListValue* out) {
  if (from)
    PopulateListFromArray(*from, out);
}

template <class T>
std::unique_ptr<base::Value> CreateValueFromArray(const std::vector<T>& from) {
  std::unique_ptr<base::ListValue> list(new base::ListValue());
  PopulateListFromArray(from, list.get());
  return std::move(list);
}

template <class T>
std::unique_ptr<base::Value> CreateValueFromOptionalArray(
    const std::unique_ptr<std::vector<T>>& from) {
  if (from)
    return CreateValueFromArray(*from);
  return nullptr;
}

std::string ValueTypeToString(base::Value::Type type);

}  // namespace util
}  // namespace json_schema_compiler

#endif  // TOOLS_JSON_SCHEMA_COMPILER_UTIL_H_