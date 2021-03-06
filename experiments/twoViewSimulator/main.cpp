/* Copyright (c) 2016, Julian Straub <jstraub@csail.mit.edu> Licensed
 * under the MIT license. See the license file LICENSE.
 */
#include <iomanip>
#include <pangolin/pangolin.h>
#include <pangolin/video/video_record_repeat.h>
#include <pangolin/gl/gltexturecache.h>
#include <pangolin/gl/glpixformat.h>
#include <pangolin/handler/handler_image.h>
#include <pangolin/utils/file_utils.h>
#include <pangolin/utils/timer.h>
#include <pangolin/gl/gl.h>
#include <pangolin/gl/glsl.h>
#include <pangolin/gl/glvbo.h>
#include <pangolin/gl/gldraw.h>
#include <pangolin/image/image_io.h>

#include <tdp/eigen/dense.h>
#include <tdp/camera/camera.h>
#include <tdp/camera/photometric.h>
#include <tdp/data/managed_volume.h>
#include <tdp/data/managed_image.h>
#include <tdp/gui/quickView.h>
#include <tdp/tsdf/tsdf.h>

#include <tdp/gl/shaders.h>

#include <tdp/io/tinyply.h>
#include <tdp/marching_cubes/marching_cubes.h>

#include <tdp/preproc/depth.h>
#include <tdp/preproc/pc.h>
#include <tdp/preproc/normals.h>


