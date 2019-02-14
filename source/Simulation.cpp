//
//  Cubism3D
//  Copyright (c) 2018 CSE-Lab, ETH Zurich, Switzerland.
//  Distributed under the terms of the MIT license.
//
//  Written by Guido Novati (novatig@ethz.ch).
//
#include "Simulation.h"
#include "obstacles/IF3D_ObstacleVector.h"

#include "operators/CoordinatorAdvectDiffuse.h"
//#include "operators/CoordinatorAdvection.h"
#include "operators/CoordinatorComputeDissipation.h"
#include "operators/CoordinatorComputeShape.h"
//#include "operators/CoordinatorDiffusion.h"
#include "operators/CoordinatorFadeOut.h"
#include "operators/CoordinatorIC.h"
//#include "operators/CoordinatorPenalization.h"
#include "operators/CoordinatorPressure.h"
#include "operators/CoordinatorPressureGrad.h"

//#include "CoordinatorVorticity.h"
#include "obstacles/IF3D_ObstacleFactory.h"
#include "utils/ProcessOperatorsOMP.h"
#include "Cubism/HDF5Dumper_MPI.h"

#include <iomanip>

/*
 * Initialization from cmdline arguments is done in few steps, because grid has
 * to be created before the obstacles and slices are created.
 */
Simulation::Simulation(MPI_Comm mpicomm) : sim(mpicomm) {}
Simulation::Simulation(MPI_Comm mpicomm, ArgumentParser &parser)
    : sim(mpicomm, parser)
{
  // ========= SETUP GRID ==========
  // Grid has to be initialized before slices and obstacles.
  setupGrid();

  // =========== SLICES ============
  #ifdef CUP_ASYNC_DUMP
    sim.m_slices = SliceType::getEntities<SliceType>(parser, * sim.dump);
  #else
    sim.m_slices = SliceType::getEntities<SliceType>(parser, * sim.grid);
  #endif

  // ========== OBSTACLES ==========
  IF3D_ObstacleFactory factory(sim);
  setObstacleVector(new IF3D_ObstacleVector(sim, factory.create(parser)));

  // ============ INIT =============
  const bool bRestart = parser("-restart").asBool(false);
  _init(bRestart);
}

const std::vector<IF3D_ObstacleOperator*>& Simulation::getObstacleVector() const
{
    return sim.obstacle_vector->getObstacleVector();
}

// For Python bindings. Really no need for `const` here...
Simulation::Simulation(
  std::array<int, 3> cells, std::array<int, 3> nproc,
  MPI_Comm comm, int nsteps, double endTime,
  double nu, double CFL, double lambda, double DLM,
  std::array<double, 3> uinf,
  bool verbose, bool computeDissipation, bool b3Ddump, bool b2Ddump,
  double fadeOutLength, int saveFreq, double saveTime,
  const std::string &path4serialization, bool restart) : sim(comm)
{
  sim.nprocsx = nproc[0];
  sim.nprocsy = nproc[1];
  sim.nprocsz = nproc[2];
  sim.nsteps = nsteps;
  sim.endTime = endTime;
  sim.uinf[0] = uinf[0];
  sim.uinf[1] = uinf[1];
  sim.uinf[2] = uinf[2];
  sim.nu = nu;
  sim.CFL = CFL;
  sim.lambda = lambda;
  sim.DLM = DLM;
  sim.verbose = verbose;
  sim.computeDissipation = computeDissipation;
  sim.b3Ddump = b3Ddump;
  sim.b2Ddump = b2Ddump;
  sim.fadeOutLength[0] = fadeOutLength;
  sim.fadeOutLength[1] = fadeOutLength;
  sim.fadeOutLength[2] = fadeOutLength;
  sim.saveFreq = saveFreq;
  sim.saveTime = saveTime;
  sim.path4serialization = path4serialization;

  if (cells[0] < 0 || cells[1] < 0 || cells[2] < 0)
    throw std::invalid_argument("N. of cells not provided.");
  if (   cells[0] % FluidBlock::sizeX != 0
      || cells[1] % FluidBlock::sizeY != 0
      || cells[2] % FluidBlock::sizeZ != 0 )
    throw std::invalid_argument("N. of cells must be multiple of block size.");

  sim.bpdx = cells[0] / FluidBlock::sizeX;
  sim.bpdy = cells[1] / FluidBlock::sizeY;
  sim.bpdz = cells[2] / FluidBlock::sizeZ;
  sim._argumentsSanityCheck();
  setupGrid();  // Grid has to be initialized before slices and obstacles.
  setObstacleVector(new IF3D_ObstacleVector(sim));
  _init(restart);
}

