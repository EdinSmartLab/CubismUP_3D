//
//  CubismUP_2D
//  Copyright (c) 2018 CSE-Lab, ETH Zurich, Switzerland.
//  Distributed under the terms of the MIT license.
//
//  Created by Guido Novati (novatig@ethz.ch).
//


#include "PoissonSolverHYPREMixed.h"
#ifdef CUP_HYPRE
#ifndef CUP_SINGLE_PRECISION
#define MPIREAL MPI_DOUBLE
#else
#define MPIREAL MPI_FLOAT
#endif /* CUP_SINGLE_PRECISION */

void PoissonSolverMixed_HYPRE::solve()
{
  sim.startProfiler("HYPRE cub2rhs");
  _cub2fftw();
  sim.stopProfiler();

  if(not bPeriodic)
  {
    static constexpr int SHIFT = 1;
    data[myN[0]*myN[1]*myN[2]-1] = pLast;
    const size_t corner_m1x = linaccess(myN[0]-1-SHIFT, myN[1]-1, myN[2]-1);
    const size_t corner_m1y = linaccess(myN[0]-1, myN[1]-1-SHIFT, myN[2]-1);
    const size_t corner_m1z = linaccess(myN[0]-1, myN[1]-1, myN[2]-1-SHIFT);
    data[corner_m1x] -= pLast;
    data[corner_m1y] -= pLast;
    data[corner_m1z] -= pLast;
  }

  sim.startProfiler("HYPRE setBoxV");
  HYPRE_StructVectorSetBoxValues(hypre_rhs, ilower, iupper, data);
  sim.stopProfiler();

  sim.startProfiler("HYPRE solve");
  if (solver == "gmres")
    HYPRE_StructGMRESSolve(hypre_solver, hypre_mat, hypre_rhs, hypre_sol);
  else if (solver == "smg")
    HYPRE_StructSMGSolve(hypre_solver, hypre_mat, hypre_rhs, hypre_sol);
  else
    HYPRE_StructPCGSolve(hypre_solver, hypre_mat, hypre_rhs, hypre_sol);
  sim.stopProfiler();

  sim.startProfiler("HYPRE getBoxV");
  HYPRE_StructVectorGetBoxValues(hypre_sol, ilower, iupper, data);
  sim.stopProfiler();

  sim.startProfiler("HYPRE mean0");
  {
    Real avgP = 0;
    const size_t dofNum = gsize[0] * gsize[1] * gsize[2];
    const Real fac = 1.0 / dofNum;
    // Compute average pressure across all ranks:
    #pragma omp parallel for schedule(static) reduction(+ : avgP)
    for (size_t i = 0; i < dofNum; i++) avgP += fac * data[i];
    MPI_Allreduce(MPI_IN_PLACE, &avgP, 1, MPIREAL, MPI_SUM, m_comm);
    // Subtract average pressure from all gridpoints
    #pragma omp parallel for schedule(static)
    for (size_t i = 0; i < dofNum; i++) data[i] -= avgP;
    // Set this new mean-0 pressure as next guess
    HYPRE_StructVectorSetBoxValues(hypre_sol, ilower, iupper, data);
    // Save pressure of a corner of the grid so that it can be imposed next time
    pLast = data[myN[0]*myN[1]*myN[2]-1];
    printf("Avg Pressure:%f\n", avgP);
  }
  sim.stopProfiler();

  sim.startProfiler("HYPRE rhs2cub");
  _fftw2cub();
  sim.stopProfiler();
}

