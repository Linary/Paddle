/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

#pragma once

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <mutex>  // NOLINT
#include <utility>
#include <vector>
#include "paddle/fluid/framework/details/cow_ptr.h"
#include "paddle/fluid/framework/tensor.h"
#include "paddle/fluid/framework/tensor_util.h"
#include "paddle/fluid/memory/memcpy.h"

#include "glog/logging.h"

namespace paddle {
namespace framework {

#if defined(PADDLE_WITH_CUDA)
namespace details {
struct CUDABuffer {
  void *data_{nullptr};
  size_t size_{0};
  platform::CUDAPlace place_;

  CUDABuffer() {}
  CUDABuffer(platform::Place place, size_t size)
      : size_(size), place_(boost::get<platform::CUDAPlace>(place)) {
    data_ = memory::Alloc(place_, size);
  }

  ~CUDABuffer() { ClearMemory(); }

  CUDABuffer(const CUDABuffer &o) = delete;
  CUDABuffer &operator=(const CUDABuffer &o) = delete;

  void Resize(platform::Place place, size_t size) {
    ClearMemory();
    place_ = boost::get<platform::CUDAPlace>(place);
    data_ = memory::Alloc(place_, size);
    PADDLE_ENFORCE_NOT_NULL(data_);
    size_ = size;
  }

  void Swap(CUDABuffer &o) {
    std::swap(data_, o.data_);
    std::swap(place_, o.place_);
    std::swap(size_, o.size_);
  }

 private:
  void ClearMemory() const {
    if (data_ != nullptr) {
      memory::Free(place_, data_);
    }
  }
};
}  // namespace details

// Vector<T> implements the std::vector interface, and can get Data or
// MutableData from any place. The data will be synced implicitly inside.
template <typename T>
class Vector {
 public:
  using value_type = T;
  using iterator = typename std::vector<T>::iterator;
  using const_iterator = typename std::vector<T>::const_iterator;

 private:
  // The actual class to implement vector logic
  class VectorData {
   public:
    VectorData() : flag_(kDataInCPU) {}
    VectorData(size_t count, const T &value)
        : cpu_(count, value), flag_(kDataInCPU) {}
    VectorData(std::initializer_list<T> init) : cpu_(init), flag_(kDataInCPU) {}
    template <typename U>
    explicit VectorData(const std::vector<U> &dat)
        : cpu_(dat), flag_(kDataInCPU) {}
    ~VectorData() {}

    VectorData(const VectorData &o) {
      o.ImmutableCPU();
      cpu_ = o.cpu_;
      flag_ = kDataInCPU;
    }

    VectorData &operator=(const VectorData &o) {
      o.ImmutableCPU();
      cpu_ = o.cpu_;
      flag_ = kDataInCPU;
      details::CUDABuffer null;
      gpu_.Swap(null);
      return *this;
    }

    T &operator[](size_t i) {
      MutableCPU();
      return cpu_[i];
    }

    const T &operator[](size_t i) const {
      ImmutableCPU();
      return cpu_[i];
    }

    size_t size() const { return cpu_.size(); }

    iterator begin() {
      MutableCPU();
      return cpu_.begin();
    }

    iterator end() {
      MutableCPU();
      return cpu_.end();
    }

    T &front() {
      MutableCPU();
      return cpu_.front();
    }

    T &back() {
      MutableCPU();
      return cpu_.back();
    }

    const_iterator begin() const {
      ImmutableCPU();
      return cpu_.begin();
    }

    const_iterator end() const {
      ImmutableCPU();
      return cpu_.end();
    }

    const T &back() const {
      ImmutableCPU();
      return cpu_.back();
    }

    T *data() { return &(*this)[0]; }

    const T *data() const { return &(*this)[0]; }

    const T &front() const {
      ImmutableCPU();
      return cpu_.front();
    }

    // assign this from iterator.
    // NOTE: the iterator must support `end-begin`
    template <typename Iter>
    void assign(Iter begin, Iter end) {
      MutableCPU();
      cpu_.assign(begin, end);
    }

    // push_back. If the previous capacity is not enough, the memory will
    // double.
    void push_back(T elem) {
      MutableCPU();
      cpu_.push_back(elem);
    }

