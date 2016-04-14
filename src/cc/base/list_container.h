// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_BASE_LIST_CONTAINER_H_
#define CC_BASE_LIST_CONTAINER_H_

#include <stddef.h>

#include <memory>

#include "base/logging.h"
#include "base/macros.h"
#include "cc/base/cc_export.h"
#include "cc/base/list_container_helper.h"

namespace cc {

// ListContainer is a container type that handles allocating contiguous memory
// for new elements and traversing through elements with either iterator or
// reverse iterator. Since this container hands out raw pointers of its
// elements, it is very important that this container never reallocate its
// memory so those raw pointer will continue to be valid.  This class is used to
// contain SharedQuadState or DrawQuad. Since the size of each DrawQuad varies,
// to hold DrawQuads, the allocations size of each element in this class is
// LargestDrawQuadSize while BaseElementType is DrawQuad.
template <class BaseElementType>
class ListContainer {
 public:
  // BaseElementType is the type of raw pointers this class hands out; however,
  // its derived classes might require different memory sizes.
  // max_size_for_derived_class the largest memory size required for all the
  // derived classes to use for allocation.
  explicit ListContainer(size_t max_size_for_derived_class)
      : helper_(max_size_for_derived_class) {}

  // This constructor omits input variable for max_size_for_derived_class. This
  // is used when there is no derived classes from BaseElementType we need to
  // worry about, and allocation size is just sizeof(BaseElementType).
  ListContainer() : helper_(sizeof(BaseElementType)) {}

  // This constructor reserves the requested memory up front so only single
  // allocation is needed. When num_of_elements_to_reserve_for is zero, use the
  // default size.
  ListContainer(size_t max_size_for_derived_class,
                size_t num_of_elements_to_reserve_for)
      : helper_(max_size_for_derived_class, num_of_elements_to_reserve_for) {}

  ~ListContainer() {
    for (Iterator i = begin(); i != end(); ++i) {
      i->~BaseElementType();
    }
  }

  class Iterator;
  class ConstIterator;
  class ReverseIterator;
  class ConstReverseIterator;

  // Removes the last element of the list and makes its space available for
  // allocation.
  void RemoveLast() {
    DCHECK(!empty());
    back()->~BaseElementType();
    helper_.RemoveLast();
  }

  // When called, all raw pointers that have been handed out are no longer
  // valid. Use with caution.
  // Returns a valid Iterator pointing to the element after the erased element.
  // This function does not deallocate memory.
  Iterator EraseAndInvalidateAllPointers(Iterator position) {
    BaseElementType* item = *position;
    item->~BaseElementType();
    helper_.EraseAndInvalidateAllPointers(&position);
    return empty() ? end() : position;
  }

  ConstReverseIterator crbegin() const {
    return ConstReverseIterator(helper_.crbegin());
  }
  ConstReverseIterator crend() const {
    return ConstReverseIterator(helper_.crend());
  }
  ConstReverseIterator rbegin() const { return crbegin(); }
  ConstReverseIterator rend() const { return crend(); }
  ReverseIterator rbegin() { return ReverseIterator(helper_.rbegin()); }
  ReverseIterator rend() { return ReverseIterator(helper_.rend()); }
  ConstIterator cbegin() const { return ConstIterator(helper_.cbegin()); }
  ConstIterator cend() const { return ConstIterator(helper_.cend()); }
  ConstIterator begin() const { return cbegin(); }
  ConstIterator end() const { return cend(); }
  Iterator begin() { return Iterator(helper_.begin()); }
  Iterator end() { return Iterator(helper_.end()); }

  // TODO(weiliangc): front(), back() and ElementAt() function should return
  // reference, consistent with container-of-object.
  BaseElementType* front() { return *begin(); }
  BaseElementType* back() { return *rbegin(); }
  const BaseElementType* front() const { return *begin(); }
  const BaseElementType* back() const { return *rbegin(); }

  BaseElementType* ElementAt(size_t index) {
    return *Iterator(helper_.IteratorAt(index));
  }
  const BaseElementType* ElementAt(size_t index) const {
    return *ConstIterator(helper_.IteratorAt(index));
  }

