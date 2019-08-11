//
//  Cubism3D
//  Copyright (c) 2018 CSE-Lab, ETH Zurich, Switzerland.
//  Distributed under the terms of the MIT license.
//
//  Created by Guido Novati (novatig@ethz.ch).
//

//#include "PoissonSolverScalarFFTW_ACC.h"
//#include <cuda_runtime_api.h>

#include "../Base.h"
#include "PoissonSolverACC_common.h"

#include <cassert>
#include <cmath>

using Real = cubismup3d::Real;

__global__
void _fourier_filter_kernel(acc_c*const __restrict__ out,
  const long Gx, const long Gy, const long Gz,
  const long nx, const long ny, const long nz,
  const long sx, const long sy, const long sz,
  const Real wx, const Real wy, const Real wz, const Real fac)
{
  const long i = blockDim.x * blockIdx.x + threadIdx.x;
  const long j = blockDim.y * blockIdx.y + threadIdx.y;
  const long k = blockDim.z * blockIdx.z + threadIdx.z;
  if( i >= nx || j >= ny || k >= nz ) return;

  const long kx = sx + i, ky = sy + j, kz = sz + k;
  const long kkx = kx > Gx/2 ? kx-Gx : kx;
  const long kky = ky > Gy/2 ? ky-Gy : ky;
  const long kkz = kz > Gz/2 ? kz-Gz : kz;
  // For some reason accfft now does this for Laplace operator:
  //const size_t kkx = kx>Gx/2 ? kx-Gx : ( kx==Gx/2 ? 0 : kx );
  //const size_t kky = ky>Gy/2 ? ky-Gy : ( ky==Gy/2 ? 0 : ky );
  //const size_t kkz = kz>Gz/2 ? kz-Gz : ( kz==Gz/2 ? 0 : kz );
  const Real rkx = kkx*wx, rky = kky*wy, rkz = kkz*wz;
  const Real kinv = kkx||kky||kkz? -fac/(rkx*rkx + rky*rky + rkz*rkz) : 0;
  //const Real kinv = fac;
  const long index = i*nz*ny + j*nz + k;
  out[index][0] *= kinv; out[index][1] *= kinv;
}


void _fourier_filter_gpu(acc_c*const __restrict__ data_hat,
 const size_t gsize[3],const int osize[3] , const int ostart[3], const Real h)
{
  const Real wfac[3] = {
    Real(2*M_PI)/(h*gsize[0]),
    Real(2*M_PI)/(h*gsize[1]),
    Real(2*M_PI)/(h*gsize[2])
  };
  const Real norm = 1.0 / ( gsize[0]*h * gsize[1]*h * gsize[2]*h );
  //const Real fac = 1.0 / ( gsize[0] * gsize[1] * gsize[2] );
  int blocksInX = std::ceil(osize[0] / 4.);
  int blocksInY = std::ceil(osize[1] / 4.);
  int blocksInZ = std::ceil(osize[2] / 4.);
  dim3 Dg(blocksInX, blocksInY, blocksInZ), Db(4, 4, 4);
  _fourier_filter_kernel<<<Dg, Db>>>(
   data_hat, gsize[0], gsize[1], gsize[2],osize[0],osize[1],osize[2],
            ostart[0],ostart[1],ostart[2], wfac[0], wfac[1], wfac[2], norm);

  //if(ostart[0]==0 && ostart[1]==0 && ostart[2]==0)
  //  cudaMemset(data_hat, 0, 2 * sizeof(Real));

  CUDA_Check( cudaDeviceSynchronize() );
}


__global__ void kPos(const int iSzX, const int iSzY, const int iSzZ,
  const int iStX, const int iStY, const int iStZ, const int nGlobX,
  const int nGlobY, const int nGlobZ, const size_t nZpad, Real*const in_out)
{
  const int i = blockDim.x * blockIdx.x + threadIdx.x;
  const int j = blockDim.y * blockIdx.y + threadIdx.y;
  const int k = blockDim.z * blockIdx.z + threadIdx.z;
  if ( (i >= iSzX) || (j >= iSzY) || (k >= iSzZ) ) return;
  const size_t linidx = k + 2*nZpad*(j + iSzY*i);
  const Real I = i + iStX, J = j + iStY, K = k + iStZ;
  in_out[linidx] = K + nGlobZ * (J + nGlobY * I);
}