void Simulation::_init(const bool restart)
{
  setupOperators();
  if (restart) _deserialize();
  else _ic();
  MPI_Barrier(sim.app_comm);
}

void Simulation::_ic()
{
  CoordinatorIC coordIC(sim);
  sim.startProfiler(coordIC.getName());
  coordIC(0);
  sim.stopProfiler();
}

void Simulation::setupGrid()
{
  assert(sim.bpdx > 0);
  assert(sim.bpdy > 0);
  assert(sim.bpdz > 0);

  if (sim.nprocsy < 0)  sim.nprocsy = 1;
  if (sim.nprocsy == 1) sim.nprocsx = sim.nprocs;  // Override existing value!
  else if (sim.nprocsx < 0) sim.nprocsx = sim.nprocs / sim.nprocsy;
  sim.nprocsz = 1;

  if (sim.nprocsx * sim.nprocsy * sim.nprocsz != sim.nprocs) {
    fprintf(stderr, "Invalid domain decomposition. %d x %d x %d != %d!\n",
            sim.nprocsx, sim.nprocsy, sim.nprocsz, sim.nprocs);
    MPI_Abort(sim.app_comm, 1);
  }

  if ( sim.bpdx%sim.nprocsx != 0 ||
       sim.bpdy%sim.nprocsy != 0 ||
       sim.bpdz%sim.nprocsz != 0   ) {
    printf("Incompatible domain decomposition: bpd*/nproc* should be an integer");
    MPI_Abort(sim.app_comm, 1);
  }

  sim.bpdx /= sim.nprocsx;
  sim.bpdy /= sim.nprocsy;
  sim.bpdz /= sim.nprocsz;
  sim.grid = new FluidGridMPI(sim.nprocsx,sim.nprocsy,sim.nprocsz,
                              sim.bpdx,sim.bpdy,sim.bpdz,
                              sim.maxextent, sim.app_comm);
  assert(sim.grid != NULL);

  #ifdef CUP_ASYNC_DUMP
    // create new comm so that if there is a barrier main work is not affected
    MPI_Comm_split(sim.app_comm, 0, sim.rank, &sim.dump_comm);
    sim.dump = new  DumpGridMPI(sim.nprocsx,sim.nprocsy,sim.nprocsz,
                                sim.bpdx,sim.bpdy,sim.bpdz,
                                sim.maxextent, sim.dump_comm);
  #endif

  char hostname[1024];
  hostname[1023] = '\0';
  gethostname(hostname, 1023);
  const int nthreads = omp_get_max_threads();
  printf("Rank %d (of %d) with %d threads on host Hostname: %s\n",
          sim.rank, sim.nprocs, nthreads, hostname);
  //if (communicator not_eq nullptr) //Yo dawg I heard you like communicators.
  //  communicator->comm_MPI = grid->getCartComm();
  fflush(0);
  if(sim.rank==0)
  {
    printf("Blocks per dimension: [%d %d %d]\n",sim.bpdx,sim.bpdy,sim.bpdz);
    printf("Nranks per dimension: [%d %d %d]\n",sim.nprocsx,sim.nprocsy,sim.nprocsz);
  }

  const double NFE[3] = {
      (double) sim.grid->getBlocksPerDimension(0) * FluidBlock::sizeX,
      (double) sim.grid->getBlocksPerDimension(1) * FluidBlock::sizeY,
      (double) sim.grid->getBlocksPerDimension(2) * FluidBlock::sizeZ,
  };
  const double maxbpd = std::max({NFE[0], NFE[1], NFE[2]});
  sim.extent[0] = (NFE[0]/maxbpd) * sim.maxextent;
  sim.extent[1] = (NFE[1]/maxbpd) * sim.maxextent;
  sim.extent[2] = (NFE[2]/maxbpd) * sim.maxextent;
}

