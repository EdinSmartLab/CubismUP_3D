//
//  Cubism3D
//  Copyright (c) 2018 CSE-Lab, ETH Zurich, Switzerland.
//  Distributed under the terms of the MIT license.
//
//  Created by Guido Novati (novatig@ethz.ch).
//

#include "IF3D_ObstacleOperator.h"

#include "../operators/GenericOperator.h"
#include "../utils/BufferedLogger.h"

#include "Cubism/ArgumentParser.h"

using std::min;
using std::max;
using UDEFMAT = Real[CUP_BLOCK_SIZE][CUP_BLOCK_SIZE][CUP_BLOCK_SIZE][3];
using CHIMAT = Real[CUP_BLOCK_SIZE][CUP_BLOCK_SIZE][CUP_BLOCK_SIZE];
static constexpr Real EPS = std::numeric_limits<Real>::epsilon();
static constexpr double DBLEPS = std::numeric_limits<double>::epsilon();

ObstacleArguments::ObstacleArguments(
        const SimulationData & sim,
        ArgumentParser &parser)
{
  length = parser("-L").asDouble();          // Mandatory.
  position[0] = parser("-xpos").asDouble();  // Mandatory.
  position[1] = parser("-ypos").asDouble(sim.extent[1] / 2);
  position[2] = parser("-zpos").asDouble(sim.extent[2] / 2);
  quaternion[0] = parser("-quat0").asDouble(1.0);
  quaternion[1] = parser("-quat1").asDouble(0.0);
  quaternion[2] = parser("-quat2").asDouble(0.0);
  quaternion[3] = parser("-quat3").asDouble(0.0);

  // if true, obstacle will never change its velocity:
  // bForcedInLabFrame = parser("-bForcedInLabFrame").asBool(false);
  bool bFSM_alldir = parser("-bForcedInSimFrame").asBool(false);
  bForcedInSimFrame[0] = bFSM_alldir || parser("-bForcedInSimFrame_x").asBool(false);
  bForcedInSimFrame[1] = bFSM_alldir || parser("-bForcedInSimFrame_y").asBool(false);
  bForcedInSimFrame[2] = bFSM_alldir || parser("-bForcedInSimFrame_z").asBool(false);

  // only active if corresponding bForcedInLabFrame is true:
  enforcedVelocity[0] = -parser("-xvel").asDouble(0.0);
  enforcedVelocity[1] = -parser("-yvel").asDouble(0.0);
  enforcedVelocity[2] = -parser("-zvel").asDouble(0.0);

  bFixToPlanar = parser("-bFixToPlanar").asBool(false);

  // this is different, obstacle can change the velocity, but sim frame will follow:
  bool bFOR_alldir = parser("-bFixFrameOfRef").asBool(false);
  bFixFrameOfRef[0] = bFOR_alldir || parser("-bFixFrameOfRef_x").asBool(false);
  bFixFrameOfRef[1] = bFOR_alldir || parser("-bFixFrameOfRef_y").asBool(false);
  bFixFrameOfRef[2] = bFOR_alldir || parser("-bFixFrameOfRef_z").asBool(false);

  // To force forced obst. into computeForces or to force self-propelled
  // into diagnostics forces (tasso del tasso del tasso):
  // If untouched forced only do diagnostics and selfprop only do surface.
  bComputeForces = parser("-computeForces").asBool(false);
}

IF3D_ObstacleOperator::IF3D_ObstacleOperator(SimulationData&s, ArgumentParser&p)
    : IF3D_ObstacleOperator( s, ObstacleArguments(s, p) ) { }

IF3D_ObstacleOperator::IF3D_ObstacleOperator(
    SimulationData& s, const ObstacleArguments &args)
    : IF3D_ObstacleOperator(s)
{
  length = args.length;
  position[0] = args.position[0];
  position[1] = args.position[1];
  position[2] = args.position[2];
  quaternion[0] = args.quaternion[0];
  quaternion[1] = args.quaternion[1];
  quaternion[2] = args.quaternion[2];
  quaternion[3] = args.quaternion[3];
  _2Dangle = 2 * std::atan2(quaternion[3], quaternion[0]);

  if (!sim.rank) {
    printf("Obstacle L=%g, pos=[%g %g %g], q=[%g %g %g %g]\n",
           length, position[0], position[1], position[2],
           quaternion[0], quaternion[1], quaternion[2], quaternion[3]);
  }

  const double one = std::sqrt(
          quaternion[0] * quaternion[0]
        + quaternion[1] * quaternion[1]
        + quaternion[2] * quaternion[2]
        + quaternion[3] * quaternion[3]);

  if (std::fabs(one - 1.0) > 5 * DBLEPS) {
    printf("Parsed quaternion length is not equal to one. It really ought to be.\n");
    fflush(0);
    abort();
  }
  if (length < 5 * EPS) {
    printf("Parsed length is equal to zero. It really ought not to be.\n");
    fflush(0);
    abort();
  }

  for (int d = 0; d < 3; ++d) {
    bForcedInSimFrame[d] = args.bForcedInSimFrame[d];
    if (bForcedInSimFrame[d]) {
      transVel_imposed[d] = transVel[d] = args.enforcedVelocity[d];
      if (!sim.rank) {
         printf("Obstacle forced to move relative to sim domain with constant %c-vel: %f\n",
                "xyz"[d], transVel[d]);
      }
    }
  }
  bFixToPlanar = args.bFixToPlanar;

  const bool anyVelForced = bForcedInSimFrame[0] || bForcedInSimFrame[1] || bForcedInSimFrame[2];
  if(anyVelForced) {
    if (!sim.rank) printf("Obstacle has no angular velocity.\n");
    bBlockRotation[0] = true;
    bBlockRotation[1] = true;
    bBlockRotation[2] = true;
  }

  bFixFrameOfRef[0] = args.bFixFrameOfRef[0];
  bFixFrameOfRef[1] = args.bFixFrameOfRef[1];
  bFixFrameOfRef[2] = args.bFixFrameOfRef[2];
  bForces = args.bComputeForces;
}