__global__ void kGreen(const int iSzX, const int iSzY, const int iSzZ,
  const int iStX, const int iStY, const int iStZ,
  const int nGlobX, const int nGlobY, const int nGlobZ, const size_t nZpad,
  const Real h, Real*const in_out)
{
  const int i = blockDim.x * blockIdx.x + threadIdx.x;
  const int j = blockDim.y * blockIdx.y + threadIdx.y;
  const int k = blockDim.z * blockIdx.z + threadIdx.z;
  if ( (i >= iSzX) || (j >= iSzY) || (k >= iSzZ) ) return;
  const size_t linidx = k + 2*nZpad*(j + iSzY*i);
  const int I = i + iStX, J = j + iStY, K = k + iStZ;
  const Real xi = I>=nGlobX? 2*nGlobX-1 - I : I;
  const Real yi = J>=nGlobY? 2*nGlobY-1 - J : J;
  const Real zi = K>=nGlobZ? 2*nGlobZ-1 - K : K;
  const Real r = std::sqrt(xi*xi + yi*yi + zi*zi);
  if(r > 0) in_out[linidx] = - h * h / ( 4 * M_PI * r );
  // G = r_eq^2 / 2 = std::pow(3/8/pi/sqrt(2))^(2/3) * h^2
  else      in_out[linidx] = - Real(0.1924173658) * h * h;
  //else      in_out[linidx] = fac;
}

__global__ void kCopyC2R(const int oSzX,const int oSzY,const int oSzZ,
  const Real norm, const size_t nZpad, const acc_c*const G_hat, Real*const m_kernel)
{
  const int i = threadIdx.x + blockIdx.x * blockDim.x;
  const int j = threadIdx.y + blockIdx.y * blockDim.y;
  const int k = threadIdx.z + blockIdx.z * blockDim.z;
  if ( (i >= oSzX) || (j >= oSzY) || (k >= oSzZ) ) return;
  const size_t linidx = (i*oSzY + j)*nZpad + k;
  m_kernel[linidx] = G_hat[linidx][0] * norm;
}

__global__ void kFreespace(const int oSzX, const int oSzY, const int oSzZ,
  const size_t nZpad, const Real*const G_hat, acc_c*const in_out)
{
  const int i = threadIdx.x + blockIdx.x * blockDim.x;
  const int j = threadIdx.y + blockIdx.y * blockDim.y;
  const int k = threadIdx.z + blockIdx.z * blockDim.z;
  if ( (i >= oSzX) || (j >= oSzY) || (k >= oSzZ) ) return;
  const size_t linidx = (i*oSzY + j)*nZpad + k;
  in_out[linidx][0] *= G_hat[linidx];
  in_out[linidx][1] *= G_hat[linidx];
}

void dSolveFreespace(const int ox,const int oy,const int oz,const size_t mz_pad,
  const Real*const G_hat, Real*const gpu_rhs)
{
  dim3 dB(4, 4, 4);
  dim3 dG(std::ceil(ox/4.), std::ceil(oy/4.), std::ceil(oz/4.));
  kFreespace <<<dG, dB>>> (ox,oy,oz, mz_pad, G_hat, (acc_c*) gpu_rhs);
  CUDA_Check(cudaDeviceSynchronize());
}

void initGreen(const int *isz, const int *ist,
  int nx, int ny, int nz, const Real h, Real*const gpu_rhs)
{
  const int mz = 2*nz -1, mz_pad = mz/2 +1;
  dim3 dB(4, 4, 4);
  dim3 dG(std::ceil(isz[0]/4.), std::ceil(isz[1]/4.), std::ceil(isz[2]/4.));
  //cout<<isz[0]<<" "<<isz[1]<<" "<<isz[2]<<" "<< ist[0]<<" "<<ist[1]<<" "<<ist[2]<<" "<<nx<<" "<<ny<<" "<<nz<<" "<<mz_pad<<endl;
  kGreen<<<dG, dB>>> (isz[0],isz[1],isz[2], ist[0],ist[1],ist[2],
    nx, ny, nz, mz_pad, h, gpu_rhs);
  CUDA_Check(cudaDeviceSynchronize());
}

