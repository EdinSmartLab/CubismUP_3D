//
//  TestGeometry.cpp
//  CubismUP_3D
//
//  Created by Christian Conti on 12/4/15.
//  Copyright © 2015 ETHZ. All rights reserved.
//

#include "TestGeometry.h"
#include "ProcessOperatorsOMP.h"
#include "CoordinatorComputeShape.h"
#include "CoordinatorBodyVelocities.h"
#include <sstream>
#include <cmath>

void TestGeometry::_ic()
{
	const Real center[3] = {.5,.5,.5};
	const Real rhoS = 300;
	const Real moll = 2;
	const int gridsize = 1024;
	const Real scale = .2;
	const Real tx = .1;
	const Real ty = .3;
	const Real tz = .25;
	Geometry::Quaternion q1(cos(.5*M_PI), 0, 0, sin(.5*M_PI));
	Geometry::Quaternion q2(cos(45./360.*M_PI), sin(45./360.*M_PI), 0, 0);
	Geometry::Quaternion q = q1*q2;
	
	double qm = q.magnitude();
	q.w /= qm;
	q.x /= qm;
	q.y /= qm;
	q.z /= qm;
	const string filename = "/cluster/home/infk/cconti/CubismUP_3D/launch/geometries/Samara_v3.obj";
	shape = new GeometryMesh(filename, gridsize, center, rhoS, moll, moll, scale, tx, ty, tz, q);
	
	vector<BlockInfo> vInfo = grid->getBlocksInfo();
	const double dh = vInfo[0].h_gridpoint;
	
	//#pragma omp parallel for
	for(int i=0; i<(int)vInfo.size(); i++)
	{
		BlockInfo info = vInfo[i];
		FluidBlock& b = *(FluidBlock*)info.ptrBlock;
		
		for(int iz=0; iz<FluidBlock::sizeZ; iz++)
			for(int iy=0; iy<FluidBlock::sizeY; iy++)
				for(int ix=0; ix<FluidBlock::sizeX; ix++)
				{
					Real p[3];
					info.pos(p, ix, iy, iz);
					
					const Real dist[3] = {p[0]-center[0],p[1]-center[1],p[2]-center[2]};
					
					b(ix,iy,iz).u = 0;
					b(ix,iy,iz).v = 0;
					b(ix,iy,iz).w = 0;
					
					b(ix,iy,iz).chi = shape->chi(p, info.h_gridpoint);
					b(ix,iy,iz).rho = shape->rho(p, info.h_gridpoint,b(ix,iy,iz).chi);
					
					b(ix,iy,iz).p = 0;
					b(ix,iy,iz).divU = 0;
					b(ix,iy,iz).pOld = 0;
				}
	}
	
	
	stringstream ss;
	ss << path2file << "-IC.vti";
	dumper.Write(*grid, ss.str());
}

TestGeometry::TestGeometry(const int argc, const char ** argv, const int bpd) : Test(argc,argv), bpd(bpd)
{
	grid = new FluidGrid(bpd,bpd,bpd);
	
	path2file = parser("-file").asString("../data/testGeometry");
	_ic();
}

TestGeometry::~TestGeometry()
{
	delete grid;
}

void TestGeometry::run()
{
}

void TestGeometry::check()
{
}