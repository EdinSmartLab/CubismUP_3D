//
//  Cubism3D
//  Copyright (c) 2018 CSE-Lab, ETH Zurich, Switzerland.
//  Distributed under the terms of the MIT license.
//
//  Created by Guido Novati (novatig@ethz.ch).
//

#include "IF3D_ObstacleVector.h"

#include <sstream>

using std::vector;

vector<std::array<int, 2>> IF3D_ObstacleVector::collidingObstacles()
{
  vector<std::array<int, 2>> colliding; //IDs of colliding obstacles
  //vector containing pointers to defBLock maps:
  vector<std::map<int,ObstacleBlock*>*> obstBlocks(obstacles.size());
  int ID(0);
  vector<int> IDs;
  for(const auto & obstacle_ptr : obstacles) {
      IDs.push_back(ID);
      obstBlocks[ID] = obstacle_ptr->getObstacleBlocksPtr();
      assert(obstBlocks[ID] != nullptr);
      ID++;
  }

  for(int i=1; i<ID; i++)
  for(int j=0; j<i; j++) {
      for(const auto& x: *obstBlocks[i]) { //iter over map of obstacle i
          //check if same block ID is allocated in map of obstacle j:
          const auto y = obstBlocks[j]->find(x.first);
          if(y != obstBlocks[j]->end()) {
            std::array<int,2> hit = {IDs[i],IDs[j]};
            colliding.push_back(hit);
            return colliding;
          }
      }
  }
  return colliding;
}

void IF3D_ObstacleVector::characteristic_function()
{
    for(const auto & obstacle_ptr : obstacles)
        obstacle_ptr->characteristic_function();
}

void IF3D_ObstacleVector::update(const int step_id, const double time, const double dt, const Real* Uinf)
{
    for(const auto & obstacle_ptr : obstacles)
        obstacle_ptr->update(step_id,time,dt,Uinf);
}

void IF3D_ObstacleVector::create(const int step_id,const double time, const double dt, const Real *Uinf)
{
  for(const auto & obstacle_ptr : obstacles)
    obstacle_ptr->create(step_id,time,dt,Uinf);

  int mpistatus = 0;
  for (size_t i = 0; i < obstacles.size(); i++) {
    if(i+1 == obstacles.size() && mpistatus==0 ) mpistatus = 2;
    obstacles[i]->computeChi(step_id,time,dt,Uinf,mpistatus);
  }

  for(const auto & obstacle_ptr : obstacles)
    obstacle_ptr->finalize(step_id,time,dt,Uinf);
}

std::vector<int> IF3D_ObstacleVector::intersectingBlockIDs(const int buffer) const
{
  std::set<int> IDcollection;
  for(const auto & obstacle_ptr : obstacles) {
    std::vector<int> myIDs = obstacle_ptr->intersectingBlockIDs(buffer);
    IDcollection.insert(myIDs.begin(), myIDs.end()); // it's a set, so only unique values are inserted
  }
  return std::vector<int>(IDcollection.begin(), IDcollection.end());
}

void IF3D_ObstacleVector::computeDiagnostics(const int stepID, const double time, const Real* Uinf, const double lambda)
{
  #ifndef RL_LAYER
  for(const auto & obstacle_ptr : obstacles)
    obstacle_ptr->computeDiagnostics(stepID,time,Uinf,lambda);
  #endif
}

void IF3D_ObstacleVector::computeVelocities(const Real* Uinf)
{
  for(const auto & obstacle_ptr : obstacles)
    obstacle_ptr->computeVelocities(Uinf);
}

void IF3D_ObstacleVector::computeForces(const int stepID, const double time, const double dt, const Real* Uinf, const double NU, const bool bDump)
{
  for(const auto & obstacle_ptr : obstacles)
    obstacle_ptr->computeForces(stepID,time,dt,Uinf,NU,bDump);
}

IF3D_ObstacleVector::~IF3D_ObstacleVector()
{
  for(const auto& obstacle_ptr :obstacles) delete obstacle_ptr;
  obstacles.clear();
}