void realGreen(const int*osz, const int*ost, int nx, int ny, int nz,
  const Real h, Real*const m_kernel, Real*const gpu_rhs)
{
  const int mx = 2*nx -1, my = 2*ny -1, mz = 2*nz -1, mz_pad = mz/2 +1;
  const Real norm = 1.0 / (mx*h * my*h * mz*h);
  dim3 dB(4, 4, 4);
  dim3 dG(std::ceil(osz[0]/4.), std::ceil(osz[1]/4.), std::ceil(osz[2]/4.));
  kCopyC2R<<<dG, dB>>> (osz[0],osz[1],osz[2], norm, mz_pad,
    (acc_c*)gpu_rhs, m_kernel);
  CUDA_Check(cudaDeviceSynchronize());
  //{
  //  dim3 dB(4, 4, 4);
  //  dim3 dG(std::ceil(isz[0]/4.), std::ceil(isz[1]/4.), std::ceil(isz[2]/4.));
  //  kPos<<<dG, dB>>> (isz[0],isz[1],isz[2], ist[0],ist[1],ist[2], mx,my,mz, mz_pad, gpu_rhs);
  //}
}

#if __CUDACC_VER_MAJOR__ >= 9
#define MASK_ALL_WARP 0xFFFFFFFF
#define warpShflDown(var, delta)   __shfl_down_sync (MASK_ALL_WARP, var, delta)
#else
#define warpShflDown(var, delta)   __shfl_down (var, delta)
#endif


#if !defined(__CUDA_ARCH__) || __CUDA_ARCH__ >= 600
#else
__device__ double atomicAdd(double* address, double val)
{
    using ULLI_t = unsigned long long int;
    ULLI_t* address_as_ull = (ULLI_t*)address;
    ULLI_t old = *address_as_ull, assumed;
    do {
        assumed = old;
        old = atomicCAS(address_as_ull, assumed,
                        __double_as_longlong(val +
                               __longlong_as_double(assumed)));
    // Note: uses integer comparison to avoid hang in case of NaN (since NaN != NaN)
    } while (assumed != old);
    return __longlong_as_double(old);
}
#endif

__device__ inline uint32_t __laneid()
{
    uint32_t laneid;
    asm volatile("mov.u32 %0, %%laneid;" : "=r"(laneid));
    return laneid;
}

__global__
void _forcing_filter_kernel( acc_c*const __restrict__ Uhat,
  acc_c*const __restrict__ Vhat, acc_c*const __restrict__ What,
  const long Gx, const long Gy, const long Gz,
  const long nx, const long ny, const long nz,
  const long sx, const long sy, const long sz,
  const Real wx, const Real wy, const Real wz, Real * const reductionBuf)
{
  const long i = blockDim.x * blockIdx.x + threadIdx.x;
  const long j = blockDim.y * blockIdx.y + threadIdx.y;
  const long k = blockDim.z * blockIdx.z + threadIdx.z;
  Real tke = 0, eps = 0, tkeFiltered = 0;

  if( i < nx && j < ny && k < nz )
  {
    const long kx = sx + i, ky = sy + j, kz = sz + k;
    const Real kkx = kx > Gx/2 ? kx-Gx : kx;
    const Real kky = ky > Gy/2 ? ky-Gy : ky;
    const Real kkz = kz > Gz/2 ? kz-Gz : kz;
    const Real rkx = kkx*wx, rky = kky*wy, rkz = kkz*wz;
    const Real k2 = rkx*rkx + rky*rky + rkz*rkz;
    const Real mult = (kz==0) or (kz==Gz/2) ? 1 : 2;
    const long ind = i*nz*ny + j*nz + k;
    const Real EU = Uhat[ind][0] * Uhat[ind][0] + Uhat[ind][1] * Uhat[ind][1];
    const Real EV = Vhat[ind][0] * Vhat[ind][0] + Vhat[ind][1] * Vhat[ind][1];
    const Real EW = What[ind][0] * What[ind][0] + What[ind][1] * What[ind][1];
    const Real E = mult/2 * ( EU + EV + EW );
    tke = E; // Total kinetic energy
    eps = k2 * E; // Dissipation rate
    if (k2 > 0 && k2 <= 2 * 2) tkeFiltered = E;
    else {
      Uhat[ind][0] = 0; Uhat[ind][1] = 0;
      Vhat[ind][0] = 0; Vhat[ind][1] = 0;
      What[ind][0] = 0; What[ind][1] = 0;
    }
  }

#if 1
  // reduction within same warp: result is summed down to thread 0
  #pragma unroll
  for (int offset = warpSize/2; offset > 0; offset /= 2) {
    tke         = tke         + warpShflDown(tke,         offset);
    eps         = eps         + warpShflDown(eps,         offset);
    tkeFiltered = tkeFiltered + warpShflDown(tkeFiltered, offset);
  }

  if (__laneid() == 0) { // thread 0 does the only atomic sum
    atomicAdd(reductionBuf + 0, tke);
    atomicAdd(reductionBuf + 1, eps);
    atomicAdd(reductionBuf + 2, tkeFiltered);
  }
#else
  atomicAdd(reductionBuf + 0, tke);
  atomicAdd(reductionBuf + 1, eps);
  atomicAdd(reductionBuf + 2, tkeFiltered);
#endif
}