void Simulation::setObstacleVector(IF3D_ObstacleVector * const obstacle_vector_)
{
  sim.obstacle_vector = obstacle_vector_;

  if (sim.rank == 0)
  {
    const double maxU = std::max({sim.uinf[0], sim.uinf[1], sim.uinf[2]});
    const double length = sim.obstacle_vector->getD();
    const double re = length * std::max(maxU, length) / sim.nu;
    assert(length > 0 || sim.obstacle_vector->getObstacleVector().empty());
    printf("Kinematic viscosity:%f, Re:%f, length scale:%f\n",sim.nu,re,length);
  }
}

void Simulation::setupOperators()
{
  sim.pipeline.clear();
  sim.pipeline.push_back(new CoordinatorComputeShape(sim));

  sim.pipeline.push_back(new CoordinatorComputeDiagnostics(sim));

  sim.pipeline.push_back(new CoordinatorAdvectDiffuse<LabMPI>(sim));
  if(sim.uMax_forced > 0 && sim.initCond not_eq "taylorGreen")
    sim.pipeline.push_back(new CoordinatorPressureGradient(sim));

  sim.pipeline.push_back(new CoordinatorPressure<LabMPI>(sim));
  sim.pipeline.push_back(new CoordinatorComputeForces(sim));
  //if(sim.computeDissipation)
  //  sim.pipeline.push_back(new CoordinatorComputeDissipation<LabMPI>(grid,nu,&step,&time));

  #ifndef CUP_UNBOUNDED_FFT
    sim.pipeline.push_back(new CoordinatorFadeOut(sim));
  #endif /* CUP_UNBOUNDED_FFT */

  if(sim.rank==0) {
    printf("Coordinator/Operator ordering:\n");
    for (size_t c=0; c<sim.pipeline.size(); c++)
      printf("\t%s\n", sim.pipeline[c]->getName().c_str());
  }
  //immediately call create!
  (*sim.pipeline[0])(0);
}

double Simulation::calcMaxTimestep()
{
  double locMaxU = (double)findMaxUOMP(sim.vInfo(), * sim.grid, sim.uinf);
  double globMaxU;
  const double h = sim.vInfo()[0].h_gridpoint;

  MPI_Allreduce(&locMaxU, &globMaxU, 1, MPI_DOUBLE,MPI_MAX, sim.app_comm);
  const double dtDif = sim.CFL*h*h/sim.nu;
  const double dtAdv = sim.CFL*h/(std::fabs(globMaxU)+1e-8);
  sim.dt = std::min(dtDif, dtAdv);
  if ( sim.step < sim.rampup )
  {
    const double x = (sim.step+1.0)/sim.rampup;
    const double rampCFL = std::exp(std::log(1e-3)*(1-x) + std::log(sim.CFL)*x);
    sim.dt = rampCFL * std::min(dtDif, dtAdv);
  }
  // if DLM>=1, adapt lambda such that penal term is independent of time step
  if (sim.DLM >= 1) sim.lambda = sim.DLM / sim.dt;
  if (sim.verbose)
    printf("maxU %f dtF %f dtC %f dt %f\n", globMaxU, dtDif, dtAdv, sim.dt);
  return sim.dt;
}