PoissonSolverMixed_HYPRE::PoissonSolverMixed_HYPRE(SimulationData&s) :
  PoissonSolver(s), solver("smg")
{
  stridez = myN[2];
  stridey = myN[1];
  printf("Employing HYPRE-based Poisson solver with Dirichlet BCs.\n");
  data = new Real[myN[0] * myN[1] * myN[2]];

  // Grid
  HYPRE_StructGridCreate(m_comm, 3, &hypre_grid);

  HYPRE_StructGridSetExtents(hypre_grid, ilower, iupper);

  //HYPRE_Int ghosts[3] = {2, 2, 2};
  //HYPRE_StructGridSetNumGhost(hypre_grid, ghosts);

  if(bPeriodic)
  {
    // if grid is periodic, this function takes the period
    // length... ie. the grid size.
    HYPRE_Int iend[] = {(int)gsize[0] -1, (int)gsize[1] -1, (int)gsize[2] -1};
    HYPRE_StructGridSetPeriodic(hypre_grid, iend);
  }

  HYPRE_StructGridAssemble(hypre_grid);

  { // Stencil
    HYPRE_Int offsets[7][3] = {{ 0, 0, 0},
                               {-1, 0, 0}, { 1, 0, 0},
                               { 0,-1, 0}, { 0, 1, 0},
                               { 0, 0,-1}, { 0, 0, 1}};
    HYPRE_StructStencilCreate(3, 7, &hypre_stencil);
    for (int j = 0; j < 7; ++j)
      HYPRE_StructStencilSetElement(hypre_stencil, j, offsets[j]);
  }

  { // Matrix
    HYPRE_StructMatrixCreate(m_comm, hypre_grid, hypre_stencil, &hypre_mat);
    HYPRE_StructMatrixInitialize(hypre_mat);

    // These indices must match to those in the offset array:
    HYPRE_Int inds[7] = {0, 1, 2, 3, 4, 5, 6};
    using RowType = Real[7];
    RowType * vals = new RowType[myN[0] * myN[1] * myN[2]];
    static constexpr Real COEF = 1;

    #pragma omp parallel for schedule(static)
    for (size_t k = 0; k < myN[2]; k++)
    for (size_t j = 0; j < myN[1]; j++)
    for (size_t i = 0; i < myN[0]; i++) {
      const auto idx = linaccess(i, j, k);
      assert(idx < (size_t) myN[0] * myN[1] * myN[2]);
      vals[idx][0] = -6*COEF; /* center */
      vals[idx][1] =  1*COEF; /* west   */ vals[idx][2] =  1*COEF; /* east   */
      vals[idx][3] =  1*COEF; /* south  */ vals[idx][4] =  1*COEF; /* north  */
      vals[idx][5] =  1*COEF; /* front  */ vals[idx][6] =  1*COEF; /* back   */
    }

    if(not bPeriodic) // 0 Neumann BC
    {
      #pragma omp parallel for collapse(3) schedule(dynamic, CUP_BLOCK_SIZE)
      for (size_t k = 0; k < myN[2]; k++)
      for (size_t j = 0; j < myN[1]; j++)
      for (size_t i = 0; i < myN[0]; i++)
      {
        const auto idx = linaccess(i, j, k);
        if(i==       0) { vals[idx][0] += COEF; vals[idx][1] = 0;}
        if(i==myN[0]-1) { vals[idx][0] += COEF; vals[idx][2] = 0;}
        if(j==       0) { vals[idx][0] += COEF; vals[idx][3] = 0;}
        if(j==myN[1]-1) { vals[idx][0] += COEF; vals[idx][4] = 0;}
        if(k==       0) { vals[idx][0] += COEF; vals[idx][5] = 0;}
        if(k==myN[2]-1) { vals[idx][0] += COEF; vals[idx][6] = 0;}
      }

      {
        static constexpr size_t SHIFT = 1;
        const size_t corner_idx = linaccess(myN[0]-1, myN[1]-1, myN[2]-1);
        const size_t corner_m1x = linaccess(myN[0]-1-SHIFT, myN[1]-1, myN[2]-1);
        const size_t corner_m1y = linaccess(myN[0]-1, myN[1]-1-SHIFT, myN[2]-1);
        const size_t corner_m1z = linaccess(myN[0]-1, myN[1]-1, myN[2]-1-SHIFT);
        // set last corner such that last point has pressure pLast
        vals[corner_idx][0] = 1;
        vals[corner_idx][1] = 0; vals[corner_idx][2] = 0;
        vals[corner_idx][3] = 0; vals[corner_idx][4] = 0;
        vals[corner_idx][5] = 0; vals[corner_idx][6] = 0;
        // neighbours read value of corner from the RHS:
        vals[corner_m1z][6]=0; vals[corner_m1y][4]=0; vals[corner_m1x][2]=0;
      }
    }
    // TODO fix last dof for periodic BC

    Real * const linV = (Real*) vals;
    HYPRE_StructMatrixSetBoxValues(hypre_mat, ilower, iupper, 7, inds, linV);
    delete [] vals;
    HYPRE_StructMatrixAssemble(hypre_mat);
  }


  // Rhs and initial guess
  HYPRE_StructVectorCreate(m_comm, hypre_grid, &hypre_rhs);
  HYPRE_StructVectorCreate(m_comm, hypre_grid, &hypre_sol);

  HYPRE_StructVectorInitialize(hypre_rhs);
  HYPRE_StructVectorInitialize(hypre_sol);

  {
    memset(data, 0, myN[0] * myN[1] * myN[2] * sizeof(Real));
    HYPRE_StructVectorSetBoxValues(hypre_rhs, ilower, iupper, data);
    HYPRE_StructVectorSetBoxValues(hypre_sol, ilower, iupper, data);
  }

  HYPRE_StructVectorAssemble(hypre_rhs);
  HYPRE_StructVectorAssemble(hypre_sol);

  if (solver == "gmres") {
    printf("Using GMRES solver\n");
    HYPRE_StructGMRESCreate(m_comm, &hypre_solver);
    HYPRE_StructGMRESSetTol(hypre_solver, 1e-2);
    HYPRE_StructGMRESSetPrintLevel(hypre_solver, 2);
    HYPRE_StructGMRESSetMaxIter(hypre_solver, 1000);
    HYPRE_StructGMRESSetup(hypre_solver, hypre_mat, hypre_rhs, hypre_sol);
  }
  else if (solver == "smg") {
    printf("Using SMG solver\n");
    HYPRE_StructSMGCreate(m_comm, &hypre_solver);
    //HYPRE_StructSMGSetMemoryUse(hypre_solver, 0);
    HYPRE_StructSMGSetMaxIter(hypre_solver, 100);
    HYPRE_StructSMGSetTol(hypre_solver, 1e-3);
    //HYPRE_StructSMGSetRelChange(hypre_solver, 0);
    HYPRE_StructSMGSetPrintLevel(hypre_solver, 3);
    HYPRE_StructSMGSetNumPreRelax(hypre_solver, 1);
    HYPRE_StructSMGSetNumPostRelax(hypre_solver, 1);

    HYPRE_StructSMGSetup(hypre_solver, hypre_mat, hypre_rhs, hypre_sol);
  }
  else {
    printf("Using PCG solver\n");
    HYPRE_StructPCGCreate(m_comm, &hypre_solver);
    HYPRE_StructPCGSetMaxIter(hypre_solver, 1000);
    HYPRE_StructPCGSetTol(hypre_solver, 1e-2);
    HYPRE_StructPCGSetPrintLevel(hypre_solver, 2);
    if(0)
    { // Use SMG preconditioner
      HYPRE_StructSMGCreate(m_comm, &hypre_precond);
      HYPRE_StructSMGSetMaxIter(hypre_precond, 1000);
      HYPRE_StructSMGSetTol(hypre_precond, 0);
      HYPRE_StructSMGSetNumPreRelax(hypre_precond, 1);
      HYPRE_StructSMGSetNumPostRelax(hypre_precond, 1);
      HYPRE_StructPCGSetPrecond(hypre_solver, HYPRE_StructSMGSolve,
                                HYPRE_StructSMGSetup, hypre_precond);
    }
    HYPRE_StructPCGSetup(hypre_solver, hypre_mat, hypre_rhs, hypre_sol);
  }
}

PoissonSolverMixed_HYPRE::~PoissonSolverMixed_HYPRE()
{
  if (solver == "gmres")
    HYPRE_StructGMRESDestroy(hypre_solver);
  else if (solver == "smg")
    HYPRE_StructSMGDestroy(hypre_solver);
  else
    HYPRE_StructPCGDestroy(hypre_solver);
  HYPRE_StructGridDestroy(hypre_grid);
  HYPRE_StructStencilDestroy(hypre_stencil);
  HYPRE_StructMatrixDestroy(hypre_mat);
  HYPRE_StructVectorDestroy(hypre_rhs);
  HYPRE_StructVectorDestroy(hypre_sol);
  delete [] data;
}

#endif