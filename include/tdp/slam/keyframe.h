#pragma once
#include <tdp/data/image.h>
#include <tdp/data/managed_image.h>
#include <tdp/data/managed_pyramid.h>
#include <tdp/eigen/dense.h>
#include <tdp/manifold/SE3.h>
#include <tdp/camera/camera_base.h>
#include <tdp/camera/rig.h>
#include <tdp/camera/photometric.h>
namespace tdp {

/// KeyFrame
///
/// Use emplace_back in std::vector to not run into memory issues.
/// Make sure that ManagedImage and ManagedPyramdid (and all additional
/// containers) have a propper move constructor.
struct KeyFrame {
  KeyFrame() {};
  KeyFrame(
      const Image<Vector3fda>& pc, 
      const Image<Vector3fda>& n,
      const Image<Vector3bda>& rgb,
      const SE3f& T_wk) :
    pc_(pc.w_, pc.h_), n_(n.w_, n.h_), rgb_(rgb.w_, rgb.h_), 
    T_wk_(T_wk)
  {
    pc_.CopyFrom(pc);
    n_.CopyFrom(n);
    rgb_.CopyFrom(rgb);
  }

  KeyFrame(
      const Pyramid<Vector3fda,3>& pc, 
      const Pyramid<Vector3fda,3>& n,
      const Pyramid<float,3>& grey,
      const Pyramid<Vector2fda,3>& gradGrey,
      const Image<Vector3bda>& rgb,
      const Image<float>& d,
      const SE3f& T_wk) :
    pc_(pc.w_, pc.h_), n_(n.w_, n.h_), rgb_(rgb.w_, rgb.h_), 
    d_(d.w_, d.h_), pyrPc_(pc.w_, pc.h_), pyrN_(n.w_, n.h_),
    pyrGrey_(grey.w_, grey.h_), pyrGradGrey_(gradGrey.w_, gradGrey.h_),
    T_wk_(T_wk)
  {
    pc_.CopyFrom(pc.GetConstImage(0));
    n_.CopyFrom(n.GetConstImage(0));
    d_.CopyFrom(d);
    rgb_.CopyFrom(rgb);

    pyrPc_.CopyFrom(pc);
    pyrN_.CopyFrom(n);
    pyrGrey_.CopyFrom(grey);
    pyrGradGrey_.CopyFrom(gradGrey);
  }

  ManagedHostImage<Vector3fda> pc_;
  ManagedHostImage<Vector3fda> n_;
  ManagedHostImage<Vector3bda> rgb_;
  ManagedHostImage<float> d_;

  ManagedHostPyramid<Vector3fda,3> pyrPc_;
  ManagedHostPyramid<Vector3fda,3> pyrN_;
  ManagedHostPyramid<float,3> pyrGrey_;
  ManagedHostPyramid<Vector2fda,3> pyrGradGrey_;

  SE3f T_wk_; // Transformation from keyframe to world

};

/// Compute overlap fraction between two KFs in a given pyramid level
/// lvl
template <int D, class Derived>
void Overlap(const KeyFrame& kfA, const KeyFrame& kfB,
    const CameraBase<float, D,Derived>& cam, int lvl, float& overlap,
    float& rmse, const SE3f* T_ab = nullptr, Image<float>* errB=nullptr) {
  tdp::SE3f T_ab_ = kfA.T_wk_.Inverse() * kfB.T_wk_;
  if (T_ab)
    T_ab_ = *T_ab;

  const Image<float> greyA = kfA.pyrGrey_.GetConstImage(lvl);
  const Image<float> greyB = kfB.pyrGrey_.GetConstImage(lvl);
  const Image<Vector3fda> pcA = kfA.pyrPc_.GetConstImage(lvl);
  const Image<Vector3fda> pcB = kfB.pyrPc_.GetConstImage(lvl);
  Overlap(greyA, greyB, pcA, pcB, T_ab_, 
      ScaleCamera<float,D,Derived>(cam,pow(0.5,lvl)), 
      overlap, rmse, errB);
}

template <typename CamT>
void Overlap(const KeyFrame& kfA, const KeyFrame& kfB,
    const Rig<CamT>& rig, int lvl, float& overlap,
    float& rmse, const SE3f* T_ab = nullptr, Image<float>* errB=nullptr) {


  tdp::SE3f T_ab_ = kfA.T_wk_.Inverse() * kfB.T_wk_;
  if (T_ab)
    T_ab_ = *T_ab;

  overlap = 0.f;
  rmse = 0.f;

  size_t w = kfA.pyrGrey_.w_;
  size_t h = kfA.pyrGrey_.h_;
  ManagedDeviceImage<float> greyA(w,h);
  greyA.CopyFrom(kfA.pyrGrey_.GetConstImage(0));
  ManagedDeviceImage<float> greyB(w,h);
  greyB.CopyFrom(kfB.pyrGrey_.GetConstImage(0));
  ManagedDeviceImage<Vector3fda> pcA(w,h);
  pcA.CopyFrom(kfA.pyrPc_.GetConstImage(0));
  ManagedDeviceImage<Vector3fda> pcB(w,h);
  pcB.CopyFrom(kfB.pyrPc_.GetConstImage(0));

  OverlapGpu(greyA, greyB, pcA, pcB, T_ab_, rig, overlap, rmse, errB); 
}



}