void IF3D_ObstacleOperator::_computeUdefMoments(double lin_momenta[3],
  double ang_momenta[3], const double CoM[3])
{
  const double oldCorrVel[3] = {
    transVel_correction[0], transVel_correction[1], transVel_correction[2]
  };
  const double h = vInfo[0].h_gridpoint;

  double V=0, FX=0, FY=0, FZ=0, TX=0, TY=0, TZ=0;
  double J0=0, J1=0, J2=0, J3=0, J4=0, J5=0;
  #pragma omp parallel for schedule(dynamic) \
  reduction(+ : V, FX, FY, FZ, TX, TY, TZ, J0, J1, J2, J3, J4, J5)
  for(size_t i=0; i<vInfo.size(); i++)
  {
    const BlockInfo info = vInfo[i];
    const auto pos = obstacleBlocks.find(info.blockID);
    if(pos == obstacleBlocks.end()) continue;
    CHIMAT & __restrict__ CHI = pos->second->chi;
    UDEFMAT & __restrict__ UDEF = pos->second->udef;
    for(int iz=0; iz<FluidBlock::sizeZ; ++iz)
    for(int iy=0; iy<FluidBlock::sizeY; ++iy)
    for(int ix=0; ix<FluidBlock::sizeX; ++ix)
    {
      if (CHI[iz][iy][ix] <= 0) continue;
      double p[3]; info.pos(p, ix, iy, iz);
      const double X = CHI[iz][iy][ix];
      const double dUs = UDEF[iz][iy][ix][0] - oldCorrVel[0];
      const double dVs = UDEF[iz][iy][ix][1] - oldCorrVel[1];
      const double dWs = UDEF[iz][iy][ix][2] - oldCorrVel[2];
      p[0] -= CoM[0]; p[1] -= CoM[1]; p[2] -= CoM[2];
      V  += X * h*h;
      FX += X * UDEF[iz][iy][ix][0] * h*h;
      FY += X * UDEF[iz][iy][ix][1] * h*h;
      FZ += X * UDEF[iz][iy][ix][2] * h*h;
      TX += X * ( p[1]*dWs - p[2]*dVs ) * h;
      TY += X * ( p[2]*dUs - p[0]*dWs ) * h;
      TZ += X * ( p[0]*dVs - p[1]*dUs ) * h;
      J0 += X * ( p[1]*p[1]+p[2]*p[2] ) * h; J3 -= X * p[0]*p[1] * h;
      J1 += X * ( p[0]*p[0]+p[2]*p[2] ) * h; J4 -= X * p[0]*p[2] * h;
      J2 += X * ( p[0]*p[0]+p[1]*p[1] ) * h; J5 -= X * p[1]*p[2] * h;
    }
  }

  double globals[13] = {0,0,0,0,0,0,0,0,0,0,0,0,0};
  double locals[13] = {FX,FY,FZ,V,TX,TY,TZ,J0,J1,J2,J3,J4,J5};
  MPI_Allreduce(locals, globals, 13, MPI_DOUBLE,MPI_SUM,grid->getCartComm());
  assert(globals[3] > DBLEPS);

  lin_momenta[0] = globals[0]/globals[3];
  lin_momenta[1] = globals[1]/globals[3];
  lin_momenta[2] = globals[2]/globals[3];
  volume         = globals[3] * h;
  J[0] = globals[ 7] *h*h; J[1] = globals[ 8] *h*h; J[2] = globals[ 9] *h*h;
  J[3] = globals[10] *h*h; J[4] = globals[11] *h*h; J[5] = globals[12] *h*h;
  //if(bFixToPlanar) {
  //  ang_momenta[0] = ang_momenta[1] = 0.0;
  //  ang_momenta[2] = globals[2]/globals[5]; // av2/j2
  //} else
  {
    //solve avel = invJ \dot angMomentum, do not multiply by h^3, but by h for numerics
    const double AM[3] = {globals[ 4], globals[ 5], globals[ 6]};
    const double J_[6] = {globals[ 7], globals[ 8], globals[ 9],
                          globals[10], globals[11], globals[12]};
    const double detJ = J_[0]*(J_[1]*J_[2] - J_[5]*J_[5])+
                        J_[3]*(J_[4]*J_[5] - J_[2]*J_[3])+
                        J_[4]*(J_[3]*J_[5] - J_[1]*J_[4]);
    assert(std::fabs(detJ)>DBLEPS);
    const double invJ[6] = {
      (J_[1]*J_[2] - J_[5]*J_[5]) / detJ,
      (J_[0]*J_[2] - J_[4]*J_[4]) / detJ,
      (J_[0]*J_[1] - J_[3]*J_[3]) / detJ,
      (J_[4]*J_[5] - J_[2]*J_[3]) / detJ,
      (J_[3]*J_[5] - J_[1]*J_[4]) / detJ,
      (J_[3]*J_[4] - J_[0]*J_[5]) / detJ
    };

    ang_momenta[0] = invJ[0]*AM[0] + invJ[3]*AM[1] + invJ[4]*AM[2];
    ang_momenta[1] = invJ[3]*AM[0] + invJ[1]*AM[1] + invJ[5]*AM[2];
    ang_momenta[2] = invJ[4]*AM[0] + invJ[5]*AM[1] + invJ[2]*AM[2];
  }
}