  // Take in derived element type and construct it at location generated by
  // Allocate().
  template <typename DerivedElementType>
  DerivedElementType* AllocateAndConstruct() {
    return new (helper_.Allocate(sizeof(DerivedElementType)))
        DerivedElementType;
  }

  // Take in derived element type and copy construct it at location generated by
  // Allocate().
  template <typename DerivedElementType>
  DerivedElementType* AllocateAndCopyFrom(const DerivedElementType* source) {
    return new (helper_.Allocate(sizeof(DerivedElementType)))
        DerivedElementType(*source);
  }

  // Construct a new element on top of an existing one.
  template <typename DerivedElementType>
  DerivedElementType* ReplaceExistingElement(Iterator at) {
    at->~BaseElementType();
    return new (at.item_iterator) DerivedElementType();
  }

  // Insert |count| new elements of |DerivedElementType| before |at|. This will
  // invalidate all outstanding pointers and iterators. Return a valid iterator
  // for the beginning of the newly inserted segment.
  template <typename DerivedElementType>
  Iterator InsertBeforeAndInvalidateAllPointers(Iterator at, size_t count) {
    helper_.InsertBeforeAndInvalidateAllPointers(&at, count);
    Iterator result = at;
    for (size_t i = 0; i < count; ++i) {
      new (at.item_iterator) DerivedElementType();
      ++at;
    }
    return result;
  }

  template <typename DerivedElementType>
  void swap(ListContainer<DerivedElementType>& other) {
    helper_.data_.swap(other.helper_.data_);
  }

  // Appends a new item without copying. The original item will not be
  // destructed and will be replaced with a new DerivedElementType. The
  // DerivedElementType does not have to match the moved type as a full block
  // of memory will be moved (up to MaxSizeForDerivedClass()). A pointer to
  // the moved element is returned.
  template <typename DerivedElementType>
  DerivedElementType* AppendByMoving(DerivedElementType* item) {
    size_t max_size_for_derived_class = helper_.MaxSizeForDerivedClass();
    void* new_item = helper_.Allocate(max_size_for_derived_class);
    memcpy(new_item, static_cast<void*>(item), max_size_for_derived_class);
    // Construct a new element in-place so it can be destructed safely.
    new (item) DerivedElementType;
    return static_cast<DerivedElementType*>(new_item);
  }

  size_t size() const { return helper_.size(); }
  bool empty() const { return helper_.empty(); }
  size_t GetCapacityInBytes() const { return helper_.GetCapacityInBytes(); }

  void clear() {
    for (Iterator i = begin(); i != end(); ++i) {
      i->~BaseElementType();
    }
    helper_.clear();
  }

  size_t AvailableSizeWithoutAnotherAllocationForTesting() const {
    return helper_.AvailableSizeWithoutAnotherAllocationForTesting();
  }

  // Iterator classes that can be used to access data.
  /////////////////////////////////////////////////////////////////
  class Iterator : public ListContainerHelper::Iterator {
    // This class is only defined to forward iterate through
    // CharAllocator.
   public:
    Iterator(ListContainerHelper::CharAllocator* container,
             size_t vector_ind,
             char* item_iter,
             size_t index)
        : ListContainerHelper::Iterator(container,
                                        vector_ind,
                                        item_iter,
                                        index) {}
    BaseElementType* operator->() const {
      return reinterpret_cast<BaseElementType*>(item_iterator);
    }
    BaseElementType* operator*() const {
      return reinterpret_cast<BaseElementType*>(item_iterator);
    }
    Iterator operator++(int unused_post_increment) {
      Iterator tmp = *this;
      operator++();
      return tmp;
    }
    Iterator& operator++() {
      Increment();
      ++index_;
      return *this;
    }

   private:
    explicit Iterator(const ListContainerHelper::Iterator& base_iterator)
        : ListContainerHelper::Iterator(base_iterator) {}
    friend Iterator ListContainer<BaseElementType>::begin();
    friend Iterator ListContainer<BaseElementType>::end();
    friend BaseElementType* ListContainer<BaseElementType>::ElementAt(
        size_t index);
  };