__global__
void _analysis_filter_kernel( acc_c*const __restrict__ Uhat,
  acc_c*const __restrict__ Vhat, acc_c*const __restrict__ What,
  const long Gx, const long Gy, const long Gz,
  const long nx, const long ny, const long nz,
  const long sx, const long sy, const long sz,
  const Real wx, const Real wy, const Real wz,
  const Real nyquist, const Real nyquist_scaling, Real * const reductionBuf)
{
  const long i = blockDim.x * blockIdx.x + threadIdx.x;
  const long j = blockDim.y * blockIdx.y + threadIdx.y;
  const long k = blockDim.z * blockIdx.z + threadIdx.z;
  Real tke = 0, eps = 0, tau = 0;

  if( i < nx && j < ny && k < nz )
  {
    const long kx = sx + i, ky = sy + j, kz = sz + k;
    const Real kkx = kx > Gx/2 ? kx-Gx : kx;
    const Real kky = ky > Gy/2 ? ky-Gy : ky;
    const Real kkz = kz > Gz/2 ? kz-Gz : kz;
    const Real rkx = kkx*wx, rky = kky*wy, rkz = kkz*wz;
    const Real k2 = rkx*rkx + rky*rky + rkz*rkz;
    const Real ks = std::sqrt(kkx*kkx + kky*kky + kkz*kkz);
    const Real mult = (kz==0) or (kz==Gz/2) ? 1 : 2;
    const long ind = i*nz*ny + j*nz + k;
    const Real EU = Uhat[ind][0] * Uhat[ind][0] + Uhat[ind][1] * Uhat[ind][1];
    const Real EV = Vhat[ind][0] * Vhat[ind][0] + Vhat[ind][1] * Vhat[ind][1];
    const Real EW = What[ind][0] * What[ind][0] + What[ind][1] * What[ind][1];
    const Real E = mult/2 * ( EU + EV + EW );
    tke = E; // Total kinetic energy
    eps = k2 * E; // Dissipation rate
    tau = (k2 > 0) ? E / std::sqrt(k2) : 0; // Large eddy turnover time
    if (ks <= nyquist) {
      const int binID = std::floor(ks * nyquist_scaling);
      // reduction buffer here holds also the energy spectrum, shifted by 3
      // to hold also tke, eps and tau
      atomicAdd(reductionBuf + 3 + binID, E);
    }
  }

  #pragma unroll
  for (int offset = warpSize/2; offset > 0; offset /= 2) {
    tke = tke + warpShflDown(tke, offset);
    eps = eps + warpShflDown(eps, offset);
    tau = tau + warpShflDown(tau, offset);
  }

  if (__laneid() == 0) { // thread 0 does the only atomic sum
    atomicAdd(reductionBuf + 0, tke);
    atomicAdd(reductionBuf + 1, eps);
    atomicAdd(reductionBuf + 2, tau);
  }
}

