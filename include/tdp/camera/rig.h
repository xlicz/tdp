#pragma once

#include <vector>
#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>

#include <pangolin/utils/picojson.h>
#include <pangolin/image/image_io.h>
#include <pangolin/utils/file_utils.h>
#include <pangolin/video/video_record_repeat.h>
#include <pangolin/gl/gldraw.h>

#include <tdp/camera/camera.h>
#include <tdp/cuda/cuda.h>
#include <tdp/data/allocator.h>
#include <tdp/data/image.h>
#include <tdp/data/managed_image.h>
#include <tdp/eigen/std_vector.h>
#include <tdp/gui/gui_base.hpp>
#include <tdp/manifold/SE3.h>
#include <tdp/preproc/depth.h>
#include <tdp/preproc/pc.h>
#include <tdp/preproc/normals.h>
#include <tdp/config.h>
#include <tdp/tsdf/tsdf.h>

#include <tdp/utils/stringUtils.h>

namespace tdp {

template <class Cam>
struct Rig {

  ~Rig() {
//    for (size_t i=0; i<depthScales_.size(); ++i) {
//      delete[] depthScales_[i].ptr_;
//    }
  }

  bool ParseTransformation(const pangolin::json::value& jsT, SE3f& T) {
    pangolin::json::value t_json = jsT["t_xyz"];
    Eigen::Vector3f t(
        t_json[0].get<double>(),
        t_json[1].get<double>(),
        t_json[2].get<double>());
    Eigen::Matrix3f R;
    if (jsT.contains("q_wxyz")) {
      pangolin::json::value q_json = jsT["q_wxyz"];
      Eigen::Quaternionf q(q_json[0].get<double>(),
          q_json[1].get<double>(),
          q_json[2].get<double>(),
          q_json[3].get<double>());
      R = q.toRotationMatrix();
    } else if (jsT.contains("R_3x3")) {
      pangolin::json::value R_json = jsT["R_3x3"];
      R << R_json[0].get<double>(), R_json[1].get<double>(), 
           R_json[2].get<double>(), 
           R_json[3].get<double>(), R_json[4].get<double>(), 
           R_json[5].get<double>(), 
           R_json[6].get<double>(), R_json[7].get<double>(), 
           R_json[8].get<double>();
    } else {
      return false;
    }
    T = SE3f(R, t);
    return true;
  }

  bool FromFile(std::string pathToConfig, bool verbose) {
    pangolin::json::value file_json(pangolin::json::object_type,true); 
    std::ifstream f(pathToConfig);
    if (f.is_open()) {
      std::string err = pangolin::json::parse(file_json,f);
      if (!err.empty()) {
//        std::cout << file_json.serialize(true) << std::endl;
        if (file_json.size() > 0) {
          std::cout << "found " << file_json.size() << " elements" << std::endl ;
          cuDepthScales_.reserve(file_json.size());
          for (size_t i=0; i<file_json.size(); ++i) {
            if (file_json[i].contains("camera")) {
              // serial number
              if (file_json[i]["camera"].contains("serialNumber")) {
                serials_.push_back(
                    file_json[i]["camera"]["serialNumber"].get<std::string>());
                if (verbose) 
                  std::cout << "Serial ID: " << serials_.back() 
                    << std::endl;
              }
              Cam cam;
              if (cam.FromJson(file_json[i]["camera"])) {
                if (verbose) 
                  std::cout << "found camera model" << std::endl ;
//                std::cout << file_json[i]["camera"].serialize(true) << std::endl;
              }
              cams_.push_back(cam);
              if (file_json[i]["camera"].contains("depthScale")) {
                std::string path = CONFIG_DIR+file_json[i]["camera"]["depthScale"].get<std::string>();
                depthScalePaths_.push_back(path);
                if (pangolin::FileExists(path)) {
                  pangolin::TypedImage scale8bit = pangolin::LoadImage(path);
                  size_t w = scale8bit.w/4;
                  size_t h = scale8bit.h;
                  std::cout << "depth scale size w x h: " 
                    << w << "x" << h << " " <<  path << std::endl;
                  Image<float> scaleWrap(w,h,w*sizeof(float),
                      (float*)scale8bit.ptr,Storage::Cpu);
                  cuDepthScales_.emplace_back(w,h);
                  std::cout << cuDepthScales_[cuDepthScales_.size()-1].Description() << std::endl;
                  cuDepthScales_[cuDepthScales_.size()-1].CopyFrom(scaleWrap);
                  std::cout << "found and loaded depth scale file"
                    << " " 
                    <<  cuDepthScales_[cuDepthScales_.size()-1].ptr_ << std::endl;
                }
              }
              if (file_json[i]["camera"].contains("depthScaleVsDepthModel")) {
                scaleVsDepths_.push_back(Eigen::Vector2f(
                  file_json[i]["camera"]["depthScaleVsDepthModel"][0].get<double>(),
                  file_json[i]["camera"]["depthScaleVsDepthModel"][1].get<double>()));
              }
              if (file_json[i]["camera"].contains("depthSensorUniformScale")) {
                depthSensorUniformScale_.push_back(file_json[i]["camera"]["depthSensorUniformScale"].get<double>());
              }
              if (file_json[i]["camera"].contains("T_rc")) {
                SE3f T_rc;
                if (ParseTransformation(file_json[i]["camera"]["T_rc"],T_rc)) {
                  if (verbose) 
                    std::cout << "found T_rc" << std::endl << T_rc << std::endl;
                  T_rcs_.push_back(T_rc);
                }
              }
            } else if (file_json[i].contains("imu")) {
              std::cout << "found IMU: " << file_json[i]["imu"]["type"].get<std::string>() << std::endl;
              if (file_json[i]["imu"].contains("T_ri")) {
                SE3f T_ri;
                if (ParseTransformation(file_json[i]["imu"]["T_ri"],T_ri)) {
                  if (verbose) 
                    std::cout << "found T_ri" << std::endl << T_ri << std::endl;
                  T_ris_.push_back(T_ri);
                }
              }
            }
          }
        } else {
          std::cerr << "error json file seems empty"  << std::endl
            << file_json.serialize(true) << std::endl;
          return false;
        }
      } else {
        std::cerr << "error reading json file: " << err << std::endl;
        return false;
      }
    } else {
      std::cerr << "couldnt open file: " << pathToConfig << std::endl;
      return false;
    }
    config_ = file_json;
    return true;
  }

