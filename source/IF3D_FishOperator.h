//
//  IF3D_CarlingFishOperator.h
//  IncompressibleFluids3D
//
//  Created by Wim van Rees on 4/15/13.
//
//

#ifndef __IncompressibleFluids3D__IF3D_FishOperator__
#define __IncompressibleFluids3D__IF3D_FishOperator__

#include <cmath>
#include <array>
#include "IF2D_Frenet.h"
#include "IF3D_ObstacleOperator.h"

struct FishMidlineData
{
	FishMidlineData
};

class IF3D_FishOperator: public IF3D_ObstacleOperator
{
protected:
	FishMidlineData * myFish;
    Real Tperiod, phaseShift, phase, sim_time, sim_dt;
    Real volume_internal, J_internal, CoM_internal[2], vCoM_internal[2];
    Real theta_internal, angvel_internal, angvel_internal_prev, CoM_interpolated[3];
    Real Tstartlearn, GoalDX, new_curv, old_curv, new_Tp, adjTh, angvel_integral[3];
    bool randomStart, bCorrectTrajectory;
    int  nActions;

public:
	
    IF3D_FishOperator(FluidGridMPI * grid, ArgumentParser & parser);
    ~IF3D_FishOperator();
	void save(const int step_id, const Real t, std::string filename = std::string()) override;
	void restart(const Real t, std::string filename = std::string()) override;
    void update(const int step_id, const Real t, const Real dt, const Real *Uinf) override;
    void getCenterOfMass(Real CM[3]) const override;
    void create(const int step_id,const Real time, const Real dt, const Real *Uinf) override;
    void _parseArguments(ArgumentParser & parser);
};


#endif /* defined(__IncompressibleFluids3D__IF3D_Fish__) */
