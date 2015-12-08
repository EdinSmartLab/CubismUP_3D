//
//  ProcessOperatorsOMP.h
//  CubismUP_3D
//
//  Created by Christian Conti on 1/8/15.
//  Copyright (c) 2015 ETHZ. All rights reserved.
//

#ifndef CubismUP_3D_ProcessOperators_h
#define CubismUP_3D_ProcessOperators_h

#include "Definitions.h"
#include "Shape.h"

template<typename Lab>
double findMaxAOMP(vector<BlockInfo>& myInfo, FluidGrid & grid)
{
	double maxA = 0;
	
	BlockInfo * ary = &myInfo.front();
	const int N = myInfo.size();
	
	const int stencil_start[3] = {-1,-1,-1};
	const int stencil_end[3]   = { 2, 2, 2};
	
#pragma omp parallel
	{
		Lab lab;
		lab.prepare(grid, stencil_start, stencil_end, true);
		
#pragma omp for schedule(static) reduction(max:maxA)
		for (int i=0; i<N; i++)
		{
			lab.load(ary[i], 0);
			
			BlockInfo info = myInfo[i];
			FluidBlock& b = *(FluidBlock*)info.ptrBlock;
			
			const double inv2h = info.h_gridpoint;
			
			for(int iz=0; iz<FluidBlock::sizeZ; ++iz)
			for(int iy=0; iy<FluidBlock::sizeY; ++iy)
				for(int ix=0; ix<FluidBlock::sizeX; ++ix)
				{
					double dudx = (lab(ix+1,iy  ,iz  ).u-lab(ix-1,iy  ,iz  ).u) * inv2h;
					double dudy = (lab(ix  ,iy+1,iz  ).u-lab(ix  ,iy-1,iz  ).u) * inv2h;
					double dudz = (lab(ix  ,iy  ,iz+1).u-lab(ix  ,iy  ,iz-1).u) * inv2h;
					
					double dvdx = (lab(ix+1,iy  ,iz  ).v-lab(ix-1,iy  ,iz  ).v) * inv2h;
					double dvdy = (lab(ix  ,iy+1,iz  ).v-lab(ix  ,iy-1,iz  ).v) * inv2h;
					double dvdz = (lab(ix  ,iy  ,iz+1).v-lab(ix  ,iy  ,iz-1).v) * inv2h;
					
					double dwdx = (lab(ix+1,iy  ,iz  ).w-lab(ix-1,iy  ,iz  ).w) * inv2h;
					double dwdy = (lab(ix  ,iy+1,iz  ).w-lab(ix  ,iy-1,iz  ).w) * inv2h;
					double dwdz = (lab(ix  ,iy  ,iz+1).w-lab(ix  ,iy  ,iz-1).w) * inv2h;
					
					maxA = max(max(max(dudx,dudy),
							       max(dvdx,dvdy)),
							   max(max(dudz,dvdz),
								   max(max(dwdx,dwdy),dwdz)));
				}
		}
	}
	
	return maxA;
}

// -gradp, divergence, advection
template<typename Lab, typename Kernel>
void processOMP(double dt, vector<BlockInfo>& myInfo, FluidGrid & grid)
{
	BlockInfo * ary = &myInfo.front();
	const int N = myInfo.size();
	
#pragma omp parallel
	{
		Kernel kernel(dt);
		
		Lab mylab;
		mylab.prepare(grid, kernel.stencil_start, kernel.stencil_end, true);
		
#pragma omp for schedule(static)
		for (int i=0; i<N; i++)
		{
			mylab.load(ary[i], 0);
			
			kernel(mylab, ary[i], *(FluidBlock*)ary[i].ptrBlock);
		}
	}
}

// divergence with layer - still useful for diagnostics
template<typename Lab, typename Kernel>
void processOMP(Layer& outputField, vector<BlockInfo>& myInfo, FluidGrid & grid)
{
	BlockInfo * ary = &myInfo.front();
	const int N = myInfo.size();
	
#pragma omp parallel
	{
		Kernel kernel(outputField);
		
		Lab mylab;
		mylab.prepare(grid, kernel.stencil_start, kernel.stencil_end, true);
		
#pragma omp for schedule(static)
		for (int i=0; i<N; i++)
		{
			mylab.load(ary[i], 0, false);
			
			kernel(mylab, ary[i], *(FluidBlock*)ary[i].ptrBlock);
		}
	}
}

// divergence split with layer - still useful for diagnostics
template<typename Lab, typename Kernel>
void processOMP(Layer& outputField, const Real rho0, const Real dt, const int step, vector<BlockInfo>& myInfo, FluidGrid & grid)
{
	BlockInfo * ary = &myInfo.front();
	const int N = myInfo.size();
	
#pragma omp parallel
	{
		Kernel kernel(outputField, rho0, dt, step);
		
		Lab mylab;
		mylab.prepare(grid, kernel.stencil_start, kernel.stencil_end, true);
		
#pragma omp for schedule(static)
		for (int i=0; i<N; i++)
		{
			mylab.load(ary[i], 0, false);
			
			kernel(mylab, ary[i], *(FluidBlock*)ary[i].ptrBlock);
		}
	}
}

double findMaxUOMP(vector<BlockInfo>& myInfo, FluidGrid & grid);
#endif