void Simulation::_serialize(const std::string append)
{
  if( ! sim.bDump ) return;

  std::stringstream ssR;
  if (append == "") ssR<<"restart_";
  else ssR<<append;
  ssR<<std::setfill('0')<<std::setw(9)<<sim.step;
  const std::string fpath = sim.path4serialization + "/" + ssR.str();
  if(sim.rank==0) std::cout<<"Saving to "<<fpath<<"\n";

  if (sim.rank==0) { //rank 0 saves step id and obstacles
    sim.obstacle_vector->save(sim.step,sim.time,fpath);
    //safety status in case of crash/timeout during grid save:
    FILE * f = fopen((fpath+".status").c_str(), "w");
    assert(f != NULL);
    fprintf(f, "time: %20.20e\n", sim.time);
    fprintf(f, "stepid: %d\n", (int)sim.step);
    fprintf(f, "uinfx: %20.20e\n", sim.uinf[0]);
    fprintf(f, "uinfy: %20.20e\n", sim.uinf[1]);
    fprintf(f, "uinfz: %20.20e\n", sim.uinf[2]);
    fclose(f);
  }

  #ifdef CUBISM_USE_HDF
  std::stringstream ssF;
  if (append == "")
   ssF<<"avemaria_"<<std::setfill('0')<<std::setw(9)<<sim.step;
  else
   ssF<<"2D_"<<append<<std::setfill('0')<<std::setw(9)<<sim.step;

  #ifdef CUP_ASYNC_DUMP
    // if a thread was already created, make sure it has finished
    if(sim.dumper not_eq nullptr) {
      sim.dumper->join();
      delete dumper;
      sim.dumper = nullptr;
    }
    // copy qois from grid to dump
    copyDumpGrid(* sim.grid, * sim.dump);
    const auto & grid2Dump = * sim.dump;
  #else //CUP_ASYNC_DUMP
    const auto & grid2Dump = * sim.grid;
  #endif //CUP_ASYNC_DUMP

  const auto name3d = ssR.str(), name2d = ssF.str(); // sstreams are weird

  const auto dumpFunction = [=] ()
  {
    if(sim.b2Ddump)
    {
      for (const auto& slice : sim.m_slices)
      {
        DumpSliceHDF5MPI<StreamerVelocityVector, DumpReal>(
          slice, sim.step, sim.time, StreamerVelocityVector::prefix()+name2d,
          sim.path4serialization);
        DumpSliceHDF5MPI<StreamerPressure, DumpReal>(
          slice, sim.step, sim.time, StreamerPressure::prefix()+name2d,
          sim.path4serialization);
        DumpSliceHDF5MPI<StreamerChi, DumpReal>(
          slice, sim.step, sim.time, StreamerChi::prefix()+name2d,
          sim.path4serialization);
      }
    }
    if(sim.b3Ddump)
    {
      DumpHDF5_MPI<StreamerVelocityVector, DumpReal>(
        grid2Dump, sim.step, sim.time, StreamerVelocityVector::prefix()+name3d,
        sim.path4serialization);
      DumpHDF5_MPI<StreamerPressure, DumpReal>(
        grid2Dump, sim.step, sim.time, StreamerPressure::prefix()+name3d,
        sim.path4serialization);
      DumpHDF5_MPI<StreamerChi, DumpReal>(
        grid2Dump, sim.step, sim.time, StreamerChi::prefix()+name3d,
        sim.path4serialization);
    }
  };

  #ifdef CUP_ASYNC_DUMP
    sim.dumper = new std::thread( dumpFunction );
  #else //CUP_ASYNC_DUMP
    dumpFunction();
  #endif //CUP_ASYNC_DUMP
  #endif //CUBISM_USE_HDF


  if (sim.rank==0)
  { //saved the grid! Write status to remember most recent ping
    std::string restart_status = sim.path4serialization+"/restart.status";
    FILE * f = fopen(restart_status.c_str(), "w");
    assert(f != NULL);
    fprintf(f, "time: %20.20e\n", sim.time);
    fprintf(f, "stepid: %d\n", (int)sim.step);
    fprintf(f, "uinfx: %20.20e\n", sim.uinf[0]);
    fprintf(f, "uinfy: %20.20e\n", sim.uinf[1]);
    fprintf(f, "uinfz: %20.20e\n", sim.uinf[2]);
    fclose(f);
    printf("time:  %20.20e\n", sim.time);
    printf("stepid: %d\n", (int)sim.step);
    printf("uinfx: %20.20e\n", sim.uinf[0]);
    printf("uinfy: %20.20e\n", sim.uinf[1]);
    printf("uinfz: %20.20e\n", sim.uinf[2]);
  }

  //CoordinatorDiagnostics coordDiags(grid,time,step);
  //coordDiags(dt);
  //obstacle_vector->interpolateOnSkin(time, step);
}

