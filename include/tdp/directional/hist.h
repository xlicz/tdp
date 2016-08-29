#pragma once

#include <Eigen/Dense>
#include <pangolin/gl/gl.h>
#include <pangolin/gl/glvbo.h>
#include <tdp/directional/geodesic_grid.h>
#include <tdp/image.h>

namespace tdp {

void ComputeCentroidBasedGeoidesicHist(
    Image<Eigen::Vector3f>& n,
    Image<Eigen::Vector3f>& tri_centers,
    Image<int>& hist);

template<uint32_t D>
class GeodesicHist {
 public:
  GeodesicHist()
  ~GeodesicHist() {}
  
  void Render3D(float scale);
  void Reset() { cudaMemset(cuHist_.ptr_,0,cuHist_.SizeBytes()); }
  void ComputeGpu(Image<Eigen::Vector3f>& cuN);

 private:
  GeodesicGrid<D> geoGrid_;
  ManagedDeviceImage<Eigen::Vector3f> cuTriCenters_;
  ManagedDeviceImage<int> cuHist_;

  std::vector<Eigen::Vector3f> lines;
  ManagedImage<int> hist_;
  pangolin::GlBuffer vbo_;

  void RefreshLines();
};

template<uint32_t D>
GeodesicGrid<D>::GeodesicHist() : cuHist_(geoGrid_.NTri(),1) {
  cuTriCenters_.Reinitialise(geoGrid_.NTri(),1);
  cudaMemcpy(cuTriCenters_.ptr, &(geoGrid_.tri_centers_[0]), 
      geoGrid_.NTri(), cudaMemcpyHostToDevice);
  Reset();
}

template<uint32_t D>
void GeodesicGrid<D>::ComputeGpu(Image<Eigen::Vector3f>& cuN) {
  ComputeCentroidBasedGeoidesicHist(cuN,cuTriCenters_,cuHist_);
  CopyImage(hist_, cuHist_, cudaMemcpyDeviceToHost);
}

template<uint32_t D>
void GeodesicGrid<D>::RefreshLines() {
  float sum  =0.;
  for (size_t i=0; i<hist_.w; ++i) sum += hist_[i];
  std::vector<Eigen::Vector3f>& cs = geoGrid_.tri_centers_;
  lines.clear();
  lines.reserve(cs.size()*2);
  for (size_t i=0; i<cs.size(); ++i) {
    lines.push_back(cs[i]);
    lines.push_back(cs[i]*(1+scale*float(hist_[i])/sum));
  }
}

template<uint32_t D>
void GeodesicGrid<D>::Render3D(float scale) {
  geoGrid_.Render3D(); 
  RefreshLines(); 
  vbo_.Reinitialise(GL_ARRAY_BUFFER,lines.size(),GL_FLOAT,3,GL_DYNAMIC_DRAW);
  vbo_.Upload(&(lines[0]),lines.size()*sizeof(Eigen::Vector3f));

  vbo_.Bind();
  glVertexPointer(vbo_.count_per_element, vbo_.datatype, 0, 0);
  glEnableClientState(GL_VERTEX_ARRAY);
  glPointSize(2.0);
  glDrawArrays(GL_LINES, 0, vbo_.num_elements);
  glDisableClientState(GL_VERTEX_ARRAY);
  vbo_.Unbind();
}
  
}
