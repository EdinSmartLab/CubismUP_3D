//
//  Cubism3D
//  Copyright (c) 2018 CSE-Lab, ETH Zurich, Switzerland.
//  Distributed under the terms of the MIT license.
//
//  Created by Guido Novati (novatig@ethz.ch).
//

#include "operators/Analysis.h"
#include <sys/stat.h>
#include <iomanip>
#include <sstream>

CubismUP_3D_NAMESPACE_BEGIN using namespace cubism;

inline void avgUx_wallNormal(Real *avgFlow_xz, const std::vector<BlockInfo>& myInfo,
                                    const Real* const uInf, const int bpdy) {
  size_t nGridPointsY = bpdy * FluidBlock::sizeY;
  const size_t nBlocks = myInfo.size();
  size_t normalize = FluidBlock::sizeX * FluidBlock::sizeZ * nBlocks/bpdy;
#pragma omp parallel for schedule(static) reduction(+ : avgFlow_xz[:2*nGridPointsY])
  for (size_t i = 0; i < nBlocks; i++) {
    const BlockInfo& info = myInfo[i];
    const FluidBlock& b = *(const FluidBlock*)info.ptrBlock;
    // Average Ux on the xz-plane for all y's :
    //     <Ux>_{xz} (y) = Sum_{ix,iz} Ux(ix, y, iz)
    int blockIdxY = info.index[1];
    for (int iy = 0; iy < FluidBlock::sizeY; ++iy) {
      size_t index = blockIdxY * FluidBlock::sizeY + iy;
      //                           sizeY
      //               <----------------------------->
      //
      //               | ... iy-1  iy  iy+1 ...      |
      // ...___,___,___|____,____,____,____,____,____|___,___,___, ...
      //
      //                     blockIdxY                  blockIdxY+1
      for (int ix = 0; ix < FluidBlock::sizeX; ++ix)
      for (int iz = 0; iz < FluidBlock::sizeZ; ++iz) {
        const Real Ux = b(ix, iy, iz).u;
        avgFlow_xz[index                 ] += Ux/normalize;
        avgFlow_xz[index +   nGridPointsY] += Ux*Ux/normalize;
      }
    }
  }
}

Analysis::Analysis(SimulationData& s) : Operator(s) {}

void Analysis::operator()(const double dt) {
  const bool bFreq = (sim.freqAnalysis!=0 && (sim.step+ 1)%sim.freqAnalysis==0);
  const bool bTime = (sim.timeAnalysis!=0 && (sim.time+dt)>sim.nextAnalysisTime);
  const bool bAnalysis =  bFreq || bTime;

  if (sim.step==0 and sim.rank==0)
    mkdir("analysis", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

  if (not bAnalysis) return;

  int nFile;
  if (bTime){
    sim.nextAnalysisTime += sim.timeAnalysis;
    nFile = (sim.time==0)? 0 : 1+(int)(sim.time/sim.timeAnalysis);
  }
  else nFile = sim.step;

  if (sim.analysis == "channel")
  {
    sim.startProfiler("Channel Analysis");

    int nGridPointsY = sim.bpdy * FluidBlock::sizeY;

    Real avgFlow_xz[2*nGridPointsY] = {};
    avgUx_wallNormal(avgFlow_xz, vInfo, sim.uinf.data(), sim.bpdy);
    MPI_Allreduce(MPI_IN_PLACE, avgFlow_xz, 2*nGridPointsY, MPI_DOUBLE,
                  MPI_SUM, grid->getCartComm());

    // avgFlow_xz = [ <Ux>_{xz}, <Ux^2>_{xz}]
    // Time average, alpha = 1.0 - dt / T_\tau
    // T_\tau = 1/2 L_y / (Re_\tau * <U>)
    // Re_\tau = 180

    /*
    const double T_tau = 0.5 * sim.extent[1] / (180 * sim.uMax_forced);
    const double alpha = (sim.step>0) ? 1.0 - dt / T_tau : 0;
    for (int i = 0; i < nGridPointsY; i++) {
      // Compute <kx>=1/2*(<Ux^2> - <Ux>^2)
      avgFlow_xz[i + nGridPointsY] = 0.5 * (avgFlow_xz[i + nGridPointsY] - avgFlow_xz[i]*avgFlow_xz[i]);
      sim.kx_avg_msr[i] = alpha * sim.kx_avg_msr[i] + (1-alpha) * avgFlow_xz[i + nGridPointsY];
      sim.Ux_avg_msr[i] = alpha * sim.Ux_avg_msr[i] + (1-alpha) * avgFlow_xz[i];
    }
    */

    if (sim.rank==0)
    {
      std::stringstream ssR;
      ssR<<"analysis/analysis_"<<std::setfill('0')<<std::setw(9)<<nFile;
      std::ofstream f;
      f.open (ssR.str());
      f << "Channel Analysis : time=" << sim.time << std::endl;
      f << std::left << std::setw(25) << "<Ux>" << std::setw(25) << "<kx>" << std::endl;
      for (int i = 0; i < nGridPointsY; i++){
        const Real kx = 0.5*(avgFlow_xz[i+nGridPointsY]/sim.nprocs - avgFlow_xz[i]*avgFlow_xz[i]/sim.nprocs/sim.nprocs);
        const Real ux = avgFlow_xz[i]/sim.nprocs;
        f << std::left << std::setw(25) << ux << std::setw(25) << kx << std::endl;
      }
    }
    sim.stopProfiler();
    check("Channel Analysis");
  }
  if (sim.analysis == "HIT")
  {
    sim.startProfiler("HIT Analysis");
    SpectralAnalysis * sA = new SpectralAnalysis(sim);
    sA->run();
    if (sim.rank==0) sA->dump2File(nFile);
    delete sA;
    sim.stopProfiler();
    check("HIT Analysis");
  }
}
CubismUP_3D_NAMESPACE_END