void Simulation::_deserialize()
{
  {
    std::string restartfile = sim.path4serialization+"/restart.status";
    FILE * f = fopen(restartfile.c_str(), "r");
    if (f == NULL) {
      printf("Could not restart... starting a new sim.\n");
      return;
    }
    assert(f != NULL);
    bool ret = true;
    ret = ret && 1==fscanf(f, "time: %le\n",   &sim.time);
    ret = ret && 1==fscanf(f, "stepid: %d\n", &sim.step);
    #ifndef CUP_SINGLE_PRECISION
    ret = ret && 1==fscanf(f, "uinfx: %le\n", &sim.uinf[0]);
    ret = ret && 1==fscanf(f, "uinfy: %le\n", &sim.uinf[1]);
    ret = ret && 1==fscanf(f, "uinfz: %le\n", &sim.uinf[2]);
    #else // CUP_SINGLE_PRECISION
    ret = ret && 1==fscanf(f, "uinfx: %e\n", &sim.uinf[0]);
    ret = ret && 1==fscanf(f, "uinfy: %e\n", &sim.uinf[1]);
    ret = ret && 1==fscanf(f, "uinfz: %e\n", &sim.uinf[2]);
    #endif // CUP_SINGLE_PRECISION
    fclose(f);
    if( (not ret) || sim.step<0 || sim.time<0) {
      printf("Error reading restart file. Aborting...\n");
      MPI_Abort(sim.grid->getCartComm(), 1);
    }
  }

  std::stringstream ssR;
  ssR<<"restart_"<<std::setfill('0')<<std::setw(9)<<sim.step;
  if (sim.rank==0) std::cout << "Restarting from " << ssR.str() << "\n";

  #ifdef CUBISM_USE_HDF
    ReadHDF5_MPI<StreamerVelocityVector, DumpReal>(* sim.grid,
      StreamerVelocityVector::prefix()+ssR.str(), sim.path4serialization);
  #else
    printf("Unable to restart without  HDF5 library. Aborting...\n");
    MPI_Abort(sim.grid->getCartComm(), 1);
  #endif

  sim.obstacle_vector->restart(sim.time, sim.path4serialization+"/"+ssR.str());

  printf("DESERIALIZATION: time is %f and step id is %d\n", sim.time, sim.step);
  // prepare time for next save
  sim.nextSaveTime = sim.time + sim.saveTime;
}

void Simulation::run()
{
    for (;;) {
        sim.startProfiler("DT");
        const double dt = calcMaxTimestep();
        sim.stopProfiler();

        if (timestep(dt)) break;
    }
}

bool Simulation::timestep(const double dt)
{
    const bool bDumpFreq = (sim.saveFreq>0 && (sim.step+ 1)%sim.saveFreq==0);
    const bool bDumpTime = (sim.saveTime>0 && (sim.time+dt)>sim.nextSaveTime);
    if (bDumpTime) sim.nextSaveTime += sim.saveTime;
    sim.bDump = (bDumpFreq || bDumpTime);

    for (size_t c=0; c<sim.pipeline.size(); c++) {
      sim.startProfiler(sim.pipeline[c]->getName());
      (*sim.pipeline[c])(dt);
      sim.stopProfiler();
    }
    sim.step++;
    sim.time+=dt;

    if(sim.verbose) printf("%d : %f uInf {%f %f %f}\n",
      sim.step,sim.time,sim.uinf[0],sim.uinf[1],sim.uinf[2]);

    sim.startProfiler("Save");
    _serialize();
    sim.stopProfiler();

    if (sim.step % 10 == 0 && sim.verbose) sim.printResetProfiler();
    if ((sim.endTime>0 && sim.time>sim.endTime) ||
        (sim.nsteps!=0 && sim.step>=sim.nsteps) ) {
      if(sim.verbose)
        std::cout<<"Finished at time "<<sim.time<<" in "<<sim.step<<" steps.\n";
      return true;  // Finished.
    }

    return false;  // Not yet finished.
}
