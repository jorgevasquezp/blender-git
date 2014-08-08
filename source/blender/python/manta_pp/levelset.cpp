




// DO NOT EDIT !
// This file is generated using the MantaFlow preprocessor (prep generate).




/******************************************************************************
 *
 * MantaFlow fluid solver framework
 * Copyright 2011 Tobias Pfaff, Nils Thuerey 
 *
 * This program is free software, distributed under the terms of the
 * GNU General Public License (GPL) 
 * http://www.gnu.org/licenses
 *
 * Levelset
 *
 ******************************************************************************/

#include "levelset.h"
#include "fastmarch.h"
#include "kernel.h"
#include "mcubes.h"
#include "mesh.h"

using namespace std;
namespace Manta {

//************************************************************************
// Helper functions and kernels for marching

static const int FlagInited = FastMarch<FmHeapEntryOut, +1>::FlagInited;

// neighbor lookup vectors
static const Vec3i neighbors[6] = { Vec3i(-1,0,0), Vec3i(1,0,0), Vec3i(0,-1,0), Vec3i(0,1,0), Vec3i(0,0,-1), Vec3i(0,0,1) };
	

 struct InitFmIn : public KernelBase { InitFmIn(FlagGrid& flags, Grid<int>& fmFlags, LevelsetGrid& phi, bool ignoreWalls, int obstacleType) :  KernelBase(&flags,1) ,flags(flags),fmFlags(fmFlags),phi(phi),ignoreWalls(ignoreWalls),obstacleType(obstacleType)   { run(); }  inline void op(int i, int j, int k, FlagGrid& flags, Grid<int>& fmFlags, LevelsetGrid& phi, bool ignoreWalls, int obstacleType )  {
	const int idx = flags.index(i,j,k);
	const Real v = phi[idx];
	if (v>=0 && (!ignoreWalls || (flags[idx] & obstacleType) == 0))
		fmFlags[idx] = FlagInited;
	else
		fmFlags[idx] = 0;
}   inline FlagGrid& getArg0() { return flags; } typedef FlagGrid type0;inline Grid<int>& getArg1() { return fmFlags; } typedef Grid<int> type1;inline LevelsetGrid& getArg2() { return phi; } typedef LevelsetGrid type2;inline bool& getArg3() { return ignoreWalls; } typedef bool type3;inline int& getArg4() { return obstacleType; } typedef int type4; void run() {  const int _maxX = maxX; const int _maxY = maxY; for (int k=minZ; k< maxZ; k++) for (int j=1; j< _maxY; j++) for (int i=1; i< _maxX; i++) op(i,j,k, flags,fmFlags,phi,ignoreWalls,obstacleType);  } FlagGrid& flags; Grid<int>& fmFlags; LevelsetGrid& phi; bool ignoreWalls; int obstacleType;   };


 struct InitFmOut : public KernelBase { InitFmOut(FlagGrid& flags, Grid<int>& fmFlags, LevelsetGrid& phi, bool ignoreWalls, int obstacleType) :  KernelBase(&flags,1) ,flags(flags),fmFlags(fmFlags),phi(phi),ignoreWalls(ignoreWalls),obstacleType(obstacleType)   { run(); }  inline void op(int i, int j, int k, FlagGrid& flags, Grid<int>& fmFlags, LevelsetGrid& phi, bool ignoreWalls, int obstacleType )  {
	const int idx = flags.index(i,j,k);
	const Real v = phi[idx];
	if (ignoreWalls) {
		fmFlags[idx] = (v<0) ? FlagInited : 0;
		if ((flags[idx] & obstacleType) != 0) {
			fmFlags[idx] = 0;
			phi[idx] = 0;
		}
	}
	else
		fmFlags[idx] = (v<0) ? FlagInited : 0;
}   inline FlagGrid& getArg0() { return flags; } typedef FlagGrid type0;inline Grid<int>& getArg1() { return fmFlags; } typedef Grid<int> type1;inline LevelsetGrid& getArg2() { return phi; } typedef LevelsetGrid type2;inline bool& getArg3() { return ignoreWalls; } typedef bool type3;inline int& getArg4() { return obstacleType; } typedef int type4; void run() {  const int _maxX = maxX; const int _maxY = maxY; for (int k=minZ; k< maxZ; k++) for (int j=1; j< _maxY; j++) for (int i=1; i< _maxX; i++) op(i,j,k, flags,fmFlags,phi,ignoreWalls,obstacleType);  } FlagGrid& flags; Grid<int>& fmFlags; LevelsetGrid& phi; bool ignoreWalls; int obstacleType;   };