void _compute_HIT_forcing(
  acc_c*const Uhat, acc_c*const Vhat, acc_c*const What,
  const size_t gsize[3], const int osize[3] , const int ostart[3], const Real h,
  Real & tke, Real & eps, Real & tkeFiltered
)
{
  Real * rdxBuf;
  cudaMalloc((void**)& rdxBuf, 3 * sizeof(Real));
  cudaMemset(rdxBuf, 0, 3 * sizeof(Real));

  const Real wfac[3] = {
    Real(2*M_PI)/(h*gsize[0]),
    Real(2*M_PI)/(h*gsize[1]),
    Real(2*M_PI)/(h*gsize[2])
  };
  int blocksInX = std::ceil(osize[0] / 4.);
  int blocksInY = std::ceil(osize[1] / 4.);
  int blocksInZ = std::ceil(osize[2] / 4.);
  dim3 Dg(blocksInX, blocksInY, blocksInZ), Db(4, 4, 4);
  _forcing_filter_kernel<<<Dg, Db>>>( Uhat, Vhat, What,
     gsize[0], gsize[1], gsize[2], osize[0],osize[1],osize[2],
    ostart[0],ostart[1],ostart[2],  wfac[0], wfac[1], wfac[2], rdxBuf);

  CUDA_Check( cudaDeviceSynchronize() );
  cudaMemcpy(&tke,         rdxBuf+0, sizeof(Real), cudaMemcpyDeviceToHost);
  cudaMemcpy(&eps,         rdxBuf+1, sizeof(Real), cudaMemcpyDeviceToHost);
  cudaMemcpy(&tkeFiltered, rdxBuf+2, sizeof(Real), cudaMemcpyDeviceToHost);
  CUDA_Check( cudaDeviceSynchronize() );
  cudaFree(rdxBuf);
}

void _compute_HIT_analysis(
  acc_c*const Uhat, acc_c*const Vhat, acc_c*const What,
  const size_t gsize[3], const int osize[3] , const int ostart[3], const Real h,
  Real & tke, Real & eps, Real & tau, Real * const eSpectrum,
  const size_t nBins, const Real nyquist
)
{
  Real * rdxBuf;
  // single buffer to contain both eps, tke, tau and spectrum
  cudaMalloc((void**)& rdxBuf, (3+nBins) * sizeof(Real));
  cudaMemset(rdxBuf, 0, (3+nBins) * sizeof(Real));

  const Real wfac[3] = {
    Real(2*M_PI)/(h*gsize[0]),
    Real(2*M_PI)/(h*gsize[1]),
    Real(2*M_PI)/(h*gsize[2])
  };
  const Real nyquist_scaling = ((int)nyquist-1) / (int)nyquist;
  int blocksInX = std::ceil(osize[0] / 4.);
  int blocksInY = std::ceil(osize[1] / 4.);
  int blocksInZ = std::ceil(osize[2] / 4.);
  dim3 Dg(blocksInX, blocksInY, blocksInZ), Db(4, 4, 4);
  _analysis_filter_kernel<<<Dg, Db>>>( Uhat, Vhat, What,
     gsize[0], gsize[1], gsize[2], osize[0],osize[1],osize[2],
    ostart[0],ostart[1],ostart[2],  wfac[0], wfac[1], wfac[2],
    nyquist, nyquist_scaling, rdxBuf);

  //if(ostart[0]==0 && ostart[1]==0 && ostart[2]==0)
  //  cudaMemset(data_hat, 0, 2 * sizeof(Real));

  CUDA_Check( cudaDeviceSynchronize() );
  cudaMemcpy(&tke,      rdxBuf+0, sizeof(Real), cudaMemcpyDeviceToHost);
  cudaMemcpy(&eps,      rdxBuf+1, sizeof(Real), cudaMemcpyDeviceToHost);
  cudaMemcpy(&tau,      rdxBuf+2, sizeof(Real), cudaMemcpyDeviceToHost);
  cudaMemcpy(eSpectrum, rdxBuf+3, nBins * sizeof(Real), cudaMemcpyDeviceToHost);
  CUDA_Check( cudaDeviceSynchronize() );
  cudaFree(rdxBuf);
}