void IF3D_ObstacleOperator::_makeDefVelocitiesMomentumFree(const double CoM[3])
{
  _computeUdefMoments(transVel_correction, angVel_correction, CoM);
  #ifdef CUP_VERBOSE
   if(sim.rank==0)
      printf("Correction of: lin mom [%f %f %f] ang mom [%f %f %f]\n",
        transVel_correction[0], transVel_correction[1], transVel_correction[2],
     angVel_correction[0], angVel_correction[1], angVel_correction[2]);
  #endif

  #pragma omp parallel for schedule(dynamic)
  for(size_t i=0; i<vInfo.size(); i++)
  {
    const BlockInfo info = vInfo[i];
    const auto pos = obstacleBlocks.find(info.blockID);
    if(pos == obstacleBlocks.end()) continue;
    UDEFMAT & __restrict__ UDEF = pos->second->udef;
    for(int iz=0; iz<FluidBlock::sizeZ; ++iz)
    for(int iy=0; iy<FluidBlock::sizeY; ++iy)
    for(int ix=0; ix<FluidBlock::sizeX; ++ix) {
      double p[3]; info.pos(p, ix, iy, iz);
      p[0] -= CoM[0]; p[1] -= CoM[1]; p[2] -= CoM[2];
      const double rotVel_correction[3] = {
        angVel_correction[1]*p[2] - angVel_correction[2]*p[1],
        angVel_correction[2]*p[0] - angVel_correction[0]*p[2],
        angVel_correction[0]*p[1] - angVel_correction[1]*p[0]
      };
      UDEF[iz][iy][ix][0] -= transVel_correction[0] + rotVel_correction[0];
      UDEF[iz][iy][ix][1] -= transVel_correction[1] + rotVel_correction[1];
      UDEF[iz][iy][ix][2] -= transVel_correction[2] + rotVel_correction[2];
    }
  }
  #ifndef NDEBUG
    double dummy_ang[3], dummy_lin[3];
    _computeUdefMoments(dummy_lin, dummy_ang, CoM);
    assert(std::fabs(dummy_lin[0])<10*EPS && std::fabs(dummy_ang[0])<10*EPS);
    assert(std::fabs(dummy_lin[1])<10*EPS && std::fabs(dummy_ang[1])<10*EPS);
    assert(std::fabs(dummy_lin[2])<10*EPS && std::fabs(dummy_ang[2])<10*EPS);
  #endif
}

void IF3D_ObstacleOperator::_writeComputedVelToFile(const int step_id, const double t)
{
  if(sim.rank!=0) return;
  std::stringstream ssR;
  ssR<<"computedVelocity_"<<obstacleID<<".dat";
  std::stringstream &savestream = logger.get_stream(ssR.str());
  const std::string tab("\t");

  if(step_id==0 && not printedHeaderVels) {
    printedHeaderVels = true;
    savestream<<"step"<<tab<<"time"<<tab<<"CMx"<<tab<<"CMy"<<tab<<"CMz"<<tab
    <<"quat_0"<<tab<<"quat_1"<<tab<<"quat_2"<<tab<<"quat_3"<<tab
    <<"vel_x"<<tab<<"vel_y"<<tab<<"vel_z"<<tab
    <<"angvel_x"<<tab<<"angvel_y"<<tab<<"angvel_z"<<tab<<"volume"<<tab
    <<"J0"<<tab<<"J1"<<tab<<"J2"<<tab<<"J3"<<tab<<"J4"<<tab<<"J5"<<std::endl;
  }

  savestream<<step_id<<tab;
  savestream.setf(std::ios::scientific);
  savestream.precision(std::numeric_limits<float>::digits10 + 1);
  savestream <<t<<tab<<position[0]<<tab<<position[1]<<tab<<position[2]<<tab
    <<quaternion[0]<<tab<<quaternion[1]<<tab<<quaternion[2]<<tab<<quaternion[3]
    <<tab<<transVel[0]<<tab<<transVel[1]<<tab<<transVel[2]
    <<tab<<angVel[0]<<tab<<angVel[1]<<tab<<angVel[2]<<tab<<volume<<tab
    <<J[0]<<tab<<J[1]<<tab<<J[2]<<tab<<J[3]<<tab<<J[4]<<tab<<J[5]<<std::endl;
}