 struct SetUninitialized : public KernelBase { SetUninitialized(Grid<int>& fmFlags, LevelsetGrid& phi, const Real val) :  KernelBase(&fmFlags,1) ,fmFlags(fmFlags),phi(phi),val(val)   { run(); }  inline void op(int i, int j, int k, Grid<int>& fmFlags, LevelsetGrid& phi, const Real val )  {
	if (fmFlags(i,j,k) != FlagInited)
		phi(i,j,k) = val;
}   inline Grid<int>& getArg0() { return fmFlags; } typedef Grid<int> type0;inline LevelsetGrid& getArg1() { return phi; } typedef LevelsetGrid type1;inline const Real& getArg2() { return val; } typedef Real type2; void run() {  const int _maxX = maxX; const int _maxY = maxY; for (int k=minZ; k< maxZ; k++) for (int j=1; j< _maxY; j++) for (int i=1; i< _maxX; i++) op(i,j,k, fmFlags,phi,val);  } Grid<int>& fmFlags; LevelsetGrid& phi; const Real val;   };

template<bool inward>
inline bool isAtInterface(Grid<int>& fmFlags, LevelsetGrid& phi, const Vec3i& p) {
	// check for interface
	for (int nb=0; nb<6; nb++) {
		const Vec3i pn(p + neighbors[nb]);
		if (!fmFlags.isInBounds(pn)) continue;
		
		if (fmFlags(pn) != FlagInited) continue;
		if ((inward && phi(pn) >= 0) || 
			(!inward && phi(pn) < 0)) return true;
	}
	return false;
}

// helper function to compute normal
inline Vec3 getNormal(const Grid<Real>& data, int i, int j, int k) {
	if (i > data.getSizeX()-2) i= data.getSizeX()-2;
	if (j > data.getSizeY()-2) j= data.getSizeY()-2;
	if (k > data.getSizeZ()-2) k= data.getSizeZ()-2;
	if (i < 1) i = 1;
	if (j < 1) j = 1;
	if (k < 1) k = 1;
	return Vec3( data(i+1,j  ,k  ) - data(i-1,j  ,k  ) ,
				 data(i  ,j+1,k  ) - data(i  ,j-1,k  ) ,
				 data(i  ,j  ,k+1) - data(i  ,j  ,k-1) );
}

//************************************************************************
// Levelset class def

LevelsetGrid::LevelsetGrid(FluidSolver* parent, bool show) 
	: Grid<Real>(parent, show) 
{ 
	mType = (GridType)(TypeLevelset | TypeReal);    
}    

Real LevelsetGrid::invalidTimeValue() {
	return FastMarch<FmHeapEntryOut, 1>::InvalidTime();
}

//! Kernel: perform levelset union
 struct KnJoin : public KernelBase { KnJoin(Grid<Real>& a, const Grid<Real>& b) :  KernelBase(&a,0) ,a(a),b(b)   { run(); }  inline void op(int idx, Grid<Real>& a, const Grid<Real>& b )  {
	a[idx] = min(a[idx], b[idx]);
}   inline Grid<Real>& getArg0() { return a; } typedef Grid<Real> type0;inline const Grid<Real>& getArg1() { return b; } typedef Grid<Real> type1; void run() {  const int _sz = size; for (int i=0; i < _sz; i++) op(i, a,b);  } Grid<Real>& a; const Grid<Real>& b;   };

void LevelsetGrid::join(const LevelsetGrid& o) {
	KnJoin(*this, o);
}

//! re-init levelset and extrapolate velocities (in & out)
//  note - uses flags to identify border (could also be done based on ls values)
void LevelsetGrid::reinitMarching(
		FlagGrid& flags, Real maxTime, MACGrid* velTransport,
		bool ignoreWalls, bool correctOuterLayer, int obstacleType
		, Grid<Real>* normSpeed )
{
	const int dim = (is3D() ? 3 : 2);
	
	Grid<int> fmFlags(mParent);
	LevelsetGrid& phi = *this;

	FastMarch<FmHeapEntryIn,  -1> marchIn (flags, fmFlags, phi, maxTime, NULL, NULL);
	
	// march inside
	InitFmIn (flags, fmFlags, phi, ignoreWalls, obstacleType);
	
	FOR_IJK_BND(flags, 1) {
		if (fmFlags(i,j,k) == FlagInited) continue;
		if ((flags(i,j,k) & obstacleType) != 0) continue;
		const Vec3i p(i,j,k);
				
		if(isAtInterface<true>(fmFlags, phi, p)) {
			// set value
			fmFlags(p) = FlagInited;
			
			// add neighbors that are not at the interface
			for (int nb=0; nb<2*dim; nb++) {
				const Vec3i pn(p + neighbors[nb]); // index always valid due to bnd=1                
				if ((flags.get(pn) & obstacleType) != 0) continue;
				
				// check neighbors of neighbor
				if (phi(pn) < 0 && !isAtInterface<true>(fmFlags, phi, pn)) {
					marchIn.addToList(pn, p); 
				}
			}            
		}
	}
	marchIn.performMarching();     
	// done with inwards marching
   
	// now march out...    
	
	// set un initialized regions
	SetUninitialized (fmFlags, phi, -maxTime - 1.); 

	InitFmOut (flags, fmFlags, phi, ignoreWalls, obstacleType);
	
	FastMarch<FmHeapEntryOut, +1> marchOut(flags, fmFlags, phi, maxTime, velTransport, normSpeed);

	// NT_DEBUG
	if(normSpeed && velTransport) {
		FOR_IJK_BND(flags, 1) {
			Vec3 vel  = velTransport->getCentered(i,j,k);
			Vec3 norm = getNormal(phi, i,j,k);  normalize(norm);
			(*normSpeed)(i,j,k) = dot( norm , vel );
		}
	}
	
	// by default, correctOuterLayer is on
	if (correctOuterLayer) {
		// normal version, inwards march is done, now add all outside values (0..2] to list
		// note, this might move the interface a bit! but keeps a nice signed distance field...        
		FOR_IJK_BND(flags, 1) {
			if ((flags(i,j,k) & obstacleType) != 0) continue;
			const Vec3i p(i,j,k);
			
			// check nbs
			for (int nb=0; nb<2*dim; nb++) {
				const Vec3i pn(p + neighbors[nb]); // index always valid due to bnd=1                
				
				if (fmFlags(pn) != FlagInited) continue;
				if ((flags.get(pn) & obstacleType) != 0) continue;
				
				const Real nbPhi = phi(pn);
				
				// only add nodes near interface, not e.g. outer boundary vs. invalid region                
				if (nbPhi < 0 && nbPhi >= -2)
					marchOut.addToList(p, pn); 
			}
		}         
	} else {
		// alternative version, keep interface, do not distort outer cells
		// add all ouside values, but not those at the IF layer
		FOR_IJK_BND(flags, 1) {
			if ((flags(i,j,k) & obstacleType) != 0) continue;
			
			// only look at ouside values
			const Vec3i p(i,j,k);
			if (phi(p) < 0) continue;
			
			if (isAtInterface<false>(fmFlags, phi, p)) {
				// now add all non, interface neighbors
				fmFlags(p) = FlagInited;
				
				// add neighbors that are not at the interface
				for (int nb=0; nb<2*dim; nb++) {
					const Vec3i pn(p + neighbors[nb]); // index always valid due to bnd=1                
					if ((flags.get(pn) & obstacleType) != 0) continue;
				
					// check neighbors of neighbor
					if (phi(pn) > 0 && !isAtInterface<false>(fmFlags, phi, pn))
						marchOut.addToList(pn, p);
				}            
			}
		}
	}    
	marchOut.performMarching();
	
	// set un initialized regions
	SetUninitialized (fmFlags, phi, +maxTime + 1.);    
	
}

void LevelsetGrid::initFromFlags(FlagGrid& flags, bool ignoreWalls) {
	FOR_IDX(*this) {
		if (flags.isFluid(idx) || (ignoreWalls && flags.isObstacle(idx)))
			mData[idx] = -0.5;
		else
			mData[idx] = 0.5;
	}
}

// note - the following functions are experimental, might be removed at some point NT_DEBUG
 struct knGridRemapLsMask : public KernelBase { knGridRemapLsMask(Grid<Real>& me, Real min, Real max, Real fac) :  KernelBase(&me,0) ,me(me),min(min),max(max),fac(fac)   { run(); }  inline void op(int idx, Grid<Real>& me, Real min, Real max, Real fac )  { 
	me[idx] = (clamp(me[idx], min, max) - min) * fac; 

