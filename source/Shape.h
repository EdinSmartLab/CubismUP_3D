//
//  Shape.h
//  CubismUP_3D
//
//	Virtual shape class which defines the interface
//	Default simple geometries are also provided and can be used as references
//
//	This class only contains static information (position, orientation,...), no dynamics are included (e.g. velocities,...)
//
//  Created by Christian Conti on 3/6/15.
//  Copyright (c) 2015 ETHZ. All rights reserved.
//

#ifndef CubismUP_3D_Shape_h
#define CubismUP_3D_Shape_h

#include "GeometryReader.h"
#include "WavefrontReader.h"
#include "CTReader.h"

class Shape
{
protected:
	// general quantities
	Geometry::Properties properties;
	
	// scale,translate
	Real scaleFactor;
	Geometry::Point translationFactor;
	
	// smoothing
	const Real mollChi;
	const Real mollRho; // currently not used - need to change in rho method
	
	Real smoothHeaviside(Real rR, Real radius, Real eps)
	{
		if (rR < radius-eps*.5)
			return (Real) 1.;
		else if (rR > radius+eps*.5)
			return (Real) 0.;
		else
			return (Real) ((1.+cos(M_PI*((rR-radius)/eps+.5)))*.5);
	}
	
public:
	Shape(Real center[3], const Real rhoS, const Real mollChi, const Real mollRho, Real scale=1, Real tX=0, Real tY=0, Real tZ=0, Geometry::Quaternion orientation=Geometry::Quaternion()) : mollChi(mollChi), mollRho(mollRho), scaleFactor(scale), translationFactor(tX,tY,tZ)
	{
		properties.com.x = center[0];
		properties.com.y = center[1];
		properties.com.z = center[2];
		
		properties.centroid.x = center[0];
		properties.centroid.y = center[1];
		properties.centroid.z = center[2];
		
		properties.ut.x = 0;
		properties.ut.y = 0;
		properties.ut.z = 0;
		
		properties.dthetadt.x = 0;
		properties.dthetadt.y = 0;
		properties.dthetadt.z = 0;
		
		properties.density = rhoS;
		
		properties.q = orientation;
		
		properties.rotation[0][0] = 1-2*(properties.q.y*properties.q.y + properties.q.z*properties.q.z);
		properties.rotation[0][1] =   2*(properties.q.x*properties.q.y - properties.q.w*properties.q.z);
		properties.rotation[0][2] =   2*(properties.q.x*properties.q.z + properties.q.w*properties.q.y);
		properties.rotation[1][0] =   2*(properties.q.x*properties.q.y + properties.q.w*properties.q.z);
		properties.rotation[1][1] = 1-2*(properties.q.x*properties.q.x + properties.q.z*properties.q.z);
		properties.rotation[1][2] =   2*(properties.q.y*properties.q.z - properties.q.w*properties.q.x);
		properties.rotation[2][0] =   2*(properties.q.x*properties.q.z - properties.q.w*properties.q.y);
		properties.rotation[2][1] =   2*(properties.q.y*properties.q.z + properties.q.w*properties.q.x);
		properties.rotation[2][2] = 1-2*(properties.q.x*properties.q.x + properties.q.y*properties.q.y);
		
		//cout << properties.q.w << " " << properties.q.x << " " << properties.q.y << " " << properties.q.z << endl;
		//cout << properties.rotation[0][0] << " " << properties.rotation[0][1] << " " << properties.rotation[0][2] << endl;
		//cout << properties.rotation[1][0] << " " << properties.rotation[1][1] << " " << properties.rotation[1][2] << endl;
		//cout << properties.rotation[2][0] << " " << properties.rotation[2][1] << " " << properties.rotation[2][2] << endl;
		
		cout << "WARNING - shape not completely initialized\n";
		// to initialize: mass, minb, maxb, J
	}
	
	virtual ~Shape() {}
	
	virtual Real chi(Real p[3], Real h) const = 0;
	virtual Real getCharLength() const = 0;
	
	virtual void updatePosition(Real u[3], Real dthetadt[3], Real J[6], Real mass, Real dt)
	{
		properties.ut.x = u[0];
		properties.ut.y = u[1];
		properties.ut.z = u[2];
		
		for (int i=0; i<6; i++)
			properties.J[i] = J[i];
		//	cout << "Inertia difference for component " << i << "\t" << abs(properties.J[i] - J[i]) << "(" << properties.J[i] << " " << J[i] << " " << properties.J[i]/J[i] << ")" << endl;
		
		const double detJ = properties.J[0]*(properties.J[1]*properties.J[2] - properties.J[5]*properties.J[5]) +
		properties.J[3]*(properties.J[4]*properties.J[5] - properties.J[2]*properties.J[3]) +
		properties.J[4]*(properties.J[3]*properties.J[5] - properties.J[1]*properties.J[4]);
		assert(abs(detJ)>numeric_limits<double>::epsilon());
		const double invDetJ = 1./detJ;
		
		const double invJ[6] = {
			invDetJ * (properties.J[1]*properties.J[2] - properties.J[5]*properties.J[5]),
			invDetJ * (properties.J[0]*properties.J[2] - properties.J[4]*properties.J[4]),
			invDetJ * (properties.J[0]*properties.J[1] - properties.J[3]*properties.J[3]),
			invDetJ * (properties.J[4]*properties.J[5] - properties.J[2]*properties.J[3]),
			invDetJ * (properties.J[3]*properties.J[5] - properties.J[1]*properties.J[4]),
			invDetJ * (properties.J[3]*properties.J[4] - properties.J[0]*properties.J[5])
		};
		
		// J-1 * dthetadt
		// angular velocity from angular momentum
		properties.dthetadt.x = (invJ[0]*dthetadt[0] + invJ[3]*dthetadt[1] + invJ[4]*dthetadt[2]);
		properties.dthetadt.y = (invJ[3]*dthetadt[0] + invJ[1]*dthetadt[1] + invJ[5]*dthetadt[2]);
		properties.dthetadt.z = (invJ[4]*dthetadt[0] + invJ[5]*dthetadt[1] + invJ[2]*dthetadt[2]);
		
		properties.update(dt);
	}
	