    // extend a vector by iterator.
    // NOTE: the iterator must support end-begin
    template <typename It>
    void Extend(It begin, It end) {
      MutableCPU();
      auto out_it = std::back_inserter<std::vector<T>>(this->cpu_);
      std::copy(begin, end, out_it);
    }

    // resize the vector
    void resize(size_t size) {
      MutableCPU();
      cpu_.resize(size);
    }

    // get cuda ptr. immutable
    const T *CUDAData(platform::Place place) const {
      PADDLE_ENFORCE(platform::is_gpu_place(place),
                     "CUDA Data must on CUDA place");
      ImmutableCUDA(place);
      return reinterpret_cast<T *>(gpu_.data_);
    }

    // get cuda ptr. mutable
    T *CUDAMutableData(platform::Place place) {
      const T *ptr = CUDAData(place);
      flag_ = kDirty | kDataInCUDA;
      return const_cast<T *>(ptr);
    }

    // clear
    void clear() {
      cpu_.clear();
      flag_ = kDirty | kDataInCPU;
    }

    size_t capacity() const { return cpu_.capacity(); }

    // reserve data
    void reserve(size_t size) const { cpu_.reserve(size); }

    // implicit cast operator. Vector can be cast to std::vector implicitly.
    operator std::vector<T>() const {
      ImmutableCPU();
      return cpu_;
    }

    bool operator==(const VectorData &other) const {
      ImmutableCPU();
      other.ImmutableCPU();
      return cpu_ == other.cpu_;
    }

    std::mutex &Mutex() const { return mtx_; }

    std::unique_ptr<platform::CUDAPlace> CUDAPlace() const {
      if (gpu_.data_ == nullptr) {
        return nullptr;
      } else {
        return std::unique_ptr<platform::CUDAPlace>(
            new platform::CUDAPlace(gpu_.place_));
      }
    }

   private:
    enum DataFlag {
      kDataInCPU = 0x01,
      kDataInCUDA = 0x02,
      // kDirty means the data has been changed in one device.
      kDirty = 0x10
    };

    void CopyToCPU() const {
      // COPY GPU Data To CPU
      auto *dev_ctx = static_cast<platform::CUDADeviceContext *>(
          platform::DeviceContextPool::Instance().Get(
              platform::Place(gpu_.place_)));
      auto stream = dev_ctx->stream();
      void *src = gpu_.data_;
      void *dst = cpu_.data();
      memory::Copy(platform::CPUPlace(), dst, gpu_.place_, src, gpu_.size_,
                   stream);
      dev_ctx->Wait();
    }

    void MutableCPU() {
      if (IsInCUDA() && IsDirty()) {
        CopyToCPU();
      }
      flag_ = kDirty | kDataInCPU;
    }

    void ImmutableCUDA(platform::Place place) const {
      if (IsDirty()) {
        if (IsInCPU()) {
          CopyCPUDataToCUDA(place);
          UnsetFlag(kDirty);
          SetFlag(kDataInCUDA);
        } else if (IsInCUDA() &&
                   !(boost::get<platform::CUDAPlace>(place) == gpu_.place_)) {
          PADDLE_THROW("This situation should not happen");
          // Still dirty
        } else {
          // Dirty && DataInCUDA && Device is same
          // Do nothing
        }
      } else {
        if (!IsInCUDA()) {
          // Even data is not dirty. However, data is not in CUDA. Copy data.
          CopyCPUDataToCUDA(place);
          SetFlag(kDataInCUDA);
        } else if (!(boost::get<platform::CUDAPlace>(place) == gpu_.place_)) {
          PADDLE_THROW("This situation should not happen.");
        } else {
          // Not Dirty && DataInCUDA && Device is same
          // Do nothing.
        }
      }
    }

    void CopyCPUDataToCUDA(const platform::Place &place) const {
      void *src = cpu_.data();
      gpu_.Resize(place, cpu_.size() * sizeof(T));
      void *dst = gpu_.data_;
      auto *dev_ctx = static_cast<platform::CUDADeviceContext *>(
          platform::DeviceContextPool::Instance().Get(place));
      auto stream = dev_ctx->stream();
      memory::Copy(gpu_.place_, dst, platform::CPUPlace(), src, gpu_.size_,
                   stream);
    }