void IF3D_ObstacleOperator::_writeSurfForcesToFile(const int step_id, const double t)
{
  if(sim.rank!=0) return;
  std::stringstream fnameF, fnameP;
  fnameF<<"forceValues_"<<(!isSelfPropelled?"surface_":"")<<obstacleID<<".dat";
  std::stringstream &ssF = logger.get_stream(fnameF.str());
  const std::string tab("\t");
  if(step_id==0) {
    ssF<<"step"<<tab<<"time"<<tab<<"mass"<<tab<<"force_x"<<tab<<"force_y"
    <<tab<<"force_z"<<tab<<"torque_x"<<tab<<"torque_y"<<tab<<"torque_z"
    <<tab<<"presF_x"<<tab<<"presF_y"<<tab<<"presF_z"<<tab<<"viscF_x"
    <<tab<<"viscF_y"<<tab<<"viscF_z"<<tab<<"gamma_x"<<tab<<"gamma_y"
    <<tab<<"gamma_z"<<tab<<"drag"<<tab<<"thrust"<<std::endl;
  }

  ssF << step_id << tab;
  ssF.setf(std::ios::scientific);
  ssF.precision(std::numeric_limits<float>::digits10 + 1);
  ssF<<t<<tab<<volume<<tab<<surfForce[0]<<tab<<surfForce[1]<<tab<<surfForce[2]
     <<tab<<surfTorque[0]<<tab<<surfTorque[1]<<tab<<surfTorque[2]<<tab
     <<presForce[0]<<tab<<presForce[1]<<tab<<presForce[2]<<tab<<viscForce[0]
     <<tab<<viscForce[1]<<tab<<viscForce[2]<<tab<<gamma[0]<<tab<<gamma[1]
     <<tab<<gamma[2]<<tab<<drag<<tab<<thrust<<std::endl;

  fnameP<<"powerValues_"<<(!isSelfPropelled?"surface_":"")<<obstacleID<<".dat";
  std::stringstream &ssP = logger.get_stream(fnameP.str());
  if(step_id==0) {
    ssP<<"step"<<tab<<"time"<<tab<<"Pthrust"<<tab<<"Pdrag"<<tab
       <<"Pout"<<tab<<"pDef"<<tab<<"etaPDef"<<tab<<"pLocom"<<tab
       <<"PoutBnd"<<tab<<"defPowerBnd"<<tab<<"etaPDefBnd"<<std::endl;
  }
  ssP << step_id << tab;
  ssP.setf(std::ios::scientific);
  ssP.precision(std::numeric_limits<float>::digits10 + 1);
  // Output defpowers to text file with the correct sign
  ssP<<t<<tab<<Pthrust<<tab<<Pdrag<<tab<<Pout<<tab<<-defPower<<tab<<EffPDef
     <<tab<<pLocom<<tab<<PoutBnd<<tab<<-defPowerBnd<<tab<<EffPDefBnd<<std::endl;
}

void IF3D_ObstacleOperator::_writeDiagForcesToFile(const int step_id, const double t)
{
  if(sim.rank!=0) return;
  std::stringstream fnameF;
  fnameF<<"forceValues_"<<(isSelfPropelled?"penalization_":"")<<obstacleID<<".dat";
  std::stringstream &ssF = logger.get_stream(fnameF.str());
  const std::string tab("\t");
  if(step_id==0) {
    ssF << "step" << tab << "time" << tab << "mass" << tab
     << "force_x" << tab << "force_y" << tab << "force_z" << tab
     << "torque_x" << tab << "torque_y" << tab << "torque_z" << std::endl;
  }

  ssF << step_id << tab;
  ssF.setf(std::ios::scientific);
  ssF.precision(std::numeric_limits<float>::digits10 + 1);
  ssF<<t<<tab<<mass<<tab<<force[0]<<tab<<force[1]<<tab<<force[2]<<tab
     <<torque[0]<<tab<<torque[1]<<tab<<torque[2]<<std::endl;
}

