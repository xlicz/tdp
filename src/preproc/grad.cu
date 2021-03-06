
#include <tdp/data/image.h>
#include <tdp/cuda/cuda.h>
#include <tdp/camera/camera.h>
#include <tdp/camera/camera_poly.h>

namespace tdp {

__global__
void KernelGradientShar(Image<float> I,
    Image<Vector2fda> gradI) {
  const int idx = threadIdx.x + blockDim.x * blockIdx.x;
  const int idy = threadIdx.y + blockDim.y * blockIdx.y;
  if (0 < idx && idx < I.w_-1 && 0 < idy && idy < I.h_-1) {
    float Iui = -3.*I(idx-1,idy-1);
    float Ivi = Iui -10.*I(idx,idy-1)-3.*I(idx+1,idy-1);
    Iui += 3.*I(idx+1,idy-1) 
      -10.*I(idx-1,idy) 
      +10.*I(idx+1,idy) 
      -3.0*I(idx-1,idy+1);
    Ivi += 3.*I(idx-1,idy+1) 
      +10.*I(idx,idy+1)
      +3.0*I(idx+1,idy+1);
    Iui += 3.*I(idx+1,idy+1);
    gradI(idx,idy)(0) = Iui*0.03125;
    gradI(idx,idy)(1) = Ivi*0.03125;
  } else if (idx == 0 || idx == I.w_-1
      || idy == 0 || idy == I.h_-1) {
    gradI(idx,idy)(0) = 0.;
    gradI(idx,idy)(1) = 0.;
  }
}

void GradientShar(const Image<float>& I,
    Image<Vector2fda>& gradI) {

  dim3 threads, blocks;
  ComputeKernelParamsForImage(blocks,threads,I,32,32);
  KernelGradientShar<<<blocks,threads>>>(I,gradI);
  checkCudaErrors(cudaDeviceSynchronize());
}

__global__
void KernelGradient2Vector(Image<float> Iu, Image<float> Iv,
    Image<Vector2fda> gradI) {
  const int idx = threadIdx.x + blockDim.x * blockIdx.x;
  const int idy = threadIdx.y + blockDim.y * blockIdx.y;
  if (idx < Iu.w_ && idy < Iu.h_) {
    float Iui = Iu(idx,idy);
    float Ivi = Iv(idx,idy);
    gradI(idx,idy) = Vector2fda(Iui,Ivi);
  }
}

void Gradient2Vector(const Image<float>& Iu, const Image<float>& Iv,
    Image<Vector2fda>& gradI) {

  dim3 threads, blocks;
  ComputeKernelParamsForImage(blocks,threads,Iu,32,32);
  KernelGradient2Vector<<<blocks,threads>>>(Iu,Iv,gradI);
  checkCudaErrors(cudaDeviceSynchronize());
}

__global__
void KernelGradient2AngleNorm(Image<float> Iu, Image<float> Iv,
    Image<float> Itheta, Image<float> Inorm) {
  const int idx = threadIdx.x + blockDim.x * blockIdx.x;
  const int idy = threadIdx.y + blockDim.y * blockIdx.y;
  if (idx < Iu.w_ && idy < Iu.h_) {
    float Iui = Iu(idx,idy);
    float Ivi = Iv(idx,idy);
    Itheta(idx,idy) = atan2(Ivi, Iui);
    Inorm(idx,idy) = sqrtf(Iui*Iui + Ivi*Ivi);
  }
}

void Gradient2AngleNorm(const Image<float>& Iu, const Image<float>& Iv,
    Image<float>& Itheta, Image<float>& Inorm) {

  dim3 threads, blocks;
  ComputeKernelParamsForImage(blocks,threads,Iu,32,32);
  KernelGradient2AngleNorm<<<blocks,threads>>>(Iu,Iv,Itheta,Inorm);
  checkCudaErrors(cudaDeviceSynchronize());
}

__global__
void KernelGradient2AngleNorm(Image<Vector2fda> gradI, 
    Image<float> Itheta, Image<float> Inorm) {
  const int idx = threadIdx.x + blockDim.x * blockIdx.x;
  const int idy = threadIdx.y + blockDim.y * blockIdx.y;
  if (idx < gradI.w_ && idy < gradI.h_) {
    float Iui = gradI(idx,idy)(0);
    float Ivi = gradI(idx,idy)(1);
    Itheta(idx,idy) = atan2(Ivi, Iui);
    Inorm(idx,idy) = sqrtf(Iui*Iui + Ivi*Ivi);
  }
}

void Gradient2AngleNorm(const Image<tdp::Vector2fda>& gradI, 
    Image<float>& Itheta, Image<float>& Inorm) {

  dim3 threads, blocks;
  ComputeKernelParamsForImage(blocks,threads,gradI,32,32);
  KernelGradient2AngleNorm<<<blocks,threads>>>(gradI,Itheta,Inorm);
  checkCudaErrors(cudaDeviceSynchronize());
}


template<int D, typename Derived>
__global__
void KernelGradient3D(Image<float> Iu, Image<float> Iv,
    Image<float> cuD,
    Image<Vector3fda> cuN,
    CameraBase<float,D,Derived> cam,
    float gradNormThr,
    Image<Vector3fda> cuGrad3D) {
  const int idx = threadIdx.x + blockDim.x * blockIdx.x;
  const int idy = threadIdx.y + blockDim.y * blockIdx.y;
  if (idx < Iu.w_ && idy < Iu.h_) {
    const float Iui = Iu(idx,idy);
    const float Ivi = Iv(idx,idy);
    const Vector3fda n = cuN(idx, idy);
    float d0 = cuD(idx,idy); 
    float norm = sqrtf(Iui*Iui + Ivi*Ivi);
    if (!isNan(d0) && IsValidNormal(n) && norm > gradNormThr) {

      //    const Vector3fda gradI(Iui, Ivi, 0.f);
      //    const Vector3fda grad3D = gradI - ((gradI.dot(n))/n.norm()) * n;
      //    cuGrad3D(idx, idy) = grad3D/grad3D.norm() * sqrtf(Iui*Iui + Ivi*Ivi);
      Vector3fda r0 = cam.Unproject(idx,idy,1.f);
      Vector3fda r1 = cam.Unproject(idx+Iui,idy+Ivi,1.f);
      float d1 = (r0.dot(n))/(r1.dot(n))*d0;
      const Vector3fda grad3D = r1*d1 - r0*d0;
      cuGrad3D(idx, idy) = grad3D/grad3D.norm() * norm;
    } else {
      cuGrad3D(idx, idy)(0) = NAN;
      cuGrad3D(idx, idy)(1) = NAN;
      cuGrad3D(idx, idy)(2) = NAN;
    }
  }
}

template<int D, typename Derived>
void Gradient3D(const Image<float>& Iu, const Image<float>& Iv,
    const Image<float>& cuD,
    const Image<Vector3fda>& cuN,
    const CameraBase<float,D,Derived>& cam,
    float gradNormThr,
    Image<Vector3fda>& cuGrad3D) {

  dim3 threads, blocks;
  ComputeKernelParamsForImage(blocks,threads,Iu,32,32);
  KernelGradient3D<<<blocks,threads>>>(Iu,Iv,cuD,cuN,cam,gradNormThr,cuGrad3D);
  checkCudaErrors(cudaDeviceSynchronize());
}

template
void Gradient3D(const Image<float>& Iu, const Image<float>& Iv,
    const Image<float>& cuD,
    const Image<Vector3fda>& cuN,
    const BaseCameraf& cam,
    float gradNormThr,
    Image<Vector3fda>& cuGrad3D);

template
void Gradient3D(const Image<float>& Iu, const Image<float>& Iv,
    const Image<float>& cuD,
    const Image<Vector3fda>& cuN,
    const BaseCameraPoly3f& cam,
    float gradNormThr,
    Image<Vector3fda>& cuGrad3D);

__global__ 
void KernelGrad2Image(
    Image<Vector2fda> grad, Image<Vector3bda> grad2d) {
  //const int tid = threadIdx.x;
  const int idx = threadIdx.x + blockDim.x * blockIdx.x;
  const int idy = threadIdx.y + blockDim.y * blockIdx.y;

  if (idx < grad.w_ && idy < grad.h_) {
    Vector2fda gradi = grad(idx,idy);
    if (IsValidData(gradi)) {
      grad2d(idx,idy)(0) = gradi(0)>1.?255:(gradi(0)<-1?0:floor(gradi(0)*128+127));
      grad2d(idx,idy)(1) = gradi(1)>1.?255:(gradi(1)<-1?0:floor(gradi(1)*128+127));
      grad2d(idx,idy)(2) = 0;
    } else {
      grad2d(idx,idy)(0) = 0;
      grad2d(idx,idy)(1) = 0;
      grad2d(idx,idy)(2) = 0;
    }
  }
}

void Grad2Image(
    const Image<Vector2fda>& grad,
    Image<Vector3bda>& grad2d
    ) {
  dim3 threads, blocks;
  ComputeKernelParamsForImage(blocks,threads,grad,32,32);
  KernelGrad2Image<<<blocks,threads>>>(grad,grad2d);
  checkCudaErrors(cudaDeviceSynchronize());
}

}
