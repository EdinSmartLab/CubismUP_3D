//
//  Cubism3D
//  Copyright (c) 2018 CSE-Lab, ETH Zurich, Switzerland.
//  Distributed under the terms of the MIT license.
//
//  Created by Guido Novati (novatig@ethz.ch).
//

#include "obstacles/IF3D_CylinderObstacleOperator.h"
#include "obstacles/extra/IF3D_ObstacleLibrary.h"
#include "Cubism/ArgumentParser.h"

namespace DCylinderObstacle
{
struct FillBlocks : FillBlocksBase<FillBlocks>
{
  const Real radius, halflength, safety;
  const double position[3];
  const Real box[3][2] = {
    {position[0] - radius     - safety, position[0]              + safety},
    {position[1] - radius     - safety, position[1] + radius     + safety},
    {position[2] - halflength - safety, position[2] + halflength + safety}
  };

  FillBlocks(const Real r, const Real halfl, const Real h, const double p[3]):
  radius(r), halflength(halfl), safety(2*h), position{p[0],p[1],p[2]} {}

  inline bool isTouching(const FluidBlock&b, const int buffer_dx=0) const
  {
    const Real intersect[3][2] = {
        std::max(b.min_pos[0], box[0][0]),
        std::min(b.max_pos[0], box[0][1]),
        std::max(b.min_pos[1], box[1][0]),
        std::min(b.max_pos[1], box[1][1]),
        std::max(b.min_pos[2], box[2][0]),
        std::min(b.max_pos[2], box[2][1])
    };
    return intersect[0][1]-intersect[0][0]>0 &&
           intersect[1][1]-intersect[1][0]>0 &&
           intersect[2][1]-intersect[2][0]>0;
  }

  inline Real signedDistance(const Real xo, const Real yo, const Real zo) const
  {
    const Real x = xo - position[0], y = yo - position[1], z = zo - position[2];
    const Real planeDist = std::min( -x, radius-std::sqrt(x*x+y*y) );
    const Real vertiDist = halflength - std::fabs(z);
    return std::min(planeDist, vertiDist);
  }
};
}

namespace CylinderObstacle
{
struct FillBlocks : FillBlocksBase<FillBlocks>
{
  const Real radius, halflength, safety;
  const double position[3];
  const Real box[3][2] = {
    {position[0] - radius     - safety, position[0] + radius     + safety},
    {position[1] - radius     - safety, position[1] + radius     + safety},
    {position[2] - halflength - safety, position[2] + halflength + safety}
  };

  FillBlocks(const Real r, const Real halfl, const Real h, const double p[3]):
  radius(r), halflength(halfl), safety(2*h), position{p[0],p[1],p[2]} {}

  inline bool isTouching(const FluidBlock&b, const int buffer_dx=0) const
  {
    const Real intersect[3][2] = {
        std::max(b.min_pos[0], box[0][0]),
        std::min(b.max_pos[0], box[0][1]),
        std::max(b.min_pos[1], box[1][0]),
        std::min(b.max_pos[1], box[1][1]),
        std::max(b.min_pos[2], box[2][0]),
        std::min(b.max_pos[2], box[2][1])
    };
    return intersect[0][1]-intersect[0][0]>0 &&
           intersect[1][1]-intersect[1][0]>0 &&
           intersect[2][1]-intersect[2][0]>0;
  }

  inline Real signedDistance(const Real xo, const Real yo, const Real zo) const
  {
    const Real x = xo - position[0], y = yo - position[1], z = zo - position[2];
    const Real planeDist = radius - std::sqrt(x*x+y*y);
    const Real vertiDist = halflength - std::fabs(z);
    return std::min(planeDist, vertiDist);
  }
};
}

IF3D_CylinderObstacleOperator::IF3D_CylinderObstacleOperator(
    SimulationData&s, ArgumentParser &p)
    : IF3D_ObstacleOperator(s, p), radius(.5 * length),
      halflength(p("-halflength").asDouble(.5 * sim.extent[2]))
{
  section = p("-section").asString("circular");
  _init();
}


IF3D_CylinderObstacleOperator::IF3D_CylinderObstacleOperator(
    SimulationData& s,
    ObstacleArguments &args,
    const double radius_,
    const double halflength_)
    : IF3D_ObstacleOperator(s, args), radius(radius_), halflength(halflength_)
{
  _init();
}

void IF3D_CylinderObstacleOperator::_init(void)
{
  printf("Created IF3D_CylinderObstacleOperator with radius %f and halflength %f\n", radius, halflength);

  // D-cyl can float around the domain, but does not support rotation. TODO
  bBlockRotation[0] = true;
  bBlockRotation[1] = true;
  bBlockRotation[2] = true;
}


void IF3D_CylinderObstacleOperator::create()
{
  if(section == "D")
  {
    const DCylinderObstacle::FillBlocks kernel(radius, halflength, vInfo[0].h_gridpoint, position);
    create_base<DCylinderObstacle::FillBlocks>(kernel);
  }
  else /* else do square section, but figure how to make code smaller */
  {    /* else normal cylinder */
    const CylinderObstacle::FillBlocks kernel(radius, halflength, vInfo[0].h_gridpoint, position);
    create_base<CylinderObstacle::FillBlocks>(kernel);
  }
}

void IF3D_CylinderObstacleOperator::finalize()
{
  // this method allows any computation that requires the char function
  // to be computed. E.g. compute the effective center of mass or removing
  // momenta from udef
}