	void getCentroid(Real centroid[3])
	{
		centroid[0] = properties.centroid.x;
		centroid[1] = properties.centroid.y;
		centroid[2] = properties.centroid.z;
	}
	
	void getCenterOfMass(Real com[3])
	{
		com[0] = properties.com.x;
		com[1] = properties.com.y;
		com[3] = properties.com.z;
	}
	
	void getOrientation(Real rotation[3][3]) const
	{
		rotation[0][0] = properties.rotation[0][0];
		rotation[0][1] = properties.rotation[0][1];
		rotation[0][2] = properties.rotation[0][2];
		rotation[1][0] = properties.rotation[1][0];
		rotation[1][1] = properties.rotation[1][1];
		rotation[1][2] = properties.rotation[1][2];
		rotation[2][0] = properties.rotation[2][0];
		rotation[2][1] = properties.rotation[2][1];
		rotation[2][2] = properties.rotation[2][2];
	}
	
	virtual inline Real getRhoS() const
	{
		return properties.density;
	}
	
	virtual Real rho(Real p[3], Real h, Real mask) const
	{
		return properties.density*mask + 1.*(1.-mask);
	}
	
	virtual Real rho(Real p[3], Real h) const
	{
		Real mask = chi(p,h);
		return rho(p,h,mask);
	}
	
	virtual void outputSettings(ostream &outStream)
	{
		outStream << "centerX " << properties.centroid.x << endl;
		outStream << "centerY " << properties.centroid.y << endl;
		outStream << "centerZ " << properties.centroid.z << endl;
		outStream << "centerMassX " << properties.com.x << endl;
		outStream << "centerMassY " << properties.com.y << endl;
		outStream << "centerMassZ " << properties.com.z << endl;
		//outStream << "orientation " << orientation << endl;
		outStream << "rhoS " << properties.density << endl;
		outStream << "mollChi " << mollChi << endl;
		outStream << "mollRho " << mollRho << endl;
	}
};

class Sphere : public Shape
{
protected:
	Real radius;
	
public:
	Sphere(Real center[3], Real radius, const Real rhoS, const Real mollChi, const Real mollRho) : Shape(center, rhoS, mollChi, mollRho), radius(radius)
	{
	}
	
	Real chi(Real p[3], Real h) const
	{
		const Real d[3] = { abs(p[0]-properties.centroid.x), abs(p[1]-properties.centroid.y), abs(p[2]-properties.centroid.z) };
		const Real dist = sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
		
		return smoothHeaviside(dist, radius, mollChi*sqrt(2)*h);
	}
	
	Real getCharLength() const
	{
		return 2 * radius;
	}
	
	void outputSettings(ostream &outStream)
	{
		outStream << "Sphere\n";
		outStream << "radius " << radius << endl;
		
		Shape::outputSettings(outStream);
	}
};

class GeometryMesh : public Shape
{
protected:
	GeometryReader * geometry;
	
public:
	GeometryMesh(const string filename, const int gridsize, Real center[3], const Real rhoS, const Real mollChi, const Real mollRho, Real scale=1, Real tX=0, Real tY=0, Real tZ=0, Geometry::Quaternion orientation=Geometry::Quaternion()) : Shape(center, rhoS, mollChi, mollRho, scale, tX, tY, tZ, orientation)
	{
		Geometry::Point transFactor(tX,tY,tZ);
#ifdef _CT_
		geometry = new GeometryReaderCT(filename,properties,gridsize,scale,transFactor);
#else
		geometry = new GeometryReaderOBJ(filename,properties,gridsize,scale,transFactor);
#endif
	}
	
	Real chi(Real p[3], Real h) const
	{
		return smoothHeaviside(geometry->distance(p[0], p[1], p[2]), 0.01, mollChi*sqrt(2)*h);
		//return smoothHeaviside(geometry->distance(p[0], p[1], p[2]), 0.004, mollChi*sqrt(2)*h);
	}
	
	Real getCharLength() const
	{
		//cout << "Not implemented yet\n";
		return .1;
	}
	
	void outputSettings(ostream &outStream)
	{
		//cout << "Not implemented yet\n";
		//abort();
	}
};

#endif