void IF3D_ObstacleOperator::computeVelocities(const double dt,const Real lambda)
{
  double CM[3];
  this->getCenterOfMass(CM);
  const double h  = vInfo[0].h_gridpoint;
  double globals[13] = {0,0,0,0,0,0,0,0,0,0,0,0,0};

  {
    double V=0, FX=0, FY=0, FZ=0, TX=0, TY=0, TZ=0;
    double J0=0, J1=0, J2=0, J3=0, J4=0, J5=0;
    #pragma omp parallel for schedule(dynamic) \
    reduction(+ : V, FX, FY, FZ, TX, TY, TZ, J0, J1, J2, J3, J4, J5)
    for(size_t i=0; i<vInfo.size(); i++)
    {
      const BlockInfo info = vInfo[i];
      const FluidBlock & b = *(FluidBlock*)info.ptrBlock;
      const auto pos = obstacleBlocks.find(info.blockID);
      if(pos == obstacleBlocks.end()) continue;
      CHIMAT & __restrict__ CHI = pos->second->chi;
      UDEFMAT & __restrict__ UDEF = pos->second->udef;

      for(int iz=0; iz<FluidBlock::sizeZ; ++iz)
      for(int iy=0; iy<FluidBlock::sizeY; ++iy)
      for(int ix=0; ix<FluidBlock::sizeX; ++ix)
      {
        if (CHI[iz][iy][ix] <= 0) continue;
        double p[3]; info.pos(p, ix, iy, iz);
        const double X = CHI[iz][iy][ix];
        p[0] -= CM[0]; p[1] -= CM[1]; p[2] -= CM[2];
        const double object_UR[3] = {
            angVel[1] * p[2] - angVel[2] * p[1],
            angVel[2] * p[0] - angVel[0] * p[2],
            angVel[0] * p[1] - angVel[1] * p[0]
        };
        const double UDIFF[3] = {
            b(ix,iy,iz).u - transVel[0] - object_UR[0] - UDEF[iz][iy][ix][0],
            b(ix,iy,iz).v - transVel[1] - object_UR[1] - UDEF[iz][iy][ix][1],
            b(ix,iy,iz).w - transVel[2] - object_UR[2] - UDEF[iz][iy][ix][2]
        };
        // the gridsize h is included here for numeric safety
        V  += X * h*h;
        FX += X * UDIFF[0] * h*h;
        FY += X * UDIFF[1] * h*h;
        FZ += X * UDIFF[2] * h*h;
        TX += X * ( p[1] * UDIFF[2] - p[2] * UDIFF[1] ) * h;
        TY += X * ( p[2] * UDIFF[0] - p[0] * UDIFF[2] ) * h;
        TZ += X * ( p[0] * UDIFF[1] - p[1] * UDIFF[0] ) * h;
        J0 += X * ( p[1]*p[1] + p[2]*p[2] ) * h; J3 -= X * p[0]*p[1] * h;
        J1 += X * ( p[0]*p[0] + p[2]*p[2] ) * h; J4 -= X * p[0]*p[2] * h;
        J2 += X * ( p[0]*p[0] + p[1]*p[1] ) * h; J5 -= X * p[1]*p[2] * h;
      }
    }

    double locals[13] = {FX,FY,FZ,V,TX,TY,TZ,J0,J1,J2,J3,J4,J5};
    MPI_Allreduce(locals, globals, 13, MPI_DOUBLE,MPI_SUM,grid->getCartComm());
    assert(globals[3] > DBLEPS);
  }

  const double V = globals[3], FX = globals[0]*lambda, FY = globals[1]*lambda;
  const double FZ = globals[2]*lambda, TX = globals[4]*lambda;
  const double TY = globals[5]*lambda, TZ = globals[6]*lambda;
  const double J0 = globals[ 7], J1 = globals[ 8], J2 = globals[ 9];
  const double J3 = globals[10], J4 = globals[11], J5 = globals[12];
  force[0] = FX * h; force[1] = FY * h; force[2] = FZ * h;
  mass = V * h; volume = V * h;
  torque[0] = TX * std::pow(h,2);
  torque[1] = TY * std::pow(h,2);
  torque[2] = TZ * std::pow(h,2);
  J[0] = J0 * std::pow(h,2); J[1] = J1 * std::pow(h,2);
  J[2] = J2 * std::pow(h,2); J[3] = J3 * std::pow(h,2);
  J[4] = J4 * std::pow(h,2); J[5] = J5 * std::pow(h,2);

  transVel_computed[0] = transVel[0] + dt*FX/V;
  transVel_computed[1] = transVel[1] + dt*FY/V;
  transVel_computed[2] = transVel[2] + dt*FZ/V;

  if(bForcedInSimFrame[0]) transVel[0] = transVel_imposed[0];
  else transVel[0] = transVel_computed[0];
  if(bForcedInSimFrame[1]) transVel[1] = transVel_imposed[1];
  else transVel[1] = transVel_computed[1];
  if(bForcedInSimFrame[2]) transVel[2] = transVel_imposed[2];
  else if (bFixToPlanar) transVel[2] = 0.0;
  else transVel[2] = transVel_computed[2];

  if(bFixToPlanar)
  {
    angVel[0] = angVel[1] = 0.0;
    angVel_computed[0] = angVel_computed[1] = 0.0;
    angVel_computed[2] = angVel[2] + dt * TZ / J2;
    if( not bBlockRotation[2] ) angVel[2] = angVel_computed[2];
    else angVel[2] = 0.0;
  }
  else
  {
    //solve avel = invJ \dot angMomentum, do not multiply by h^3 for numerics
    const double detJ = J0*(J1*J2-J5*J5) + J3*(J4*J5-J2*J3) + J4*(J3*J5-J1*J4);
    assert(std::fabs(detJ)>DBLEPS);
    const double invJ[6] = {
      (J1*J2-J5*J5)/detJ, (J0*J2-J4*J4)/detJ, (J0*J1-J3*J3)/detJ,
      (J4*J5-J2*J3)/detJ, (J3*J5-J1*J4)/detJ, (J3*J4-J0*J5)/detJ
    };
    angVel_computed[0] = angVel[0] + dt*(invJ[0]*TX + invJ[3]*TY + invJ[4]*TZ);
    angVel_computed[1] = angVel[1] + dt*(invJ[3]*TX + invJ[1]*TY + invJ[5]*TZ);
    angVel_computed[2] = angVel[2] + dt*(invJ[4]*TX + invJ[5]*TY + invJ[2]*TZ);
    if(not bBlockRotation[0]) angVel[0] = angVel_computed[0];
    if(not bBlockRotation[1]) angVel[1] = angVel_computed[1];
    if(not bBlockRotation[2]) angVel[2] = angVel_computed[2];
  }
}

