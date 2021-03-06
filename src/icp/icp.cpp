
#include <tdp/icp/icp.h>
#include <tdp/data/pyramid.h>
#include <tdp/camera/camera.h>
#include <tdp/camera/camera_poly.h>
#include <tdp/manifold/SE3.h>
#include <tdp/utils/Stopwatch.h>

#ifdef ANN_FOUND
# include <tdp/nn/ann.h>
#endif

namespace tdp {

#ifdef ANN_FOUND
void ICP::ComputeANN(
    Image<Vector3fda>& pc_m,
    Image<Vector3fda>& cuPc_m,
    Image<Vector3fda>& n_m,
    Image<Vector3fda>& pc_o,
    Image<Vector3fda>& cuPc_o,
    Image<Vector3fda>& n_o,
    Image<int>& assoc_om,
    Image<int>& cuAssoc_om,
    SE3f& T_mo,
    size_t maxIt, float angleThr_deg, float distThr,
    int downSampleANN, bool verbose,
    float& err, float& count
    ) {
  float errPrev = 0.f; 
  int countThr = 0;
  size_t it;

  TICK("ICP ANN");
  for (it=0; it<maxIt; ++it) {
    TICK("ANN");
    int Nassoc = tdp::AssociateANN(pc_m, pc_o, T_mo.Inverse(),
        assoc_om, downSampleANN);
    TOCK("ANN");
    countThr = Nassoc / 50; 

    TICK("ICP given assoc");
    cuAssoc_om.CopyFrom(assoc_om, cudaMemcpyHostToDevice);
    tdp::ICP::ComputeGivenAssociation(cuPc_m, n_m, cuPc_o, n_o, 
        cuAssoc_om, T_mo, 1, angleThr_deg, distThr, countThr, verbose,
        err, count);
    TOCK("ICP given assoc");
    if (verbose) {
      std::cout << " it " << it 
        << ": err=" << err << "\tdErr/err=" << fabs(err-errPrev)/err
        << " # inliers: " << count 
        << " det(R): " << T_mo.rotation().matrix().determinant()
        << std::endl;
    }
    //std::cout << dT.matrix() << std::endl;
    //std::cout << T_mo.matrix() << std::endl;
    if (it>0 && fabs(err-errPrev)/err < 1e-5) break;
    errPrev = err;
  }
  TOCK("ICP ANN");
  std::cout << "it=" << it
    << ": err=" << err << "\tdErr/err=" << fabs(err-errPrev)/err
    << " # inliers: " << count  << " thr: " << countThr
    << " det(R): " << T_mo.rotation().matrix().determinant()
    << std::endl;
}
#endif

void ICP::ComputeGivenAssociation(
    Image<Vector3fda>& pc_m,
    Image<Vector3fda>& n_m,
    Image<Vector3fda>& pc_o,
    Image<Vector3fda>& n_o,
    Image<int>& assoc_om,
    SE3f& T_mo,
    size_t maxIt, float angleThr_deg, float distThr,
    int countThr,
    bool verbose,
    float& error, float& count
    ) {
  Eigen::Matrix<float,6,6,Eigen::DontAlign> ATA;
  Eigen::Matrix<float,6,1,Eigen::DontAlign> ATb;
  float errPrev = error; 
  count = 0.f; 
  error = 0.f; 
  for (size_t it=0; it<maxIt; ++it) {
    // Compute ATA and ATb from A x = b
#ifdef CUDA_FOUND
    ICPStep(pc_m, n_m, pc_o, n_o, assoc_om,
        T_mo, cos(angleThr_deg*M_PI/180.),
        distThr,ATA,ATb,error,count);
#endif
    if (count < countThr) {
//      std::cout << "# inliers " << count << " to small; skipping" << std::endl;
      break;
    }
    error /= count;
    ATA /= count;
    ATb /= count;

    // solve for x using ldlt
    Eigen::Matrix<float,6,1,Eigen::DontAlign> x =
      (ATA.cast<double>().ldlt().solve(ATb.cast<double>())).cast<float>(); 

    // apply x to the transformation
    SE3f dT = SE3f::Exp_(x);
    T_mo = dT * T_mo;
    if (verbose) {
      std::cout << " it " << it 
        << ": err=" << error << "\tdErr/err=" << fabs(error-errPrev)/error
        << " # inliers: " << count 
//        << " rank(ATA): " << rank
        << " det(R): " << T_mo.rotation().matrix().determinant()
        << " |x|: " << x.topRows(3).norm()*180./M_PI 
        << " " <<  x.bottomRows(3).norm()
        << std::endl;
    }
    //std::cout << dT.matrix() << std::endl;
    //std::cout << T_mo.matrix() << std::endl;
    if (it>0 && fabs(error-errPrev)/error < 1e-7) break;
    errPrev = error;
  }
}

template<int D, typename Derived>
void ICP::ComputeProjective(
    Pyramid<Vector3fda,3>& pcs_m,
    Pyramid<Vector3fda,3>& ns_m,
    Pyramid<Vector3fda,3>& pcs_o,
    Pyramid<Vector3fda,3>& ns_o,
    SE3f& T_mo,
    const SE3f& T_cm,
    const CameraBase<float,D,Derived>& cam,
    const std::vector<size_t>& maxIt, float angleThr_deg, float distThr,
    bool verbose
    ) {
  Eigen::Matrix<float,6,6,Eigen::DontAlign> ATA;
  Eigen::Matrix<float,6,1,Eigen::DontAlign> ATb;
  size_t lvls = maxIt.size();
  float count = 0.f; 
  for (int lvl=lvls-1; lvl >= 0; --lvl) {
    float errPrev = 0.f; 
    float error = 0.f; 
    for (size_t it=0; it<maxIt[lvl]; ++it) {
      // Compute ATA and ATb from A x = b
#ifdef CUDA_FOUND
      Image<Vector3fda> pc_m = pcs_m.GetImage(lvl);
      ICPStep<D,Derived>(pc_m, ns_m.GetImage(lvl), 
          pcs_o.GetImage(lvl), ns_o.GetImage(lvl),
          T_mo, T_cm, ScaleCamera<float>(cam,pow(0.5,lvl)),
          cos(angleThr_deg*M_PI/180.),
          distThr,ATA,ATb,error,count);
#endif
      if (count < 10) {
        std::cout << "# inliers " << count 
          << " in pyramid level " << lvl
          << " to small; skipping" << std::endl;
        break;
      }
      // solve for x using ldlt
      Eigen::Matrix<float,6,1,Eigen::DontAlign> x =
        (ATA.cast<double>().ldlt().solve(ATb.cast<double>())).cast<float>(); 

      Eigen::SelfAdjointEigenSolver<Eigen::Matrix<float,6,6>> eig(ATA);
      Eigen::Matrix<float,6,1> ev = eig.eigenvalues().array().square();
      // condition number
      float kappa = ev.maxCoeff() / ev.minCoeff();

      // apply x to the transformation
//      std::cout << T_mo << std::endl;
      T_mo = T_mo * SE3f::Exp_(x);
//      T_mo = SE3f(SE3f::Exp_(x).matrix() * T_mo.matrix());
//      std::cout << SE3f::Exp_(x) << std::endl;
//      std::cout << T_mo << std::endl;
      if (verbose) {
        std::cout << "lvl " << lvl << " it " << it 
          << ": err=" << error << "\tdErr/err=" << fabs(error-errPrev)/error
          << "\t# inliers: " << count 
          << "\tkappa(ATA): " << kappa
//        << "\tev(ATA): " << ev.transpose()
//          << "\tdet(R): " << T_mo.rotation().matrix().determinant()
          << "\t|x|: " << x.topRows(3).norm()*180./M_PI 
          << " " <<  x.bottomRows(3).norm()
          << std::endl;
      }
      //std::cout << dT.matrix() << std::endl;
      //std::cout << T_mo.matrix() << std::endl;
      if (it>0 && fabs(error-errPrev)/error < 1e-7) break;
      errPrev = error;
    }
  }
}

// explicit instantiation
template void ICP::ComputeProjective(
    Pyramid<Vector3fda,3>& pcs_m,
    Pyramid<Vector3fda,3>& ns_m,
    Pyramid<Vector3fda,3>& pcs_o,
    Pyramid<Vector3fda,3>& ns_o,
    SE3f& T_mo,
    const SE3f& T_cm,
    const BaseCameraf& cam,
    const std::vector<size_t>& maxIt, float angleThr_deg, float distThr,
    bool verbose
    );
template void ICP::ComputeProjective(
    Pyramid<Vector3fda,3>& pcs_m,
    Pyramid<Vector3fda,3>& ns_m,
    Pyramid<Vector3fda,3>& pcs_o,
    Pyramid<Vector3fda,3>& ns_o,
    SE3f& T_mo,
    const SE3f& T_cm,
    const BaseCameraPoly3f& cam,
    const std::vector<size_t>& maxIt, float angleThr_deg, float distThr,
    bool verbose
    );


template<typename CameraT>
void ICP::ComputeProjective(
    Pyramid<Vector3fda,3>& pcs_m,
    Pyramid<Vector3fda,3>& ns_m,
    Pyramid<Vector3fda,3>& pcs_o,
    Pyramid<Vector3fda,3>& ns_o,
    const Rig<CameraT>& rig,
    const std::vector<int32_t>& stream2cam,
    const std::vector<size_t>& maxIt, 
    float angleThr_deg, float distThr,
    bool verbose,
    SE3f& T_mr,
    Eigen::Matrix<float,6,6>& Sigma_mr,
    std::vector<float>& errPerLvl,
    std::vector<float>& countPerLvl
    ) {

  size_t lvls = maxIt.size();
  errPerLvl   = std::vector<float>(lvls, 0);
  countPerLvl = std::vector<float>(lvls, 0);
  for (int lvl=lvls-1; lvl >= 0; --lvl) {
    float errPrev = 0.f; 
    float error = 0.f; 
    float count = 0.f; 
    for (size_t it=0; it<maxIt[lvl]; ++it) {
      count = 0.f; 
      error = 0.f; 
      Eigen::Matrix<float,6,6,Eigen::DontAlign> ATA;
      Eigen::Matrix<float,6,1,Eigen::DontAlign> ATb;
      ATA.fill(0.);
      ATb.fill(0.);
      tdp::Image<tdp::Vector3fda> pc_ml = pcs_m.GetImage(lvl);
      tdp::Image<tdp::Vector3fda> n_ml = ns_m.GetImage(lvl);
      tdp::Image<tdp::Vector3fda> pc_ol = pcs_o.GetImage(lvl);
      tdp::Image<tdp::Vector3fda> n_ol = ns_o.GetImage(lvl);
      float scale = pow(0.5,lvl);
      for (size_t sId=0; sId < stream2cam.size(); sId++) {
        int32_t cId = stream2cam[sId]; 
        CameraT cam = rig.cams_[cId].Scale(scale);
        tdp::SE3f T_cr = rig.T_rcs_[cId].Inverse();

        // all PC and normals are in rig coordinates
        tdp::Image<tdp::Vector3fda> pc_mli = rig.GetStreamRoi(pc_ml, sId, scale);
        tdp::Image<tdp::Vector3fda> pc_oli = rig.GetStreamRoi(pc_ol, sId, scale);
        tdp::Image<tdp::Vector3fda> n_mli =  rig.GetStreamRoi(n_ml, sId, scale);
        tdp::Image<tdp::Vector3fda> n_oli =  rig.GetStreamRoi(n_ol, sId, scale);

//        std::cout 
//          << pc_mli.Description() << std::endl
//          << pc_oli.Description() << std::endl
//          << n_mli.Description() << std::endl
//          << n_oli.Description() << std::endl
//          << T_mr << std::endl
//          << T_cr << std::endl;

        Eigen::Matrix<float,6,6,Eigen::DontAlign> ATA_i;
        Eigen::Matrix<float,6,1,Eigen::DontAlign> ATb_i;
        float error_i = 0;
        float count_i = 0;
        // Compute ATA and ATb from A x = b
        ICPStep(pc_mli, n_mli, pc_oli, n_oli,
            T_mr, T_cr, cam,
            cos(angleThr_deg*M_PI/180.),
            distThr,ATA_i,ATb_i,error_i,count_i);
        ATA += ATA_i;
        ATb += ATb_i;
        error += error_i;
        count += count_i;
      }
      if (count < 10) {
        std::cout << "# inliers " << count << " to small " << std::endl;
        break;
      }
//      ATA /= count;
//      ATb /= count;
      // solve for x using ldlt
      Eigen::Matrix<float,6,1,Eigen::DontAlign> x =
        (ATA.cast<double>().ldlt().solve(ATb.cast<double>())).cast<float>(); 

      Sigma_mr = ATA.inverse();
      Eigen::SelfAdjointEigenSolver<Eigen::Matrix<float,6,6>> eig(Sigma_mr);
      Eigen::Matrix<float,6,1> ev = eig.eigenvalues().array();
      // condition number
      float kappa = ev.maxCoeff() / ev.minCoeff();
      float kappaR = ev.head<3>().maxCoeff() / ev.head<3>().minCoeff();
      float kappat = ev.tail<3>().maxCoeff() / ev.tail<3>().minCoeff();

      // apply x to the transformation
      T_mr = T_mr * SE3f::Exp_(x);
      if (verbose) {
      std::cout << std::setprecision(2) 
        << std::scientific << "lvl " << lvl << " it " << it 
        << ": err=" << error 
        << "\tdErr/err=" << fabs(error-errPrev)/error
        << "\t# inliers: " << count 
        << "\t# det(S): " << ev.array().prod()
        << "\t# kappa: " << kappa << ", " << kappaR << ", " << kappat
        << "\t|x|=" << x.head<3>().norm() << ", " << x.tail<3>().norm()
//        << "\t# evs: " << ev.transpose()
        //<< " |ATA|=" << ATA.determinant()
        //<< " x=" << x.transpose()
        << std::endl;
      }
      if (it>0 && fabs(error-errPrev)/error < 1e-7) break;
      errPrev = error;
    }
    errPerLvl[lvl] = log(error);
    countPerLvl[lvl] = count;
  }
}

template void ICP::ComputeProjective(
    Pyramid<Vector3fda,3>& pcs_m,
    Pyramid<Vector3fda,3>& ns_m,
    Pyramid<Vector3fda,3>& pcs_o,
    Pyramid<Vector3fda,3>& ns_o,
    const Rig<Cameraf>& rig,
    const std::vector<int32_t>& stream2cam,
    const std::vector<size_t>& maxIt, 
    float angleThr_deg, float distThr,
    bool verbose,
    SE3f& T_mr,
    Eigen::Matrix<float,6,6>& Sigma_mr,
    std::vector<float>& errPerLvl,
    std::vector<float>& countPerLvl
    );

template void ICP::ComputeProjective(
    Pyramid<Vector3fda,3>& pcs_m,
    Pyramid<Vector3fda,3>& ns_m,
    Pyramid<Vector3fda,3>& pcs_o,
    Pyramid<Vector3fda,3>& ns_o,
    const Rig<CameraPoly3f>& rig,
    const std::vector<int32_t>& stream2cam,
    const std::vector<size_t>& maxIt, 
    float angleThr_deg, float distThr,
    bool verbose,
    SE3f& T_mr,
    Eigen::Matrix<float,6,6>& Sigma_mr,
    std::vector<float>& errPerLvl,
    std::vector<float>& countPerLvl
    );

template<typename CameraT>
void ICP::ComputeProjectiveUpdateIndividual(
    Pyramid<Vector3fda,3>& pcs_m,
    Pyramid<Vector3fda,3>& ns_m,
    Pyramid<Vector3fda,3>& pcs_o,
    Pyramid<Vector3fda,3>& ns_o,
    Rig<CameraT>& rig,
    const std::vector<int32_t>& stream2cam,
    const std::vector<size_t>& maxIt, 
    float angleThr_deg, float distThr,
    bool verbose,
    SE3f& T_mr,
    std::vector<float>& errPerLvl,
    std::vector<float>& countPerLvl
    ) {
  std::vector<tdp::SE3f> dT_mr(stream2cam.size(), tdp::SE3f());

  size_t lvls = maxIt.size();
  errPerLvl   = std::vector<float>(lvls, 0);
  countPerLvl = std::vector<float>(lvls, 0);
  for (int lvl=lvls-1; lvl >= 0; --lvl) {
    float errPrev = 0.f; 
    float error = 0.f; 
    float count = 0.f; 
    for (size_t it=0; it<maxIt[lvl]; ++it) {
      count = 0.f; 
      error = 0.f; 
      tdp::Image<tdp::Vector3fda> pc_ml = pcs_m.GetImage(lvl);
      tdp::Image<tdp::Vector3fda> n_ml = ns_m.GetImage(lvl);
      tdp::Image<tdp::Vector3fda> pc_ol = pcs_o.GetImage(lvl);
      tdp::Image<tdp::Vector3fda> n_ol = ns_o.GetImage(lvl);
      float scale = pow(0.5,lvl);
      for (size_t sId=0; sId < stream2cam.size(); sId++) {
        int32_t cId = stream2cam[sId]; 
        CameraT cam = rig.cams_[cId].Scale(scale);
        tdp::SE3f T_cr = rig.T_rcs_[cId].Inverse();

        // all PC and normals are in rig coordinates
        tdp::Image<tdp::Vector3fda> pc_mli = rig.GetStreamRoi(pc_ml, sId, scale);
        tdp::Image<tdp::Vector3fda> pc_oli = rig.GetStreamRoi(pc_ol, sId, scale);
        tdp::Image<tdp::Vector3fda> n_mli =  rig.GetStreamRoi(n_ml, sId, scale);
        tdp::Image<tdp::Vector3fda> n_oli =  rig.GetStreamRoi(n_ol, sId, scale);

        Eigen::Matrix<float,6,6,Eigen::DontAlign> ATA_i;
        Eigen::Matrix<float,6,1,Eigen::DontAlign> ATb_i;
        float error_i = 0;
        float count_i = 0;
        tdp::SE3f T_mr_new = T_mr*dT_mr[sId];
        // Compute ATA and ATb from A x = b
        ICPStep(pc_mli, n_mli, pc_oli, n_oli,
            T_mr_new, T_cr, cam, cos(angleThr_deg*M_PI/180.),
            distThr,ATA_i,ATb_i,error_i,count_i);
        error += error_i;
        count += count_i;

        if (count_i < 10) {
          std::cout << "# inliers " << count_i << " to small " << std::endl;
          continue;
        }
        // solve for x using ldlt
        Eigen::Matrix<float,6,1,Eigen::DontAlign> x =
          (ATA_i.cast<double>().ldlt().solve(ATb_i.cast<double>())).cast<float>(); 
        dT_mr[sId] = dT_mr[sId] * SE3f::Exp_(x);
      }
      if (count < 10) {
        std::cout << "# inliers " << count << " to small " << std::endl;
        break;
      }
      if (verbose) {
      std::cout << std::setprecision(2) 
        << std::scientific << "lvl " << lvl << " it " << it 
        << ": err=" << error 
        << "\tdErr/err=" << fabs(error-errPrev)/error
        << "\t# inliers: " << count 
        << std::endl;
      }
      if (it>0 && fabs(error-errPrev)/error < 1e-7) break;
      errPrev = error;
    }
    errPerLvl[lvl] = log(error);
    countPerLvl[lvl] = count;
  }

  for (size_t sId=0; sId < stream2cam.size(); sId++) {
    int32_t cId = stream2cam[sId]; 
    rig.T_rcs_[cId] = dT_mr[sId]*rig.T_rcs_[cId];
  }
}

template void ICP::ComputeProjectiveUpdateIndividual(
    Pyramid<Vector3fda,3>& pcs_m,
    Pyramid<Vector3fda,3>& ns_m,
    Pyramid<Vector3fda,3>& pcs_o,
    Pyramid<Vector3fda,3>& ns_o,
    Rig<Cameraf>& rig,
    const std::vector<int32_t>& stream2cam,
    const std::vector<size_t>& maxIt, 
    float angleThr_deg, float distThr,
    bool verbose,
    SE3f& T_mr,
    std::vector<float>& errPerLvl,
    std::vector<float>& countPerLvl
    );

template void ICP::ComputeProjectiveUpdateIndividual(
    Pyramid<Vector3fda,3>& pcs_m,
    Pyramid<Vector3fda,3>& ns_m,
    Pyramid<Vector3fda,3>& pcs_o,
    Pyramid<Vector3fda,3>& ns_o,
    Rig<CameraPoly3f>& rig,
    const std::vector<int32_t>& stream2cam,
    const std::vector<size_t>& maxIt, 
    float angleThr_deg, float distThr,
    bool verbose,
    SE3f& T_mr,
    std::vector<float>& errPerLvl,
    std::vector<float>& countPerLvl
    );

}