  bool ToFile(std::string pathToConfig, bool verbose) {
    return false;
  }

  bool CorrespondOpenniStreams2Cams(
    const std::vector<pangolin::VideoInterface*>& streams,
    int maxDevs=999);

  void CollectRGB(const GuiBase& gui,
    Image<Vector3bda>& rgb);

  void CollectD(const GuiBase& gui,
    float dMin, float dMax, Image<uint16_t>& cuDraw,
    Image<float>& cuD, int64_t& t_host_us_d) ;

  void ComputePc(Image<float>& cuD, bool useRgbCamParasForDepth, 
    Image<Vector3fda>& cuPc);

  template<int LEVELS>
  void ComputePc(Image<float>& cuD, bool useRgbCamParasForDepth, 
      Pyramid<Vector3fda,LEVELS>& cuPyrPc);

  void ComputeNormals(Image<float>& cuD, bool useRgbCamParasForDepth, 
    Image<Vector3fda>& cuN);

  template<int LEVELS>
  void ComputeNormals(Image<float>& cuD, bool useRgbCamParasForDepth, 
      Pyramid<Vector3fda,LEVELS>& cuPyrN);

  void AddToTSDF(const Image<float>& cuD, const SE3f& T_mr,
    bool useRgbCamParasForDepth, 
    const Vector3fda& grid0, const Vector3fda& dGrid,
    float tsdfMu, float tsdfWMax,
    Volume<TSDFval>& cuTSDF);

  void AddToTSDF(const Image<float>& cuD, 
    const Image<Vector3bda>& cuRgb,
    const SE3f& T_mr,
    bool useRgbCamParasForDepth, 
    const Vector3fda& grid0,
    const Vector3fda& dGrid,
    float tsdfMu,
    float tsdfWMax,
    Volume<TSDFval>& cuTSDF);

  void AddToTSDF(const Image<float>& cuD, 
    const Image<Vector3bda>& cuRgb,
    const Image<Vector3fda>& cuN,
    const SE3f& T_mr,
    bool useRgbCamParasForDepth, 
    const Vector3fda& grid0,
    const Vector3fda& dGrid,
    float tsdfMu,
    float tsdfWMax,
    Volume<TSDFval>& cuTSDF);