void IF3D_ObstacleOperator::computeForces(const int stepID, const double time,
  const double dt, const double NU, const bool bDump)
{
  // Surface forces are very imprecise for non-self propelled obstacles
  // bForces forces computeForces. Makes sense, right?
  if(not bForces && not isSelfPropelled) return;

  double CM[3];
  this->getCenterOfMass(CM);

  double vel_unit[3] = {0., 0., 0.};
  const double vel_norm = std::sqrt(transVel[0]*transVel[0]
                                  + transVel[1]*transVel[1]
                                  + transVel[2]*transVel[2]);
  if (vel_norm>1e-9) {
      vel_unit[0] = transVel[0] / vel_norm;
      vel_unit[1] = transVel[1] / vel_norm;
      vel_unit[2] = transVel[2] / vel_norm;
  }

  const int nthreads = omp_get_max_threads();
  std::vector<OperatorComputeForces*> forces(nthreads, nullptr);
  for(int i=0; i<nthreads; ++i)
    forces[i] = new OperatorComputeForces(NU,dt,vel_unit,CM,angVel,transVel);

  compute<OperatorComputeForces, SURFACE>(forces);

  for(int i=0; i<nthreads; i++) delete forces[i];

  static const int nQoI = ObstacleBlock::nQoI;
  std::vector<double> sum = std::vector<double>(nQoI, 0);
  for (auto & block : obstacleBlocks) block.second->sumQoI(sum);

  MPI_Allreduce(MPI_IN_PLACE, sum.data(), nQoI, MPI_DOUBLE, MPI_SUM, grid->getCartComm());

  //additive quantities: (check against order in sumQoI of ObstacleBlocks.h )
  unsigned k = 0;
  surfForce[0]= sum[k++]; surfForce[1]= sum[k++]; surfForce[2]= sum[k++];
  presForce[0]= sum[k++]; presForce[1]= sum[k++]; presForce[2]= sum[k++];
  viscForce[0]= sum[k++]; viscForce[1]= sum[k++]; viscForce[2]= sum[k++];
  surfTorque[0]=sum[k++]; surfTorque[1]=sum[k++]; surfTorque[2]=sum[k++];
  gamma[0]    = sum[k++]; gamma[1]    = sum[k++]; gamma[2]    = sum[k++];
  drag        = sum[k++]; thrust      = sum[k++]; Pout        = sum[k++];
  PoutBnd     = sum[k++]; defPower    = sum[k++]; defPowerBnd = sum[k++];
  pLocom      = sum[k++];

  //derived quantities:
  Pthrust    = thrust*vel_norm;
  Pdrag      =   drag*vel_norm;
  EffPDef    = Pthrust/(Pthrust-min(defPower,(double)0)+EPS);
  EffPDefBnd = Pthrust/(Pthrust-    defPowerBnd        +EPS);

  #if defined(CUP_DUMP_SURFACE_BINARY) && !defined(RL_LAYER)
  if (bDump) {
    char buf[500];
    sprintf(buf, "surface_%02d_%07d_rank%03d.raw", obstacleID, stepID, sim.rank);
    FILE * pFile = fopen (buf, "wb");
    for(auto & block : obstacleBlocks) block.second->print(pFile);
    fflush(pFile);
    fclose(pFile);
  }
  #endif
  _writeSurfForcesToFile(stepID, time);
}

