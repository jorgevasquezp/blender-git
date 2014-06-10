#ifndef MANTA_H
#define MANTA_H
#include "FLUID_3D.h"
#include "zlib.h"
#include "../../../source/blender/makesdna/DNA_scene_types.h"
#include "../../../source/blender/makesdna/DNA_modifier_types.h"
#include "../../../source/blender/makesdna/DNA_smoke_types.h"
#include <sstream>
#include <fstream>

extern "C" bool manta_check_grid_size(struct FLUID_3D *fluid, int dimX, int dimY, int dimZ)
{
	if (!(dimX == fluid->xRes() && dimY == fluid->yRes() && dimZ == fluid->zRes())) {
		for (int cnt(0); cnt < fluid->_totalCells; cnt++)
			fluid->_density[cnt] = 0.0f;
		return false;
	}
	return true;
}

extern "C" void read_mantaflow_sim(struct FLUID_3D *fluid, char *name)
{
    /*! legacy headers for reading old files */
	typedef struct {
		int dimX, dimY, dimZ;
		int frames, elements, elementType, bytesPerElement, bytesPerFrame;
	} UniLegacyHeader;
	
	typedef struct {
		int dimX, dimY, dimZ;
		int gridType, elementType, bytesPerElement;
	} UniLegacyHeader2;
	
	/* uni file header - currently used */ 
	typedef struct {
		int dimX, dimY, dimZ;
		int gridType, elementType, bytesPerElement;
		char info[256]; /* mantaflow build information */
		unsigned long long timestamp; /* creation time */
	} UniHeader;
	
#	if NO_ZLIB!=1
    gzFile gzf = gzopen(name, "rb");
    if (!gzf) {
		for (int cnt(0); cnt < fluid->_totalCells; cnt++)
			fluid->_density[cnt] = 0.0f;
		return;
	}
	
    char ID[5] = {0,0,0,0,0};
	gzread(gzf, ID, 4);
	
	/* legacy file format */
    if (!strcmp(ID, "DDF2")) {
        UniLegacyHeader head;
		gzread(gzf, &head, sizeof(UniLegacyHeader));
		if (!manta_check_grid_size(fluid, head.dimX, head.dimY, head.dimZ))	return;
        int numEl = head.dimX*head.dimY*head.dimZ;
        gzseek(gzf, numEl, SEEK_CUR);
        /* actual grid read */
        gzread(gzf, fluid->_density, sizeof(float)*numEl);
    } 
	/* legacy file format 2 */
    else if (!strcmp(ID, "MNT1")) {
        UniLegacyHeader2 head;
        gzread(gzf, &head, sizeof(UniLegacyHeader2));
		if (!manta_check_grid_size(fluid, head.dimX, head.dimY, head.dimZ))	return;
        /* actual grid read*/
        gzread(gzf, fluid->_density, sizeof(float)*head.dimX*head.dimY*head.dimZ);
    }
	/* current file format*/
    else if (!strcmp(ID, "MNT2")) {
        UniHeader head;
        gzread(gzf, &head, sizeof(UniHeader));
		if (!manta_check_grid_size(fluid, head.dimX, head.dimY, head.dimZ))	return;
		/* actual grid read */
        gzread(gzf,fluid->_density, sizeof(float)*head.dimX*head.dimY*head.dimZ);
    }
    gzclose(gzf);

#	endif	/*zlib*/
 }

static void indent_ss(stringstream& ss, int indent)
{
	/*two-spaces indent*/
	if (indent < 0) return;
	std::string indentation = ""; 
	for (size_t cnt(0); cnt < indent; ++cnt) {
		indentation += "  ";
	}
	ss << indentation;
}

