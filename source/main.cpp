//
//  main.cpp
//  CubismUP_3D
//
//  Created by Christian Conti on 1/7/15.
//  Copyright (c) 2015 ETHZ. All rights reserved.
//

#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <sstream>
using namespace std;
#include "Simulation.h"
#include "Save_splicer.h"

int main(int argc, char **argv)
{
	Communicator * communicator = nullptr;
	int provided;
	MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided);
	if (provided < MPI_THREAD_FUNNELED) {
		printf("ERROR: The MPI implementation does not have required thread support\n");
		fflush(0);
		MPI_Abort(MPI_COMM_WORLD, 1);
	}
	int rank;
	MPI_Comm_rank(MPI_COMM_WORLD,&rank);

	ArgumentParser parser(argc,argv);
	parser.set_strict_mode();

	if (rank==0) {
		cout << "====================================================================================================================\n";
		cout << "\t\tCubism UP 3D (velocity-pressure 3D incompressible Navier-Stokes solver)\n";
		cout << "====================================================================================================================\n";
	}
	parser.unset_strict_mode();
	#ifdef __RL_MPI_CLIENT
	const int _sockID = parser("-sock").asInt(-1);
	const int nActions = parser("-nActions").asInt(0);
	//const int nStates = (nActions==1) ? 20+200 : 25+200;
	const int nStates = (nActions==1) ? 20+10*__NpLatLine : 25+10*__NpLatLine;
	if (_sockID>=0 && nActions>0) {
		if(!rank)
			printf("Communicating over sock %d\n", _sockID);
		//communicator = new Communicator(_sockID,nStates,nActions,MPI_COMM_WORLD);
		communicator = NULL; 
	}
	#endif

	const bool io_job = parser("-saveSplicer").asBool(false);
	if (io_job) {
		Save_splicer * sim = new Save_splicer(argc, argv);
		sim->run();
		MPI_Finalize();
		return 0;
	}

	Simulation * sim = new Simulation(MPI_COMM_WORLD, communicator, argc, argv);
	sim->init();
	sim->simulate();

	MPI_Finalize();
	if(communicator not_eq nullptr)
			delete communicator;
	delete sim;
	return 0;
}
