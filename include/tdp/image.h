/* Copyright (c) 2016, Julian Straub <jstraub@csail.mit.edu> Licensed
 * under the MIT license. See the license file LICENSE.
 */
#pragma once 
#include <assert.h>
#include <tdp/config.h>
#include <sstream>
#ifdef CUDA_FOUND
#  include <cuda.h>
#  include <cuda_runtime_api.h>
#  include <tdp/nvidia/helper_cuda.h>
#endif

namespace tdp {

template <class T>
class Image {
 public:
  Image()
    : w_(0), h_(0), pitch_(0), ptr_(nullptr)
  {}
  Image(size_t w, size_t h, T* ptr)
    : w_(w), h_(h), pitch_(w*sizeof(T)), ptr_(ptr)
  {}
  Image(size_t w, size_t h, size_t pitch, T* ptr)
    : w_(w), h_(h), pitch_(pitch), ptr_(ptr)
  {}
  Image(const Image& img)
    : w_(img.w_), h_(img.h_), pitch_(img.pitch_), ptr_(img.ptr_)
  {}
  ~Image()
  {}

  TDP_HOST_DEVICE
  const T& operator()(size_t u, size_t v) const {
    return *(RowPtr(v)+u);
  }

  TDP_HOST_DEVICE
  T& operator()(size_t u, size_t v) {
    return *(RowPtr(v)+u);
  }

  TDP_HOST_DEVICE
  T& operator[](size_t i) {
    return *(ptr_+i);
  }

  TDP_HOST_DEVICE
  const T& operator[](size_t i) const {
    return *(ptr_+i);
  }

  TDP_HOST_DEVICE
  T* RowPtr(size_t v) const { 
    return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(ptr_)+v*pitch_); }

  TDP_HOST_DEVICE
  size_t SizeBytes() const { return pitch_*h_; }

  TDP_HOST_DEVICE
  size_t Area() const { return w_*h_; }

  std::string Description() const {
    std::stringstream ss;
    ss << w_ << "x" << h_ << " pitch=" << pitch_ 
      << " " << SizeBytes() << "bytes";
    return ss.str();
  }

  void Fill(T value) { for (size_t i=0; i<w_*h_; ++i) ptr_[i] = value; }

#ifdef CUDA_FOUND
  /// Perform pitched copy from the given src image to this image.
  /// Use type to specify from which memory to which memory to copy.
  void CopyFrom(const Image<T>& src, cudaMemcpyKind type) {
    checkCudaErrors(cudaMemcpy2D(ptr_, pitch_, 
          src.ptr_, src.pitch_, w_*sizeof(T), h_, type));
  }
#endif

  size_t w_;
  size_t h_;
  size_t pitch_; // the number of bytes per row
  T* ptr_;
 private:
  
};

}