    void ImmutableCPU() const {
      if (IsDirty() && !IsInCPU()) {  // If data has been changed in CUDA, or
                                      // CPU has no data.
        CopyToCPU();
        UnsetFlag(kDirty);
      }
      SetFlag(kDataInCPU);
    }

    void UnsetFlag(int flag) const { flag_ &= ~flag; }
    void SetFlag(int flag) const { flag_ |= flag; }

    bool IsDirty() const { return flag_ & kDirty; }

    bool IsInCUDA() const { return flag_ & kDataInCUDA; }

    bool IsInCPU() const { return flag_ & kDataInCPU; }

    mutable std::vector<T> cpu_;
    mutable details::CUDABuffer gpu_;
    mutable int flag_;

    mutable std::mutex mtx_;
  };

 public:
  // Default ctor. Create empty Vector
  Vector() : m_(new VectorData()) {}

  // Fill vector with value. The vector size is `count`.
  explicit Vector(size_t count, const T &value = T())
      : m_(new VectorData(count, value)) {}

  // Ctor with init_list
  Vector(std::initializer_list<T> init) : m_(new VectorData(init)) {}

  // implicit cast from std::vector.
  template <typename U>
  Vector(const std::vector<U> &dat) : m_(new VectorData(dat)) {  // NOLINT
  }

  // Copy ctor
  Vector(const Vector<T> &other) { m_ = other.m_; }

  // Copy operator
  Vector<T> &operator=(const Vector<T> &other) {
    m_ = other.m_;
    return *this;
  }

  // Move ctor
  Vector(Vector<T> &&other) { m_ = std::move(other.m_); }

  // CPU data access method. Mutable.
  T &operator[](size_t i) { return (*m_.MutableData())[i]; }

  // CPU data access method. Immutable.
  const T &operator[](size_t i) const { return m_.Data()[i]; }

  // std::vector iterator methods. Based on CPU data access method
  size_t size() const { return m_.Data().size(); }

  iterator begin() { return m_.MutableData()->begin(); }

  iterator end() { return m_.MutableData()->end(); }

  T &front() { return m_.MutableData()->front(); }

  T &back() { return m_.MutableData()->back(); }

  const_iterator begin() const { return m_.Data().begin(); }

  const_iterator end() const { return m_.Data().end(); }

  const_iterator cbegin() const { return begin(); }

  const_iterator cend() const { return end(); }

  const T &back() const { return m_.Data().back(); }

  T *data() { return m_.MutableData()->data(); }

  const T *data() const { return m_.Data().data(); }

  const T &front() const { return m_.Data().front(); }
  // end of std::vector iterator methods

  // assign this from iterator.
  // NOTE: the iterator must support `end-begin`
  template <typename Iter>
  void assign(Iter begin, Iter end) {
    m_.MutableData()->assign(begin, end);
  }

  // push_back. If the previous capacity is not enough, the memory will
  // double.
  void push_back(T elem) { m_.MutableData()->push_back(elem); }

  // extend a vector by iterator.
  // NOTE: the iterator must support end-begin
  template <typename It>
  void Extend(It begin, It end) {
    m_.MutableData()->Extend(begin, end);
  }

  // resize the vector
  void resize(size_t size) {
    if (m_.Data().size() != size) {
      m_.MutableData()->resize(size);
    }
  }

  // get cuda ptr. immutable
  const T *CUDAData(platform::Place place) const {
    {
      auto &mtx = m_.Data().Mutex();
      std::lock_guard<std::mutex> guard(mtx);
      auto cuda_place = m_.Data().CUDAPlace();
      if (cuda_place == nullptr ||
          *cuda_place == boost::get<platform::CUDAPlace>(place)) {
        return m_.Data().CUDAData(place);
      }
    }
    // If m_ contains CUDAData in a different place. Detach manually.
    m_.Detach();
    return CUDAData(place);
  }