  template<int LEVELS>
  void RayTraceTSDF(
      const Volume<TSDFval>& cuTSDF, const SE3f& T_mr,
      bool useRgbCamParasForDepth, 
      const Vector3fda& grid0, const Vector3fda& dGrid,
      float tsdfMu, float tsdfWThr,
      Pyramid<Vector3fda,LEVELS>& cuPyrPc,
      Pyramid<Vector3fda,LEVELS>& cuPyrN);

  size_t NumStreams() { return rgbdStream2cam_.size()*2; }
  size_t NumCams() { return rgbdStream2cam_.size(); }

#ifndef __CUDACC__
  void Render3D(const SE3f& T_mr, float scale=1.);
#endif

  template <typename T>
  Image<T> GetStreamRoi(const Image<T>& I, size_t streamId, float
      scale=1.) const {
    size_t camMin = *std::min_element(rgbdStream2cam_.begin(),
        rgbdStream2cam_.end());
    int w = floor(wSingle*scale);
    int h = floor(hSingle*scale);
    return I.GetRoi(0, (rgbdStream2cam_[streamId]-camMin)*h, w, h);
  }

  template <typename T>
  Image<T> GetStreamRoiOrigSize(const Image<T>& I, size_t streamId,
      float scale=1.) const {
    size_t camMin = *std::min_element(rgbdStream2cam_.begin(),
        rgbdStream2cam_.end());
    int w = floor(wOrig*scale);
    int h = floor(hOrig*scale);
    int hS = floor(hSingle*scale);
    return I.GetRoi(0, (rgbdStream2cam_[streamId]-camMin)*hS, w, h);
  }

  // imu to rig transformations
  std::vector<SE3f> T_ris_; 
  // camera to rig transformations
  std::vector<SE3f> T_rcs_; 
  // cameras
  std::vector<Cam> cams_;

  std::vector<float> depthSensorUniformScale_;
  // depth scale calibration images
  std::vector<std::string> depthScalePaths_;
//  std::vector<Image<float>> depthScales_;
  std::vector<ManagedDeviceImage<float>> cuDepthScales_;
  // depth scale scaling model as a function of depth
  eigen_vector<Eigen::Vector2f> scaleVsDepths_;

  std::vector<int32_t> rgbStream2cam_;
  std::vector<int32_t> dStream2cam_;
  std::vector<int32_t> rgbdStream2cam_;

  size_t wOrig; // original size of stream
  size_t hOrig;
  size_t wSingle; // original size + additional size to get %64 == 0
  size_t hSingle; // for convolution