static void manta_gen_noise(stringstream& ss, char* solver, int indent, char *noise, int seed, bool load, bool clamp, int clampNeg, int clampPos, float valScale, float valOffset, float timeAnim)
{
	if (ss == NULL)/*should never be here*/
	{
		return;
	}
	indent_ss(ss, indent);
	ss << noise << " = "<<solver<<".create(NoiseField, fixedSeed=" << seed << ", loadFromFile="<< (load?"True":"False") <<") \n";
	ss << noise << ".posScale = vec3(20) \n";
	ss << noise << ".clamp = " << ((clamp)?"True":"False") << " \n";
	ss << noise << ".clampNeg = " << clampNeg << " \n";
	ss << noise << ".clampPos = " << clampPos << " \n";
	ss << noise << ".valScale = " << valScale << " \n";
	ss << noise << ".valOffset = " << valOffset << " \n";
	ss << noise << ".timeAnim = " << timeAnim << " \n";
}

static void manta_solve_pressure(stringstream& ss, char *flags, char *vel, char *pressure, bool useResNorms, int openBound, int solver_res,float cgMaxIterFac=1.0, float cgAccuracy = 0.01)
{
	/*open:0 ; vertical : 1; closed:2*/
	ss << "  solvePressure(flags=" << flags << ", vel=" << vel << ", pressure=" << pressure << ", useResNorm=" << (useResNorms?"True":"False") << ", openBound='";	
	
	if(openBound == 1) /*vertical*/
	{
		ss << "yY'";
	}
	else if (openBound == 0) /*open*/
	{
		if(solver_res == 2)
			ss << "xXyY";
		else
			ss << "xXyYzZ";
	}
		ss << "'";	/*empty for closed bounds*/ 
	
	ss << ", cgMaxIterFac=" << cgMaxIterFac << ", cgAccuracy=" << cgAccuracy << ") \n";
}

static void manta_advect_SemiLagr(stringstream& ss, int indent, char *flags, char *vel, char *grid, int order)
{
	if((order <=1) || (flags == NULL) || (vel == NULL) || (grid == NULL)){return;}
	indent_ss(ss, indent);
	ss << "advectSemiLagrange(flags=" << flags << ", vel=" << vel \
	<< ", grid=" << grid << ", order=" << order << ") \n"; 
}

/*create solver, handle 2D case*/
static void manta_create_solver(stringstream& ss, char *name, char *nick, char *grid_size_name, int x_res, int y_res, int z_res, int dim)
{
	if ((dim != 2) && (dim != 3))
	{ return; }
	if (dim == 2)
	{ z_res = 1; }
	ss << grid_size_name << " = vec3(" << x_res << ", " << y_res << ", " << z_res << ")" << " \n";
	ss << name << " = Solver(name = '" << nick << "', gridSize = " << grid_size_name << ", dim = " << dim << ") \n";
}

