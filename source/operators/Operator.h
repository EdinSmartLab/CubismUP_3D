//
//  Cubism3D
//  Copyright (c) 2018 CSE-Lab, ETH Zurich, Switzerland.
//  Distributed under the terms of the MIT license.
//
//  Created by Guido Novati (novatig@ethz.ch) and Christian Conti.
//

#ifndef CubismUP_3D_Operator_h
#define CubismUP_3D_Operator_h

#include "SimulationData.h"

class Operator
{
 protected:
  SimulationData & sim;
  FluidGridMPI * const grid = sim.grid;
  const std::vector<BlockInfo>& vInfo = sim.vInfo();

  inline void check(std::string infoText)
  {
    #ifndef NDEBUG
    int rank;
    MPI_Comm comm = grid->getCartComm();
    MPI_Comm_rank(comm,&rank);
    MPI_Barrier(comm);

    #pragma omp parallel for schedule(static)
    for (int i = 0; i < (int)vInfo.size(); ++i)
    {
      BlockInfo info = vInfo[i];
      FluidBlock& b = *(FluidBlock*)info.ptrBlock;

      for(int iz=0; iz<FluidBlock::sizeZ; ++iz)
      for(int iy=0; iy<FluidBlock::sizeY; ++iy)
      for(int ix=0; ix<FluidBlock::sizeX; ++ix)
        if (std::isnan(b(ix,iy,iz).u) || std::isnan(b(ix,iy,iz).v) ||
            std::isnan(b(ix,iy,iz).w) || std::isnan(b(ix,iy,iz).p) ) {
          fflush(stderr);
          std::cout << "GenericCoordinator::check isnan " << infoText.c_str() << std::endl;
          MPI_Abort(comm, 1);
        }
    }
    MPI_Barrier(comm);
    #endif
  }

  template <typename Kernel>
  void compute(const std::vector<Kernel*>& kernels)
  {
    SynchronizerMPI<Real>& Synch = grid->sync(*(kernels[0]));
    const int nthreads = omp_get_max_threads();
    LabMPI * labs = new LabMPI[nthreads];
    #pragma omp parallel for schedule(static, 1)
    for(int i = 0; i < nthreads; ++i) {
      labs[i].setBC(sim.BCx_flag, sim.BCy_flag, sim.BCz_flag);
      labs[i].prepare(* sim.grid, Synch);
    }

    int rank;
    MPI_Comm_rank(grid->getCartComm(), &rank);
    MPI_Barrier(grid->getCartComm());
    std::vector<BlockInfo> avail0 = Synch.avail_inner();
    const int Ninner = avail0.size();

    #pragma omp parallel
    {
      int tid = omp_get_thread_num();
      Kernel& kernel = * (kernels[tid]); LabMPI& lab = labs[tid];

      #pragma omp for schedule(static)
      for(int i=0; i<Ninner; i++) {
        const BlockInfo& I = avail0[i];
        FluidBlock& b = *(FluidBlock*)I.ptrBlock;
        lab.load(I, 0);
        kernel(lab, I, b);
      }
    }

    std::vector<BlockInfo> avail1 = Synch.avail_halo();
    const int Nhalo = avail1.size();

    #pragma omp parallel
    {
      int tid = omp_get_thread_num();
      Kernel& kernel = * (kernels[tid]); LabMPI& lab = labs[tid];

      #pragma omp for schedule(static)
      for(int i=0; i<Nhalo; i++) {
        const BlockInfo& I = avail1[i];
        FluidBlock& b = *(FluidBlock*)I.ptrBlock;
        lab.load(I, 0);
        kernel(lab, I, b);
      }
    }

    if(labs!=NULL) {
      delete [] labs;
      labs=NULL;
    }

    MPI_Barrier(grid->getCartComm());
  }

  template <typename Kernel>
  void compute(const Kernel& kernel)
  {
    SynchronizerMPI<Real>& Synch = grid->sync(kernel);
    const int nthreads = omp_get_max_threads();
    LabMPI * labs = new LabMPI[nthreads];
    #pragma omp parallel for schedule(static, 1)
    for(int i = 0; i < nthreads; ++i) {
      labs[i].setBC(sim.BCx_flag, sim.BCy_flag, sim.BCz_flag);
      labs[i].prepare(* sim.grid, Synch);
    }

    int rank;
    MPI_Comm_rank(grid->getCartComm(), &rank);
    MPI_Barrier(grid->getCartComm());
    std::vector<BlockInfo> avail0 = Synch.avail_inner();
    const int Ninner = avail0.size();

    #pragma omp parallel
    {
      int tid = omp_get_thread_num();
      LabMPI& lab = labs[tid];

      #pragma omp for schedule(static)
      for(int i=0; i<Ninner; i++) {
        BlockInfo I = avail0[i];
        FluidBlock& b = *(FluidBlock*)I.ptrBlock;
        lab.load(I, 0);
        kernel(lab, I, b);
      }
    }

    std::vector<BlockInfo> avail1 = Synch.avail_halo();
    const int Nhalo = avail1.size();

    #pragma omp parallel
    {
      int tid = omp_get_thread_num();
      LabMPI& lab = labs[tid];

      #pragma omp for schedule(static)
      for(int i=0; i<Nhalo; i++) {
        BlockInfo I = avail1[i];
        FluidBlock& b = *(FluidBlock*)I.ptrBlock;
        lab.load(I, 0);
        kernel(lab, I, b);
      }
    }

    if(labs != nullptr) {
      delete [] labs;
      labs = nullptr;
    }

    MPI_Barrier(grid->getCartComm());
  }

public:
  Operator(SimulationData & s) : sim(s) {  }
  virtual ~Operator() {}
  virtual void operator()(const double dt) = 0;
  virtual std::string getName() = 0;
};

#endif