  // camera serial IDs
  std::vector<std::string> serials_;
  // raw properties
  pangolin::json::value config_;
};

/// Uses serial number in openni device props and rig to find
/// correspondences.
template<class CamT>
bool Rig<CamT>::CorrespondOpenniStreams2Cams(
    const std::vector<pangolin::VideoInterface*>& streams, 
    int maxDevs) {

  rgbStream2cam_.clear();
  dStream2cam_.clear();
  rgbdStream2cam_.clear();
  
  pangolin::json::value devProps = pangolin::GetVideoDeviceProperties(streams[0]);

  std::string devType = "";
  if (devProps.contains("openni")) {
    devType = "openni"; 
  } else if(devProps.contains("realsense"))  {
    devType = "realsense"; 
  } else {
    std::cout << "cannot match based on serial number because no openni or realsense streams; have " 
      << streams.size() << " streams" << std::endl;
    for (size_t camId=0; camId<streams.size(); ++camId) {
      rgbStream2cam_.push_back(2*camId); // rgb
      dStream2cam_.push_back(2*camId+1); // ir/depth
      rgbdStream2cam_.push_back(camId); // rgbd
    }
    return false;
  }
  pangolin::json::value jsDevices = devProps[devType]["devices"];

  for (size_t i=0; i< jsDevices.size() && i < maxDevs; ++i) {
    std::string serial;
    if (jsDevices[i].contains("ONI_DEVICE_PROPERTY_SERIAL_NUMBER")) {
      serial = 
        jsDevices[i]["ONI_DEVICE_PROPERTY_SERIAL_NUMBER"].get<std::string>();
    } else if (jsDevices[i].contains("serial_number")) {
      serial = jsDevices[i]["serial_number"].get<std::string>();
    } else {
      serial = "0000";
    }

    // One of the serial numbers is 11 characters instead of 12, and
    // somehow gets padded, so we need to remove the whitespace
    serial = rtrim(serial);

    std::cout << "Device " << i << " serial #: " << serial << std::endl;
    int32_t camId = -1;
    for (size_t j=0; j<cams_.size(); ++j) {
      if (serials_[j].compare(serial) == 0) {
        camId = j;
        break;
      }
    }
    if (camId < 0) {
      std::cerr << "no matching camera found in calibration!" << std::endl;
    } else {
      std::cout << "matching camera in config: " << camId << " " 
        << config_[camId]["camera"]["description"].template get<std::string>()
        << std::endl;
    }
    rgbStream2cam_.push_back(camId); // rgb
    dStream2cam_.push_back(camId+1); // ir/depth
    rgbdStream2cam_.push_back(camId/2); // rgbd
  }
  std::cout << "Found " << NumStreams()
    << " stream paired them with " << NumCams()
    << " cams" << std::endl;
  return true;
}

template<class CamT>
void Rig<CamT>::CollectRGB(const GuiBase& gui,
    Image<Vector3bda>& rgb) {
  for (size_t sId=0; sId < rgbdStream2cam_.size(); sId++) {
    Image<Vector3bda> rgbStream;
    if (!gui.ImageRGB(rgbStream, sId)) continue;
    // TODO: this is a bit hackie; should get the w and h somehow else
    // beforehand
    wOrig = rgbStream.w_;
    hOrig = rgbStream.h_;
    wSingle = rgbStream.w_+rgbStream.w_%64;
    hSingle = rgbStream.h_+rgbStream.h_%64;
    Image<Vector3bda> rgb_i = GetStreamRoi(rgb, sId);
    rgb_i.CopyFrom(rgbStream);
  }
}

template<class CamT>
void Rig<CamT>::CollectD(const GuiBase& gui,
    float dMin, float dMax, Image<uint16_t>& cuDraw,
    Image<float>& cuD, int64_t& t_host_us_d) {
//  tdp::ManagedDeviceImage<float> cuScale(wSingle, hSingle);
  int32_t numStreams = 0;
  t_host_us_d = 0;
  for (size_t sId=0; sId < rgbdStream2cam_.size(); sId++) {
    tdp::Image<uint16_t> dStream;
    int64_t t_host_us_di = 0;
    if (!gui.ImageD(dStream, sId, &t_host_us_di)) continue;
    // TODO: this is a bit hackie; should get the w and h somehow else
    // beforehand
    t_host_us_d += t_host_us_di;
    numStreams ++;
    int32_t cId = rgbdStream2cam_[sId]; 
    wOrig = dStream.w_;
    hOrig = dStream.h_;
    wSingle = dStream.w_+dStream.w_%64;
    hSingle = dStream.h_+dStream.h_%64;
    tdp::Image<uint16_t> cuDraw_i = GetStreamRoi(cuDraw, sId);
    cudaMemset(cuDraw_i.ptr_, 0, cuDraw_i.SizeBytes());
    cuDraw_i.CopyFrom(dStream);
    // convert depth image from uint16_t to float [m]
    tdp::Image<float> cuD_i = GetStreamRoi(cuD, sId);
    if (cuDepthScales_.size() > cId) {
      float a = scaleVsDepths_[cId](0);
      float b = scaleVsDepths_[cId](1);
      tdp::ConvertDepthGpu(cuDraw_i, cuD_i, cuDepthScales_[cId], 
          a, b, dMin, dMax);
    } else if (depthSensorUniformScale_.size() > cId) {
      tdp::ConvertDepthGpu(cuDraw_i, cuD_i,
          depthSensorUniformScale_[cId], dMin, dMax);
    } else {
       std::cout << "Warning no scale information found" << std::endl;
    }
  }
  t_host_us_d /= numStreams;  

}

template<class CamT>
void Rig<CamT>::ComputeNormals(Image<float>& cuD,
    bool useRgbCamParasForDepth, 
    Image<Vector3fda>& cuN) {
  for (size_t sId=0; sId < dStream2cam_.size(); sId++) {
    int32_t cId;
    if (useRgbCamParasForDepth) {
      cId = rgbStream2cam_[sId]; 
    } else {
      cId = dStream2cam_[sId]; 
    }
    CamT cam = cams_[cId];
    tdp::SE3f T_rc = T_rcs_[cId];

    tdp::Image<tdp::Vector3fda> cuN_i = GetStreamRoi(cuN, sId);
    tdp::Image<float> cuD_i = GetStreamRoi(cuD, sId);
    // compute normals from depth in rig coordinate system
    tdp::Depth2Normals(cuD_i, cam, T_rc, cuN_i);
  }
}

template<class CamT>
template<int LEVELS>
void Rig<CamT>::ComputeNormals(Image<float>& cuD, 
    bool useRgbCamParasForDepth, 
    Pyramid<Vector3fda,LEVELS>& cuPyrN) {
  Image<Vector3fda> cuN = cuPyrN.GetImage(0);
  ComputeNormals(cuD, useRgbCamParasForDepth, cuN);
  tdp::CompleteNormalPyramid<LEVELS>(cuPyrN);
}

template<class CamT>
void Rig<CamT>::ComputePc(Image<float>& cuD, 
    bool useRgbCamParasForDepth, 
    Image<Vector3fda>& cuPc) {
  for (size_t sId=0; sId < dStream2cam_.size(); sId++) {
    int32_t cId;
    if (useRgbCamParasForDepth) {
      cId = rgbStream2cam_[sId]; 
    } else {
      cId = dStream2cam_[sId]; 
    }
    CamT cam = cams_[cId];
    tdp::SE3f T_rc = T_rcs_[cId];

    tdp::Image<tdp::Vector3fda> cuPc_i = GetStreamRoi(cuPc, sId);
    cudaMemset(cuPc_i.ptr_, 0, cuPc_i.SizeBytes());
    tdp::Image<float> cuD_i = GetStreamRoi(cuD, sId);
    // compute point cloud from depth in rig coordinate system
    tdp::Depth2PCGpu(cuD_i, cam, T_rc, cuPc_i);
  }
}

template<class CamT>
template<int LEVELS>
void Rig<CamT>::ComputePc(Image<float>& cuD, 
    bool useRgbCamParasForDepth, 
    Pyramid<Vector3fda,LEVELS>& cuPyrPc) {
  Image<Vector3fda> cuPc = cuPyrPc.GetImage(0);
  ComputePc(cuD, useRgbCamParasForDepth, cuPc);
  tdp::CompletePyramid<tdp::Vector3fda,LEVELS>(cuPyrPc);
}


template<class CamT>
void Rig<CamT>::AddToTSDF(const Image<float>& cuD, 
    const SE3f& T_mr,
    bool useRgbCamParasForDepth, 
    const Vector3fda& grid0,
    const Vector3fda& dGrid,
    float tsdfMu,
    float tsdfWMax,
    Volume<TSDFval>& cuTSDF) {
  for (size_t sId=0; sId < dStream2cam_.size(); sId++) {
    int32_t cId;
    if (useRgbCamParasForDepth) {
      cId = rgbStream2cam_[sId]; 
    } else {
      cId = dStream2cam_[sId]; 
    }
    CamT cam = cams_[cId];
    tdp::SE3f T_rc = T_rcs_[cId];
    tdp::SE3f T_mo = T_mr*T_rc;
    tdp::Image<float> cuD_i = GetStreamRoi(cuD,sId);
//    (wSingle, hSingle, cuD.ptr_+rgbdStream2cam_[sId]*wSingle*hSingle);
    TSDF::AddToTSDF<CamT::NumParams,CamT>(cuTSDF, cuD_i, T_mo, cam, grid0,
        dGrid, tsdfMu, tsdfWMax); 
  }
}

template<class CamT>
void Rig<CamT>::AddToTSDF(const Image<float>& cuD, 
    const Image<Vector3bda>& cuRgb,
    const SE3f& T_mr,
    bool useRgbCamParasForDepth, 
    const Vector3fda& grid0,
    const Vector3fda& dGrid,
    float tsdfMu,
    float tsdfWMax,
    Volume<TSDFval>& cuTSDF) {
  for (size_t sId=0; sId < dStream2cam_.size(); sId++) {
    int32_t cId;
    if (useRgbCamParasForDepth) {
      cId = rgbStream2cam_[sId]; 
    } else {
      cId = dStream2cam_[sId]; 
    }
    CamT cam = cams_[cId];
    tdp::SE3f T_rc = T_rcs_[cId];
    tdp::SE3f T_mo = T_mr*T_rc;
    tdp::Image<float> cuD_i = GetStreamRoi(cuD,sId);
    tdp::Image<Vector3bda> cuRgb_i = GetStreamRoi(cuRgb,sId);
//    (wSingle, hSingle, cuD.ptr_+rgbdStream2cam_[sId]*wSingle*hSingle);
    TSDF::AddToTSDF<CamT::NumParams,CamT>(cuTSDF, cuD_i, cuRgb_i, T_mo,
        cam, grid0, dGrid, tsdfMu, tsdfWMax); 
  }
}

template<class CamT>
void Rig<CamT>::AddToTSDF(const Image<float>& cuD, 
    const Image<Vector3bda>& cuRgb,
    const Image<Vector3fda>& cuN,
    const SE3f& T_mr,
    bool useRgbCamParasForDepth, 
    const Vector3fda& grid0,
    const Vector3fda& dGrid,
    float tsdfMu,
    float tsdfWMax,
    Volume<TSDFval>& cuTSDF) {
  for (size_t sId=0; sId < dStream2cam_.size(); sId++) {
    int32_t cId;
    if (useRgbCamParasForDepth) {
      cId = rgbStream2cam_[sId]; 
    } else {
      cId = dStream2cam_[sId]; 
    }
    CamT cam = cams_[cId];
    tdp::SE3f T_rc = T_rcs_[cId];
    tdp::SE3f T_mo = T_mr*T_rc;
    tdp::Image<float> cuD_i = GetStreamRoi(cuD,sId);
    tdp::Image<Vector3bda> cuRgb_i = GetStreamRoi(cuRgb,sId);
    tdp::Image<Vector3fda> cuN_i = GetStreamRoi(cuN,sId);
//    (wSingle, hSingle, cuD.ptr_+rgbdStream2cam_[sId]*wSingle*hSingle);
    TSDF::AddToTSDF<CamT::NumParams,CamT>(cuTSDF, cuD_i, cuRgb_i, cuN_i, T_mo,
        cam, grid0, dGrid, tsdfMu, tsdfWMax); 
  }
}

template<class CamT>
template<int LEVELS>
void Rig<CamT>::RayTraceTSDF(
    const Volume<TSDFval>& cuTSDF,
    const SE3f& T_mr,
    bool useRgbCamParasForDepth, 
    const Vector3fda& grid0,
    const Vector3fda& dGrid,
    float tsdfMu,
    float tsdfWThr,
    Pyramid<Vector3fda,LEVELS>& cuPyrPc,
    Pyramid<Vector3fda,LEVELS>& cuPyrN) {
  tdp::Image<tdp::Vector3fda> cuNEst = cuPyrN.GetImage(0);
  tdp::Image<tdp::Vector3fda> cuPcEst = cuPyrPc.GetImage(0);
  for (size_t sId=0; sId < dStream2cam_.size(); sId++) {
    int32_t cId;
    if (useRgbCamParasForDepth) {
      cId = rgbStream2cam_[sId]; 
    } else {
      cId = dStream2cam_[sId]; 
    }
    CamT cam = cams_[cId];
    tdp::SE3f T_rc = T_rcs_[cId];
    tdp::SE3f T_mo = T_mr*T_rc;

    tdp::Image<tdp::Vector3fda> cuNEst_i = GetStreamRoi(cuNEst, sId);
    tdp::Image<tdp::Vector3fda> cuPcEst_i = GetStreamRoi(cuPcEst, sId);

    // ray trace the TSDF to get pc and normals in model cosy
    tdp::TSDF::RayTraceTSDF<CamT::NumParams,CamT>(cuTSDF, cuPcEst_i, 
        cuNEst_i, T_mo, cam, grid0, dGrid, tsdfMu, tsdfWThr); 
  }
  // just complete the surface normals obtained from the TSDF
  tdp::CompletePyramid<tdp::Vector3fda,LEVELS>(cuPyrPc);
  tdp::CompleteNormalPyramid<LEVELS>(cuPyrN);
}

#ifndef __CUDACC__
template<class CamT>
void Rig<CamT>::Render3D(
    const SE3f& T_mr,
    float scale) {

  for (size_t sId=0; sId < dStream2cam_.size(); sId++) {
    int32_t cId;
//    if (useRgbCamParasForDepth) {
    cId = rgbStream2cam_[sId]; 
//    } else {
//      cId = dStream2cam_[sId]; 
//    }
    CamT cam = cams_[cId];
    tdp::SE3f T_rc = T_rcs_[cId];
    tdp::SE3f T_mo = T_mr*T_rc;
    
    pangolin::glDrawFrustrum(cam.GetKinv(), wSingle, hSingle,
        T_mo.matrix(), scale);
//    pangolin::glDrawAxis(T_mo.matrix(), scale);

  }
  pangolin::glDrawAxis(T_mr.matrix(), scale);
}
#endif

}