  // get cuda ptr. mutable
  T *CUDAMutableData(platform::Place place) {
    {
      auto &mtx = m_.Data().Mutex();
      std::lock_guard<std::mutex> guard(mtx);
      auto cuda_place = m_.Data().CUDAPlace();
      if (cuda_place == nullptr ||
          *cuda_place == boost::get<platform::CUDAPlace>(place)) {
        return m_.MutableData()->CUDAMutableData(place);
      }
    }
    // If m_ contains CUDAData in a different place. Detach manually.
    m_.Detach();
    return CUDAMutableData(place);
  }

  // clear
  void clear() { m_.MutableData()->clear(); }

  size_t capacity() const { return m_.Data().capacity(); }

  // reserve data
  void reserve(size_t size) { m_.Data().reserve(size); }

  // the unify method to access CPU or CUDA data. immutable.
  const T *Data(platform::Place place) const {
    if (platform::is_gpu_place(place)) {
      return CUDAData(place);
    } else {
      return data();
    }
  }

  // the unify method to access CPU or CUDA data. mutable.
  T *MutableData(platform::Place place) {
    if (platform::is_gpu_place(place)) {
      return CUDAMutableData(place);
    } else {
      return data();
    }
  }

  // implicit cast operator. Vector can be cast to std::vector implicitly.
  operator std::vector<T>() const { return m_.Data(); }

  bool operator==(const Vector<T> &other) const {
    if (size() != other.size()) return false;
    auto it1 = cbegin();
    auto it2 = other.cbegin();
    for (; it1 < cend(); ++it1, ++it2) {
      if (*it1 != *it2) {
        return false;
      }
    }
    return true;
  }

  const void *Handle() const { return &m_.Data(); }

 private:
  // Vector is an COW object.
  mutable details::COWPtr<VectorData> m_;
};

#else  // PADDLE_WITH_CUDA

template <typename T>
class CPUVector : public std::vector<T, std::allocator<T>> {
 public:
  CPUVector() : std::vector<T>() {}
  CPUVector(size_t count, const T &value = T())  // NOLINT
      : std::vector<T>(count, value) {}
  CPUVector(std::initializer_list<T> init) : std::vector<T>(init) {}
  CPUVector(const std::vector<T> &other) : std::vector<T>(other) {}  // NOLINT
  CPUVector(const CPUVector<T> &other) : std::vector<T>(other) {}
  CPUVector(CPUVector<T> &&other) : std::vector<T>(std::move(other)) {}
  CPUVector(std::vector<T> &&other)  // NOLINT
      : std::vector<T>(std::move(other)) {}
  CPUVector &operator=(const CPUVector &other) {
    this->assign(other.begin(), other.end());
    return *this;
  }
  CPUVector &operator=(const std::vector<T> &other) {
    this->assign(other.begin(), other.end());
    return *this;
  }

  friend std::ostream &operator<<(std::ostream &os, const CPUVector<T> &other) {
    std::stringstream ss;
    for (auto v : other) {
      os << v << " ";
    }
    return os;
  }

  T &operator[](size_t id) { return this->at(id); }

  const T &operator[](size_t id) const { return this->at(id); }

  template <typename D>
  void Extend(const D &begin, const D &end) {
    this->reserve(this->size() + size_t(end - begin));
    this->insert(this->end(), begin, end);
  }

  const T *CUDAData(platform::Place place) const {
    PADDLE_THROW(
        "Vector::CUDAData() method is not supported in CPU-only version");
  }

  T *CUDAMutableData(platform::Place place) {
    PADDLE_THROW(
        "Vector::CUDAMutableData() method is not supported in CPU-only "
        "version");
  }

  const T *Data(platform::Place place) const {
    PADDLE_ENFORCE(
        platform::is_cpu_place(place),
        "Vector::Data() method is not supported when not in CPUPlace");
    return this->data();
  }

  T *MutableData(platform::Place place) {
    PADDLE_ENFORCE(
        platform::is_cpu_place(place),
        "Vector::MutableData() method is not supported when not in CPUPlace");
    return this->data();
  }

  const void *Handle() const { return static_cast<const void *>(this); }
};

template <typename T>
using Vector = CPUVector<T>;

#endif  // PADDLE_WITH_CUDA

};  // namespace framework
}  // namespace paddle