void IF3D_ObstacleVector::save(const int step_id, const double t, std::string filename)
{
  int cntr = 0;
  for(const auto & obstacle_ptr : obstacles) {
    std::stringstream ssR;
    ssR<<filename<<"_"<<cntr;
      obstacle_ptr->save(step_id, t, ssR.str());
      cntr++;
  }
}

void IF3D_ObstacleVector::restart(const double t, std::string filename)
{
    int cntr = 0;
    for(const auto & obstacle_ptr : obstacles) {
      std::stringstream ssR;
      ssR<<filename<<"_"<<cntr;
        obstacle_ptr->restart(t, ssR.str());
        cntr++;
    }
}

void IF3D_ObstacleVector::Accept(ObstacleVisitor * visitor)
{
  for(size_t i=0;i<obstacles.size();++i)
    obstacles[i]->Accept(visitor);
}
Real IF3D_ObstacleVector::getD() const
{
  Real maxL(0);
  for(size_t i=0; i<obstacles.size(); ++i) {
      const Real Li = obstacles[i]->getD();
      maxL = std::max(maxL,Li);
      assert(Li>0.);
  }
  return maxL;
}

#ifdef RL_LAYER

std::vector<StateReward*> IF3D_ObstacleVector::_getData()
{
    int ind = 0;
    std::vector<StateReward*> _D(obstacles.size());
    for(const auto & obstacle_ptr : obstacles)
        _D[ind++] = obstacle_ptr->_getData();
    return _D;
}

void IF3D_ObstacleVector::execute(const int iAgent, const double time, const vector<double> action)
{
   obstacles[iAgent]->execute(iAgent, time, action);
}

struct swimmerInFOV
{ //for now, assume we only have fish, simplifies a lot edge detection
  Real xPOV, yPOV, thetaPOV; //these are not modified
  int Npts;
  Real *xSurfLower, *ySurfLower, *xSurfUpper, *ySurfUpper;

  swimmerInFOV() : xPOV(0), yPOV(0), thetaPOV(0), Npts(0) {}
};

struct rayTracing
{
  vector<swimmerInFOV*> FOVobst;
  vector<StateReward*> FOVdata;

  Real findDistanceOnSkin(const Real* const xSkin, const Real* const ySkin,
    const int N, const int jPOV, const int pRay) const
  {
    const Real xPOV  = FOVobst[jPOV]->xPOV;
    const Real yPOV  = FOVobst[jPOV]->yPOV;
    const Real thPOV = FOVobst[jPOV]->thetaPOV;
    //const int NpLatLine = FOVdata[jPOV]->NpLatLine;
    const Real thRAY = pRay * M_PI /(Real)NpLatLine; //0 to 2pi

    //hardcoded, resolve skin with 51 points and find distance of interection with 50 segments
    const int nS = 50;
    Real dist = 100.;
    for (int i=0; i<nS; i++) { //
      const int iS =     i*(Real)N/(Real)nS;
      const int iE = (i+1)*(Real)N/(Real)nS;
      const Real x_S = xPOV - *(xSkin+iS);
      const Real y_S = yPOV - *(ySkin+iS);
      const Real x_E = xPOV - *(xSkin+iE);
      const Real y_E = yPOV - *(ySkin+iE);
      //puts the points between 0 and 2 pi from the point of view of the ray
      const Real thSPOV = std::fmod(std::atan2(y_S,x_S)-thPOV-thRAY,2*M_PI);
      const Real thS_POV = thSPOV < 0 ? thSPOV+2*M_PI : thSPOV;
      const Real thEPOV = std::fmod(std::atan2(y_E,x_E)-thPOV-thRAY,2*M_PI);
      const Real thE_POV = thEPOV < 0 ? thEPOV+2*M_PI : thEPOV;

      const Real distS = std::sqrt( std::pow(y_S, 2) + std::pow(x_S, 2));
      const Real distE = std::sqrt( std::pow(y_E, 2) + std::pow(x_E, 2));
      assert(thS_POV>=0 && thS_POV<=2*M_PI && thE_POV>=0 && thE_POV<=2*M_PI);
      //sort points so that theta1st < theta2nd
      Real dist1st(0.), dist2nd(0.), theta1st(0.), theta2nd(0.);
      if( thS_POV > thE_POV ) {
        if ( thS_POV - thE_POV > M_PI ) {  //then E is positive and S neg
          theta1st = thS_POV - 2*M_PI;
          theta2nd = thE_POV;
          dist1st = distS;
          dist2nd = distE;
        } else {
          theta1st = thE_POV;
          theta2nd = thS_POV;
          dist1st = distE;
          dist2nd = distS;
        }
      } else { // thE_POV > thS_POV
        if ( thE_POV - thS_POV > M_PI ) {  //then S is positive and E neg
          theta1st = thE_POV - 2*M_PI;
          theta2nd = thS_POV;
          dist1st = distE;
          dist2nd = distS;
        } else {
          theta1st = thS_POV;
          theta2nd = thE_POV;
          dist1st = distS;
          dist2nd = distE;
        }
      }
      assert(dist1st>0 && dist2nd>0 && theta2nd>theta1st);
      //if segment intersected by ray, return distance of closest point
      if (theta2nd>0 && theta1st<0) {
        if (theta2nd < -theta1st) {
            if (dist2nd < dist) dist = dist2nd;
        } else {
            if (dist1st < dist) dist = dist1st;
        }
      }
    }
    return dist;
  }