void IF3D_ObstacleOperator::update(const int step_id, const double t, const double dt, const Real* Uinf)
{
  position[0] += dt * ( transVel[0] + Uinf[0] );
  position[1] += dt * ( transVel[1] + Uinf[1] );
  position[2] += dt * ( transVel[2] + Uinf[2] );
  absPos[0] += dt * transVel[0];
  absPos[1] += dt * transVel[1];
  absPos[2] += dt * transVel[2];
  const double Q[] = {quaternion[0],quaternion[1],quaternion[2],quaternion[3]};
  const double dqdt[4] = {
    .5*( - angVel[0]*Q[1] - angVel[1]*Q[2] - angVel[2]*Q[3] ),
    .5*( + angVel[0]*Q[0] + angVel[1]*Q[3] - angVel[2]*Q[2] ),
    .5*( - angVel[0]*Q[3] + angVel[1]*Q[0] + angVel[2]*Q[1] ),
    .5*( + angVel[0]*Q[2] - angVel[1]*Q[1] + angVel[2]*Q[0] )
  };

  // normality preserving advection (Simulation of colliding constrained rigid bodies - Kleppmann 2007 Cambridge University, p51)
  // move the correct distance on the quaternion unit ball surface, end up with normalized quaternion
  const double DQ[4] = { dqdt[0]*dt, dqdt[1]*dt, dqdt[2]*dt, dqdt[3]*dt };
  const double DQn = std::sqrt(DQ[0]*DQ[0]+DQ[1]*DQ[1]+DQ[2]*DQ[2]+DQ[3]*DQ[3]);

  if(DQn>DBLEPS)
  {
    const double tanF = std::tan(DQn)/DQn;
    const double D[4] = {
      Q[0] +tanF*DQ[0], Q[1] +tanF*DQ[1], Q[2] +tanF*DQ[2], Q[3] +tanF*DQ[3],
    };
    const double invD = 1/std::sqrt(D[0]*D[0]+D[1]*D[1]+D[2]*D[2]+D[3]*D[3]);
    quaternion[0] = D[0] * invD; quaternion[1] = D[1] * invD;
    quaternion[2] = D[2] * invD; quaternion[3] = D[3] * invD;
  }

  //_2Dangle += dt*angVel[2];
  const double old2DA = _2Dangle;
  //keep consistency: get 2d angle from quaternions:
  _2Dangle = 2*std::atan2(quaternion[3], quaternion[0]);
  const double err = std::fabs(_2Dangle-old2DA-dt*angVel[2]);
  if(err>EPS && !sim.rank)
    printf("Discrepancy in angvel from quaternions: %f (%f %f)\n",
      err, (_2Dangle-old2DA)/dt, angVel[2]);

  #ifndef NDEBUG
  if(sim.rank==0)
  {
    #ifdef CUP_VERBOSE
     std::cout<<"POSITION INFO AFTER UPDATE T, DT: "<<t<<" "<<dt<<std::endl;
     std::cout<<"POS: "<<position[0]<<" "<<position[1]<<" "<<position[2]<<std::endl;
     std::cout<<"TVL: "<<transVel[0]<<" "<<transVel[1]<<" "<<transVel[2]<<std::endl;
     std::cout<<"QUT: "<<quaternion[0]<<" "<<quaternion[1]<<" "<<quaternion[2]<<" "<<quaternion[3]<<std::endl;
     std::cout<<"AVL: "<<angVel[0]<<" "<<angVel[1]<<" "<<angVel[2]<<std::endl;
    #endif
  }
  const double q_length=std::sqrt(quaternion[0]*quaternion[0]
        +  quaternion[1]*quaternion[1]
        +  quaternion[2]*quaternion[2]
        +  quaternion[3]*quaternion[3]);
  assert(std::abs(q_length-1.0) < 5*EPS);
  #endif

  _writeComputedVelToFile(step_id, t);
  _writeDiagForcesToFile(step_id, t);
}

void IF3D_ObstacleOperator::characteristic_function()
{
  #pragma omp parallel
 {
  #pragma omp for schedule(dynamic)
  for(size_t i=0; i<vInfo.size(); i++) {
   BlockInfo info = vInfo[i];
   std::map<int,ObstacleBlock* >::const_iterator pos = obstacleBlocks.find(info.blockID);

   if(pos != obstacleBlocks.end()) {
        FluidBlock& b = *(FluidBlock*)info.ptrBlock;
    for(int iz=0; iz<FluidBlock::sizeZ; iz++)
    for(int iy=0; iy<FluidBlock::sizeY; iy++)
    for(int ix=0; ix<FluidBlock::sizeX; ix++)
     b(ix,iy,iz).chi = std::max(pos->second->chi[iz][iy][ix], b(ix,iy,iz).chi);
   }
  }
 }
}

std::vector<int> IF3D_ObstacleOperator::intersectingBlockIDs(const int buffer) const
{
  assert(buffer <= 2); // only works for 2: if different definition of deformation blocks, implement your own
  std::vector<int> retval;
  for(size_t i=0; i<vInfo.size(); i++) {
    const auto pos = obstacleBlocks.find(vInfo[i].blockID);
    if(pos != obstacleBlocks.end()) retval.push_back(vInfo[i].blockID);
  }
  return retval;
}

void IF3D_ObstacleOperator::create(const int step_id,const double time,
    const double dt, const Real *Uinf)
{
  printf("Entered the wrong create operator\n");
  fflush(0);
  abort();
}