static void generate_manta_sim_file(Scene *scene, SmokeModifierData *smd)
{
	/*for now, simpleplume file creation
	*create python file with 2-spaces indentation*/
	
	bool wavelets = smd->domain->flags & MOD_SMOKE_HIGHRES;
	FLUID_3D *fluid = smd->domain->fluid;
	
	ofstream manta_setup_file;
	manta_setup_file.open("manta_scene.py", std::fstream::trunc);
	stringstream ss; /*setup contents*/
	
	/*header*/
	ss << "from manta import * \n";
	ss << "import os, shutil, math, sys \n";
	
/*Data Declaration*/
	/*Wavelets variables*/
	int upres = smd->domain->amplify;
	ss << "uvs = 1" << "\n";					/*TODO:add UI*/
	ss << "velInflow = vec3(2, 0, 0)"<< "\n";	/*TODO:add UI*/
	if (wavelets) {
		ss << "upres = " << upres << "\n";
		ss << "wltStrength = " << smd->domain->strength << "\n";
		if(upres > 0)/*TODO:add UI*/
		{	ss << "octaves = int( math.log(upres)/ math.log(2.0) + 0.5 ) \n";	}
		else
		{	ss << "octaves = 0"<< "\n";	}
	}
	else upres = 0;
		
	/*Solver Resolution*/
	ss << "res = " << smd->domain->maxres << " \n";
		/*Z axis in Blender = Y axis in Mantaflow*/
	manta_create_solver(ss, "s", "main", "gs", fluid->xRes(), fluid->zRes(), fluid->yRes(), smd->domain->manta_solver_res);
	ss << "s.timestep = " << smd->domain->time_scale << " \n";
		
/*Noise Field*/
	manta_gen_noise(ss, "s", 0, "noise", 256, true, true, 0, 2, 1, 0.075, 0.2);

/*Inflow source - for now, using mock sphere */
	ss << "source    = s.create(Cylinder, center=gs*vec3(0.3,0.2,0.5), radius=res*0.081, z=gs*vec3(0.081, 0, 0))\n";
	ss << "sourceVel = s.create(Cylinder, center=gs*vec3(0.3,0.2,0.5), radius=res*0.15 , z=gs*vec3(0.15 , 0, 0))\n";
	ss << "obs       = s.create(Sphere,   center=gs*vec3(0.5,0.5,0.5), radius=res*0.15)\n";	

/*Wavelets: larger solver*/
	if(wavelets && upres>0)
	{
		manta_create_solver(ss, "xl", "larger", "xl_gs", fluid->xRes() * upres, fluid->zRes()* upres, fluid->yRes() * upres, smd->domain->manta_solver_res);
		ss << "xl.timestep = " << smd->domain->time_scale << " \n";
		
		ss << "xl_vel = xl.create(MACGrid) \n";
		ss << "xl_density = xl.create(RealGrid) \n";/*smoke simulation*/
		ss << "xl_flags = xl.create(FlagGrid) \n";
		ss << "xl_flags.initDomain() \n";
		ss << "xl_flags.fillGrid() \n";
			
		ss << "xl_source = xl.create(Cylinder, center=xl_gs*vec3(0.3,0.2,0.5), radius=xl_gs.x*0.081, z=xl_gs*vec3(0.081, 0, 0)) \n";
		ss << "xl_obs    = xl.create(Sphere,   center=xl_gs*vec3(0.5,0.5,0.5), radius=xl_gs.x*0.15) \n";
		ss << "xl_obs.applyToGrid(grid=xl_flags, value=FlagObstacle) \n";
		manta_gen_noise(ss, "xl", 0, "xl_noise", 256, true, true, 0, 2, 1, 0.075, 0.2 * (float)upres);
	}
/*Flow setup*/
	ss << "flags = s.create(FlagGrid) \n";/*must always be present*/
	ss << "flags.initDomain() \n";
	ss << "flags.fillGrid() \n";
	ss << "obs.applyToGrid(grid=flags, value=FlagObstacle)\n";

	/*Create the array of UV grids*/
	if(wavelets){
		ss << "uv = [] \n";
		ss << "for i in range(uvs): \n";
		ss << "  uvGrid = s.create(VecGrid) \n";
		ss << "  uv.append(uvGrid) \n";
		ss << "  resetUvGrid( uv[i] ) \n";
	}
	/*Grids setup*/
	/*For now, only one grid of each kind is needed*/
	ss << "vel = s.create(MACGrid) \n";
	ss << "density = s.create(RealGrid) \n";/*smoke simulation*/
	ss << "pressure = s.create(RealGrid) \n";/*must always be present*/
	ss << "energy = s.create(RealGrid) \n";
	ss << "tempFlag  = s.create(FlagGrid)\n";

	/*Wavelets noise field*/
	if (wavelets)
	{
		ss << "xl_wltnoise = s.create(NoiseField, loadFromFile=True) \n";
		ss << "xl_wltnoise.posScale = vec3( int(1.0*gs.x) ) * 0.5 \n";
		/*scale according to lowres sim , smaller numbers mean larger vortices*/
		if (upres > 0){
			ss << "xl_wltnoise.posScale = xl_wltnoise.posScale * " << (1./(float)upres) << ((upres == 1)?". \n":"\n");
		}
		ss << "xl_wltnoise.timeAnim = 0.1 \n";
	}
	
/*GUI for debugging purposes*/
	ss << "if (GUI):\n  gui = Gui()\n  gui.show() \n";

/*Flow solving stepsv, main loop*/
	ss << "for t in xrange(" << scene->r.sfra << ", " << scene->r.efra << "): \n";
	manta_advect_SemiLagr(ss, 1, "flags", "vel", "density", 2);
	manta_advect_SemiLagr(ss, 1, "flags", "vel", "vel", 2);
	
	if(wavelets){
		ss << "  for i in range(uvs): \n";
		manta_advect_SemiLagr(ss, 2, "flags", "vel", "uv[i]", 2);
		ss << "    updateUvWeight( resetTime=16.5 , index=i, numUvs=uvs, uv=uv[i] )\n"; 
	}
	ss << "  applyInflow=False\n";
	ss << "  if (t>=0 and t<75):\n";
	ss << "    densityInflow( flags=flags, density=density, noise=noise, shape=source, scale=1, sigma=0.5 )\n";
	ss << "    sourceVel.applyToGrid( grid=vel , value=velInflow )\n";
	ss << "    applyInflow=True\n";
	
	ss << "  setWallBcs(flags=flags, vel=vel) \n";
	ss << "  addBuoyancy(density=density, vel=vel, gravity=vec3(0,-6e-4,0), flags=flags) \n";
	ss << "  vorticityConfinement( vel=vel, flags=flags, strength=0.4 ) \n";
	
	manta_solve_pressure(ss,"flags", "vel", "pressure",true,smd->domain->border_collisions, smd->domain->manta_solver_res,1.0,0.01);
	ss << "  setWallBcs(flags=flags, vel=vel) \n";

	/*determine weighting*/
	ss << "  computeEnergy(flags=flags, vel=vel, energy=energy)\n";
	/* mark outer obstacle region by extrapolating flags for 2 layers */
	ss << "  tempFlag.copyFrom(flags)\n";
	ss << "  extrapolateSimpleFlags( flags=flags, val=tempFlag, distance=2, flagFrom=FlagObstacle, flagTo=FlagFluid )\n";
	/*now extrapolate energy weights into obstacles to fix boundary layer*/
	ss << "  extrapolateSimpleFlags( flags=tempFlag, val=energy, distance=6, flagFrom=FlagFluid, flagTo=FlagObstacle )\n";
	ss << "  computeWaveletCoeffs(energy)\n";
/*Saving output*/
//	ss << "  density.save('den%04d.uni' % t) \n";
	ss << "  s.step()\n";
	ss << " \n";
	
	/**/
	if (wavelets && upres > 0)
	{
		ss << "  interpolateMACGrid( source=vel, target=xl_vel ) \n";
		/*add all necessary octaves*/
		ss << "  sStr = 1.0 * wltStrength  \n";
		ss << "  sPos = 2.0  \n";
		ss << "  for o in range(octaves): \n";
		ss << "    for i in range(uvs): \n";
		ss << "      uvWeight = getUvWeight(uv[i])  \n";
		ss << "      applyNoiseVec3( flags=xl_flags, target=xl_vel, noise=xl_wltnoise, scale=sStr * uvWeight, scaleSpatial=sPos , weight=energy, uv=uv[i] ) \n";
		ss << "    sStr *= 0.06 # magic kolmogorov factor \n";
		ss << "    sPos *= 2.0 \n";
		ss << "  for substep in range(upres):  \n";
		ss << "    advectSemiLagrange(flags=xl_flags, vel=xl_vel, grid=xl_density, order=2)  \n";
		ss << "  if (applyInflow): \n";
		ss << "    densityInflow( flags=xl_flags, density=xl_density, noise=xl_noise, shape=xl_source, scale=1, sigma=0.5 ) \n";
		ss << "  xl.step()   \n";
	}
	
	manta_setup_file << ss.rdbuf();
	manta_setup_file.close();		
}

#endif /* MANTA_H */