	// now we have 0..1 range, convert to hat around 1/2
	me[idx] = me[idx] * 2.;
	if(me[idx]>1.0) me[idx] = 2. - me[idx];
}   inline Grid<Real>& getArg0() { return me; } typedef Grid<Real> type0;inline Real& getArg1() { return min; } typedef Real type1;inline Real& getArg2() { return max; } typedef Real type2;inline Real& getArg3() { return fac; } typedef Real type3; void run() {  const int _sz = size; for (int i=0; i < _sz; i++) op(i, me,min,max,fac);  } Grid<Real>& me; Real min; Real max; Real fac;   };
// remap min/max range to hat function around (min+max)/2
void remapLsMask(Grid<Real>& phi, Real min, Real max) {
	Real fac = 0.;
	if ( fabs(max-min) > VECTOR_EPSILON ) fac = 1. / (max-min);
	knGridRemapLsMask(phi, min, max, fac);
} static PyObject* _W_0 (PyObject* _self, PyObject* _linargs, PyObject* _kwds) { try { PbArgs _args(_linargs, _kwds); FluidSolver *parent = _args.obtainParent(); pbPreparePlugin(parent, "remapLsMask" ); PyObject *_retval = 0; { ArgLocker _lock; Grid<Real>& phi = *_args.getPtr<Grid<Real> >("phi",0,&_lock); Real min = _args.get<Real >("min",1,&_lock); Real max = _args.get<Real >("max",2,&_lock);   _retval = getPyNone(); remapLsMask(phi,min,max);  _args.check(); } pbFinalizePlugin(parent,"remapLsMask" ); return _retval; } catch(std::exception& e) { pbSetError("remapLsMask",e.what()); return 0; } } static const Pb::Register _RP_remapLsMask ("","remapLsMask",_W_0); 

//! run marching cubes to create a mesh for the 0-levelset
void LevelsetGrid::createMesh(Mesh& mesh) {
	assertMsg(is3D(), "Only 3D grids supported so far");
	
	mesh.clear();
		
	const Real invalidTime = invalidTimeValue();
	const Real isoValue = 1e-4;
	
	// create some temp grids
	Grid<int> edgeVX(mParent);
	Grid<int> edgeVY(mParent);
	Grid<int> edgeVZ(mParent);
	
	for(int i=0; i<mSize.x-1; i++)
	for(int j=0; j<mSize.y-1; j++)
	for(int k=0; k<mSize.z-1; k++) {
		 Real value[8] = { get(i,j,k),   get(i+1,j,k),   get(i+1,j+1,k),   get(i,j+1,k),
						   get(i,j,k+1), get(i+1,j,k+1), get(i+1,j+1,k+1), get(i,j+1,k+1) };
		
		// build lookup index, check for invalid times
		bool skip = false;
		int cubeIdx = 0;
		for (int l=0;l<8;l++) {
			value[l] *= -1;
			if (-value[l] <= invalidTime)
				skip = true;
			if (value[l] < isoValue) 
				cubeIdx |= 1<<l;
		}
		if (skip || (mcEdgeTable[cubeIdx] == 0)) continue;
		
		// where to look up if this point already exists
		int triIndices[12];
		int *eVert[12] = { &edgeVX(i,j,k),   &edgeVY(i+1,j,k),   &edgeVX(i,j+1,k),   &edgeVY(i,j,k), 
						   &edgeVX(i,j,k+1), &edgeVY(i+1,j,k+1), &edgeVX(i,j+1,k+1), &edgeVY(i,j,k+1), 
						   &edgeVZ(i,j,k),   &edgeVZ(i+1,j,k),   &edgeVZ(i+1,j+1,k), &edgeVZ(i,j+1,k) };
		
		const Vec3 pos[9] = { Vec3(i,j,k),   Vec3(i+1,j,k),   Vec3(i+1,j+1,k),   Vec3(i,j+1,k),
						Vec3(i,j,k+1), Vec3(i+1,j,k+1), Vec3(i+1,j+1,k+1), Vec3(i,j+1,k+1) };
		
		for (int e=0; e<12; e++) {
			if (mcEdgeTable[cubeIdx] & (1<<e)) {
				// vertex already calculated ?
				if (*eVert[e] == 0) {
					// interpolate edge
					const int e1 = mcEdges[e*2  ];
					const int e2 = mcEdges[e*2+1];
					const Vec3 p1 = pos[ e1  ];    // scalar field pos 1
					const Vec3 p2 = pos[ e2  ];    // scalar field pos 2
					const float valp1  = value[ e1  ];  // scalar field val 1
					const float valp2  = value[ e2  ];  // scalar field val 2
					const float mu = (isoValue - valp1) / (valp2 - valp1);

					// init isolevel vertex
					Node vertex;
					vertex.pos = p1 + (p2-p1)*mu;
					vertex.normal = getNormalized( 
										getNormal( *this, i+cubieOffsetX[e1], j+cubieOffsetY[e1], k+cubieOffsetZ[e1]) * (1.0-mu) +
										getNormal( *this, i+cubieOffsetX[e2], j+cubieOffsetY[e2], k+cubieOffsetZ[e2]) * (    mu)) ;
					
					triIndices[e] = mesh.addNode(vertex) + 1;
					
					// store vertex 
					*eVert[e] = triIndices[e];
				} else {
					// retrieve  from vert array
					triIndices[e] = *eVert[e];
				}
			}
		}
		
		// Create the triangles... 
		for(int e=0; mcTriTable[cubeIdx][e]!=-1; e+=3) {
			mesh.addTri( Triangle( triIndices[ mcTriTable[cubeIdx][e+0]] - 1,
										triIndices[ mcTriTable[cubeIdx][e+1]] - 1,
										triIndices[ mcTriTable[cubeIdx][e+2]] - 1));
		}
	}
	
	//mesh.rebuildCorners();
	//mesh.rebuildLookup();
}


} //namespace