int main( int argc, char* argv[] )
{
  const std::string input_uri = std::string(argv[1]);
  const std::string output_uri = (argc > 2) ? std::string(argv[2]) : "./";

  tdp::SE3f T_wG;
  tdp::Vector3fda grid0, dGrid, gridE;
  tdp::ManagedHostVolume<tdp::TSDFval> tsdf(0, 0, 0);
  if (!tdp::TSDF::LoadTSDF(input_uri, tsdf, T_wG, grid0, dGrid)) {
//  if (!tdp::LoadVolume<tdp::TSDFval>(tsdf, input_uri)) {
    pango_print_error("Unable to load volume");
    return 1;
  }
  tdp::Vector3fda numGrid(tsdf.w_, tsdf.h_, tsdf.d_);
  gridE = grid0+(dGrid.array()*numGrid.array()).matrix();
  std::cout << "loaded TSDF volume of size: " << tsdf.w_ << "x" 
    << tsdf.h_ << "x" << tsdf.d_ << std::endl;
  tdp::ManagedDeviceVolume<tdp::TSDFval> cuTsdf(tsdf.w_, tsdf.h_, tsdf.d_);
  cuTsdf.CopyFrom(tsdf, cudaMemcpyHostToDevice);

  size_t w = 640;
  size_t h = 480;
  float tsdfMu = 0.05;
  float tsdfWThr = 10;
  // camera model for computing point cloud and normals
  tdp::Camera<float> cam(Eigen::Vector4f(550,550,319.5,239.5)); 

  int menue_w = 180;
  pangolin::CreateWindowAndBind( "GuiBase", 1200+menue_w, 800);
  // current frame in memory buffer and displaying.
  pangolin::CreatePanel("ui").SetBounds(0.,1.,0.,pangolin::Attach::Pix(menue_w));
  // Assume packed OpenGL data unless otherwise specified
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glEnable (GL_BLEND);
  glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  pangolin::View& container = pangolin::Display("container");
  container.SetLayout(pangolin::LayoutEqual)
    .SetBounds(0., 1.0, pangolin::Attach::Pix(menue_w), 1.0);

  // Define Camera Render Object (for view / scene browsing)
  pangolin::OpenGlRenderState s_cam(
      pangolin::ProjectionMatrix(640,480,420,420,320,240,0.1,1000),
      pangolin::ModelViewLookAt(0,0.5,-3, 0,0,0, pangolin::AxisNegY)
      );
  // Add named OpenGL viewport to window and provide 3D Handler
  pangolin::View& d_cam = pangolin::CreateDisplay()
    .SetHandler(new pangolin::Handler3D(s_cam));
  container.AddDisplay(d_cam);

  pangolin::Var<float> dMin("ui.d min",0.10,0.0,0.1);
  pangolin::Var<float> dMax("ui.d max",6.,0.1,10.);
  pangolin::Var<float> wThr("ui.weight thr",1,1,100);
  pangolin::Var<float> fThr("ui.tsdf value thr",1.,0.01,0.5);
  pangolin::Var<bool> recomputeMesh("ui.recompute mesh", true, false);
  pangolin::Var<bool> nextFrame("ui.next frame", true, false);
  pangolin::Var<bool> continuousMode("ui.continuous", true, true);

  pangolin::GlBuffer meshVbo;
  pangolin::GlBuffer meshCbo;
  pangolin::GlBuffer meshIbo;

  pangolin::GlBuffer vboA(pangolin::GlArrayBuffer,w*h,GL_FLOAT,3);
  pangolin::GlBuffer vboB(pangolin::GlArrayBuffer,w*h,GL_FLOAT,3);

  // host image: image in CPU memory
  tdp::ManagedHostImage<tdp::Vector3fda> pcA(w, h);
  tdp::ManagedHostImage<tdp::Vector3fda> pcB(w, h);
  tdp::ManagedHostImage<tdp::Vector3fda> nA(w, h);
  tdp::ManagedHostImage<tdp::Vector3fda> nB(w, h);

  // device image: image in GPU memory
  tdp::ManagedDeviceImage<uint16_t> cuDraw(w, h);
  tdp::ManagedDeviceImage<float> cuD(w, h);
  tdp::ManagedDeviceImage<tdp::Vector3fda> cuN(w, h);
  tdp::ManagedDeviceImage<tdp::Vector3fda> cuPc(w, h);

//  tdp::SE3f T_wm;
//  T_wm.translation() = grid0 + 256*dGrid;

  tdp::SE3f T_gcA;
  tdp::SE3f T_gcB;
  tdp::SE3f T_ab;

  float overlap = 0.;
  float fillA = 0.;
  float fillB = 0.;
  size_t frame = 0;
  // Stream and display video
  while(!pangolin::ShouldQuit())
  {
    if (meshVbo.num_elements == 0 || pangolin::Pushed(recomputeMesh)) {
      tdp::ComputeMesh(tsdf, grid0, dGrid,
          T_wG, meshVbo, meshCbo, meshIbo, wThr, fThr);      
    }

    if (pangolin::Pushed(nextFrame) || continuousMode) {
      overlap = 0.;
      fillA = 0.;
      fillB = 0.;
      int it = 0;
      do {
        Eigen::Vector3f mean_t(0,0,0);
        T_gcA = tdp::SE3f::Random(M_PI, mean_t, 0.3);
        T_gcB = T_gcA* tdp::SE3f::Random(45./180.*M_PI, mean_t, 0.3);
        T_ab = T_gcA.Inverse() * T_gcB;

        tdp::TSDF::RayTraceTSDF(cuTsdf, cuPc,
            cuN, T_gcA, cam, grid0, dGrid, tsdfMu, tsdfWThr); 
        tdp::TransformPc(T_gcA.Inverse(), cuPc);
        tdp::TransformPc(T_gcA.rotation().Inverse(), cuN);
        pcA.CopyFrom(cuPc, cudaMemcpyDeviceToHost);
        nA.CopyFrom(cuN, cudaMemcpyDeviceToHost);
        tdp::TSDF::RayTraceTSDF(cuTsdf, cuPc,
            cuN, T_gcB, cam, grid0, dGrid, tsdfMu, tsdfWThr); 
        tdp::TransformPc(T_gcB.Inverse(), cuPc);
        tdp::TransformPc(T_gcB.rotation().Inverse(), cuN);
        pcB.CopyFrom(cuPc, cudaMemcpyDeviceToHost);
        nB.CopyFrom(cuN, cudaMemcpyDeviceToHost);

        for (size_t i=0; i<pcA.Area(); ++i) 
          if (tdp::IsValidData(pcA[i])) {
            if (dMin < pcA[i](2) && pcA[i](2) < dMax) {
              fillA++;
            } else  {
              pcA[i] << NAN, NAN, NAN;
            }
          }
        for (size_t i=0; i<pcB.Area(); ++i) 
          if (tdp::IsValidData(pcB[i])) {
            if (dMin < pcB[i](2) && pcB[i](2) < dMax) {
              fillB++;
            } else {
              pcB[i] << NAN, NAN, NAN;
            }
          }
        fillA /= pcA.Area();
        fillB /= pcB.Area();

        float overlapAB, overlapBA;
        tdp::Overlap(pcA, pcB, T_ab, cam, overlapAB);
        tdp::Overlap(pcB, pcA, T_ab.Inverse(), cam, overlapBA);
        overlap = std::min(overlapAB, overlapBA);

        std::cout << overlap << " " << fillA << " " << fillB << std::endl;
      } while (it++ < 0 && (overlap < 0.5 || fillA < 0.7 || fillB < 0.7));

      vboA.Upload(pcA.ptr_, pcA.SizeBytes(), 0);
      vboB.Upload(pcB.ptr_, pcB.SizeBytes(), 0);

      if (overlap >= 0.5 && fillA >= 0.7 && fillB >= 0.7) {
        std::stringstream pathA;
        std::stringstream pathB;
        pathA << output_uri << "frame_" << std::setfill('0') << std::setw(10) 
          << frame << "_A.ply";
        pathB << output_uri << "frame_" << std::setfill('0') << std::setw(10) 
          << frame << "_B.ply";
        tdp::SavePointCloud(pathA.str(), pcA, nA, false);
        tdp::SavePointCloud(pathB.str(), pcB, nB, false);

        std::stringstream pathConfig;
        pathConfig << output_uri << "/config_" << std::setfill('0') 
          << std::setw(10) << frame << ".txt";
        std::ofstream fout(pathConfig.str());
        fout << pathA.str() << std::endl;
        fout << pathB.str() << std::endl;
        fout << "q_abx q_aby q_abz q_abw t_abx t_aby t_abz overlap fillA fillB" << std::endl;
        fout << T_ab.rotation().vector()(0) << " " 
          << T_ab.rotation().vector()(1) << " " 
          << T_ab.rotation().vector()(2) << " " 
          << T_ab.rotation().vector()(3) << " " 
          << T_ab.translation()(0) << " "
          << T_ab.translation()(1) << " "
          << T_ab.translation()(2) << " " 
          << overlap << " " << fillA << " " << fillB << std::endl;
        fout.close();

        std::cout << pathA.str() << std::endl << pathB.str() << std::endl;
        ++frame;
      }
    }

    // clear the OpenGL render buffers
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    glColor3f(1.0f, 1.0f, 1.0f);

    // Draw 3D stuff
    glEnable(GL_DEPTH_TEST);
    if (d_cam.IsShown()) {
      d_cam.Activate(s_cam);

      if (meshVbo.num_elements > 0
          && meshCbo.num_elements > 0
          && meshIbo.num_elements > 0) {
        meshVbo.Bind();
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0); 
        meshCbo.Bind();
        glVertexAttribPointer(1, 3, GL_UNSIGNED_BYTE, GL_TRUE, 0, 0); 
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);

        auto& shader = tdp::Shaders::Instance()->normalMeshShader_;   
        shader.Bind();
        pangolin::OpenGlMatrix P = s_cam.GetProjectionMatrix();
        pangolin::OpenGlMatrix MV = s_cam.GetModelViewMatrix();
        shader.SetUniform("P",P);
        shader.SetUniform("MV",MV);

        meshIbo.Bind();
        glDrawElements(GL_TRIANGLES, meshIbo.num_elements*3,
            meshIbo.datatype, 0);
        meshIbo.Unbind();

        shader.Unbind();
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(0);
        meshCbo.Unbind();
        meshVbo.Unbind();
      }

      // draw the axis
      pangolin::glDrawAxis((T_wG*T_gcA).matrix(),0.1f);
      pangolin::glDrawAxis((T_wG*T_gcB).matrix(),0.1f);
      pangolin::glDrawFrustrum(cam.GetKinv(), w, h, (T_wG*T_gcA).matrix(), 0.1f);
      pangolin::glDrawFrustrum(cam.GetKinv(), w, h, (T_wG*T_gcB).matrix(), 0.1f);

      glColor3f(1,0,0);
      pangolin::glSetFrameOfReference((T_wG*T_gcA).matrix());
      pangolin::RenderVbo(vboA);
      pangolin::glUnsetFrameOfReference();
      glColor3f(0,1,0);
      pangolin::glSetFrameOfReference((T_wG*T_gcB).matrix()); 
      pangolin::RenderVbo(vboB);
      pangolin::glUnsetFrameOfReference();

      pangolin::glSetFrameOfReference(T_wG.matrix());
      Eigen::AlignedBox3f box(grid0,gridE);
      glColor4f(1,0,0,0.5f);
      pangolin::glDrawAlignedBox(box);
      pangolin::glUnsetFrameOfReference();
    }

    glDisable(GL_DEPTH_TEST);
    // Draw 2D stuff

    // leave in pixel orthographic for slider to render.
    pangolin::DisplayBase().ActivatePixelOrthographic();
    // finish this frame
    pangolin::FinishFrame();
  }
  return 0;
}