void IF3D_ObstacleOperator::computeChi(const int step_id, const double time,
  const double dt, const Real *Uinf, int& mpi_status) {}

void IF3D_ObstacleOperator::finalize(const int step_id,const double time,
  const double dt, const Real *Uinf)
{ }

void IF3D_ObstacleOperator::getTranslationVelocity(double UT[3]) const
{
  UT[0]=transVel[0];
  UT[1]=transVel[1];
  UT[2]=transVel[2];
}

void IF3D_ObstacleOperator::setTranslationVelocity(double UT[3])
{
  transVel[0] = UT[0];
  transVel[1] = UT[1];
  transVel[2] = UT[2];
}

void IF3D_ObstacleOperator::getAngularVelocity(double W[3]) const
{
  W[0]=angVel[0];
  W[1]=angVel[1];
  W[2]=angVel[2];
}

void IF3D_ObstacleOperator::setAngularVelocity(const double W[3])
{
  angVel[0]=W[0];
  angVel[1]=W[1];
  angVel[2]=W[2];
}

void IF3D_ObstacleOperator::getCenterOfMass(double CM[3]) const
{
  CM[0]=position[0];
  CM[1]=position[1];
  CM[2]=position[2];
}

void IF3D_ObstacleOperator::save(const int step_id, const double t, std::string filename)
{
  if(sim.rank!=0) return;
  #ifdef RL_LAYER
  sr.save(step_id,filename);
  #endif
  std::ofstream savestream;
  savestream.setf(std::ios::scientific);
  savestream.precision(std::numeric_limits<Real>::digits10 + 1);
  savestream.open(filename+".txt");
  savestream<<t<<std::endl;
  savestream<<position[0]<<"\t"<<position[1]<<"\t"<<position[2]<<std::endl;
  savestream<<absPos[0]<<"\t"<<absPos[1]<<"\t"<<absPos[2]<<std::endl;
  savestream<<quaternion[0]<<"\t"<<quaternion[1]<<"\t"<<quaternion[2]<<"\t"<<quaternion[3]<<std::endl;
  savestream<<transVel[0]<<"\t"<<transVel[1]<<"\t"<<transVel[2]<<std::endl;
  savestream<<angVel[0]<<"\t"<<angVel[1]<<"\t"<<angVel[2]<<std::endl;
  savestream<<_2Dangle<<std::endl;
}

void IF3D_ObstacleOperator::restart(const double t, std::string filename)
{
  #ifdef RL_LAYER
    sr.restart(filename);
  #endif
  std::ifstream restartstream;
  restartstream.open(filename+".txt");
  if(!restartstream.good()){
    printf("Could not restart from file\n");
    return;
  }
  Real restart_time;
  restartstream >> restart_time;
  //assert(std::abs(restart_time-t) < 1e-9);

  restartstream>>position[0]>>position[1]>>position[2];
  restartstream>>absPos[0]>>absPos[1]>>absPos[2];
  restartstream>>quaternion[0]>>quaternion[1]>>quaternion[2]>>quaternion[3];
  restartstream>>transVel[0]>>transVel[1]>>transVel[2];
  restartstream>>angVel[0]>>angVel[1]>>angVel[2];
  restartstream >> _2Dangle;
  restartstream.close();

  {
  std::cout<<"RESTARTED BODY: "<<std::endl;
  std::cout<<"TIME: \t"<<restart_time<<std::endl;
  std::cout<<"POS : \t"<<position[0]<<" "<<position[1]<<" "<<position[2]<<std::endl;
  std::cout<<"ABS POS : \t"<<absPos[0]<<" "<<absPos[1]<<" "<<absPos[2]<<std::endl;
  std::cout<<"ANGLE:\t"<<quaternion[0]<<" "<<quaternion[1]<<" "
                       <<quaternion[2]<<" "<<quaternion[3]<<std::endl;
  std::cout<<"TVEL: \t"<<transVel[0]<<" "<<transVel[1]<<" "<<transVel[2]<<std::endl;
  std::cout<<"AVEL: \t"<<angVel[0]<<" "<<angVel[1]<<" "<<angVel[2]<<std::endl;
  std::cout<<"2D angle: \t"<<_2Dangle<<std::endl;
  }
}

void IF3D_ObstacleOperator::Accept(ObstacleVisitor * visitor)
{
 visitor->visit(this);
}


#ifdef RL_LAYER
void IF3D_ObstacleOperator::getSkinsAndPOV(Real& x, Real& y, Real& th,
  Real*& pXL, Real*& pYL, Real*& pXU, Real*& pYU, int& Npts)
{
  printf("Entered the wrong get skin operator\n");
  fflush(0);
  abort();
}

void IF3D_ObstacleOperator::execute(const int iAgent, const double time, const std::vector<double> action)
{
  printf("Entered the wrong execute operator\n");
  fflush(0);
  abort();
}

void IF3D_ObstacleOperator::interpolateOnSkin(const double time, const int stepID, bool dumpWake)
{
  //printf("Entered the wrong interpolate operator\n");
  //fflush(0);
  //abort();
}

#endif