  void execute(const Real lengthscale)
  {
    int Nrays(0);
    for (size_t j=0; j<FOVobst.size(); j++) Nrays = max(Nrays, NpLatLine);
      //Nrays = max(Nrays,FOVdata[j]->NpLatLine);

    #pragma omp parallel for collapse(2)
    for (size_t j=0; j<FOVobst.size(); j++)  //loop over obstacles
    for (int p=0; p<2*Nrays; p++)   { //loop over sight rays of obstacle
      if(p > 2*NpLatLine - 1) continue;
      Real dist = 5*lengthscale;
      for (size_t i=0; i<FOVobst.size(); i++) {
        if (i==j) continue;
        const int N = FOVobst[i]->Npts-1;

        const Real* const xSkinU = FOVobst[i]->xSurfUpper;
        const Real* const ySkinU = FOVobst[i]->ySurfUpper;
        dist = std::min(dist, findDistanceOnSkin(xSkinU, ySkinU, N, j, p));

        const Real* const xSkinL = FOVobst[i]->xSurfLower;
        const Real* const ySkinL = FOVobst[i]->ySurfLower;
        dist = std::min(dist, findDistanceOnSkin(xSkinL, ySkinL, N, j, p));
      }
     FOVdata[j]->raySight[p] = dist;
    }
  }

  rayTracing(vector<swimmerInFOV*>& _FOVobst, vector<StateReward*>& _FOVdata) :
  FOVobst(_FOVobst), FOVdata(_FOVdata)
  {
      assert(FOVobst.size() == FOVdata.size());
  }
};

void IF3D_ObstacleVector::getFieldOfView(const double lengthscale)
{
  #ifdef __useSkin_
    //two ingredients: all skins and containers for perceptions
    vector<swimmerInFOV*> FOVobst;
    vector<StateReward*>  FOVdata;
    int cnt = 0;
    for(const auto & obst_ptr : obstacles) { //stand-in
      //if (obst_ptr->bCheckCollisions)
      if(not obst_ptr->bHasSkin) return;
      swimmerInFOV * F = new swimmerInFOV();

      obst_ptr->getSkinsAndPOV(F->xPOV, F->yPOV, F->thetaPOV,
      F->xSurfLower, F->ySurfLower, F->xSurfUpper, F->ySurfUpper, F->Npts);
      StateReward* D = obst_ptr->_getData();

      FOVobst.push_back(F);
      FOVdata.push_back(D);
      cnt++;
    }
    if(!cnt) return;
    rayTracing tracing(FOVobst, FOVdata);
    tracing.execute(lengthscale);
  #endif
}

void IF3D_ObstacleVector::interpolateOnSkin(const double time, const int step, bool dumpWake)
{
    for(const auto & obst_ptr : obstacles)
      if(obst_ptr->bHasSkin)
        obst_ptr->interpolateOnSkin(time, step, dumpWake);
}

#endif