  class ConstIterator : public ListContainerHelper::ConstIterator {
    // This class is only defined to forward iterate through
    // CharAllocator.
   public:
    ConstIterator(ListContainerHelper::CharAllocator* container,
                  size_t vector_ind,
                  char* item_iter,
                  size_t index)
        : ListContainerHelper::ConstIterator(container,
                                             vector_ind,
                                             item_iter,
                                             index) {}
    ConstIterator(const Iterator& other)  // NOLINT
        : ListContainerHelper::ConstIterator(other) {}
    const BaseElementType* operator->() const {
      return reinterpret_cast<const BaseElementType*>(item_iterator);
    }
    const BaseElementType* operator*() const {
      return reinterpret_cast<const BaseElementType*>(item_iterator);
    }
    ConstIterator operator++(int unused_post_increment) {
      ConstIterator tmp = *this;
      operator++();
      return tmp;
    }
    ConstIterator& operator++() {
      Increment();
      ++index_;
      return *this;
    }

   private:
    explicit ConstIterator(
        const ListContainerHelper::ConstIterator& base_iterator)
        : ListContainerHelper::ConstIterator(base_iterator) {}
    friend ConstIterator ListContainer<BaseElementType>::cbegin() const;
    friend ConstIterator ListContainer<BaseElementType>::cend() const;
    friend const BaseElementType* ListContainer<BaseElementType>::ElementAt(
        size_t index) const;
  };

  class ReverseIterator : public ListContainerHelper::ReverseIterator {
    // This class is only defined to reverse iterate through
    // CharAllocator.
   public:
    ReverseIterator(ListContainerHelper::CharAllocator* container,
                    size_t vector_ind,
                    char* item_iter,
                    size_t index)
        : ListContainerHelper::ReverseIterator(container,
                                               vector_ind,
                                               item_iter,
                                               index) {}
    BaseElementType* operator->() const {
      return reinterpret_cast<BaseElementType*>(item_iterator);
    }
    BaseElementType* operator*() const {
      return reinterpret_cast<BaseElementType*>(item_iterator);
    }
    ReverseIterator operator++(int unused_post_increment) {
      ReverseIterator tmp = *this;
      operator++();
      return tmp;
    }
    ReverseIterator& operator++() {
      ReverseIncrement();
      ++index_;
      return *this;
    }

   private:
    explicit ReverseIterator(ListContainerHelper::ReverseIterator base_iterator)
        : ListContainerHelper::ReverseIterator(base_iterator) {}
    friend ReverseIterator ListContainer<BaseElementType>::rbegin();
    friend ReverseIterator ListContainer<BaseElementType>::rend();
  };

  class ConstReverseIterator
      : public ListContainerHelper::ConstReverseIterator {
    // This class is only defined to reverse iterate through
    // CharAllocator.
   public:
    ConstReverseIterator(ListContainerHelper::CharAllocator* container,
                         size_t vector_ind,
                         char* item_iter,
                         size_t index)
        : ListContainerHelper::ConstReverseIterator(container,
                                                    vector_ind,
                                                    item_iter,
                                                    index) {}
    ConstReverseIterator(const ReverseIterator& other)  // NOLINT
        : ListContainerHelper::ConstReverseIterator(other) {}
    const BaseElementType* operator->() const {
      return reinterpret_cast<const BaseElementType*>(item_iterator);
    }
    const BaseElementType* operator*() const {
      return reinterpret_cast<const BaseElementType*>(item_iterator);
    }
    ConstReverseIterator operator++(int unused_post_increment) {
      ConstReverseIterator tmp = *this;
      operator++();
      return tmp;
    }
    ConstReverseIterator& operator++() {
      ReverseIncrement();
      ++index_;
      return *this;
    }

   private:
    explicit ConstReverseIterator(
        ListContainerHelper::ConstReverseIterator base_iterator)
        : ListContainerHelper::ConstReverseIterator(base_iterator) {}
    friend ConstReverseIterator ListContainer<BaseElementType>::crbegin() const;
    friend ConstReverseIterator ListContainer<BaseElementType>::crend() const;
  };

 private:
  ListContainerHelper helper_;

  DISALLOW_COPY_AND_ASSIGN(ListContainer);
};

}  // namespace cc

#endif  // CC_BASE_LIST_CONTAINER_H_