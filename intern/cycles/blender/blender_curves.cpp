/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License
 */

#include "attribute.h"
#include "camera.h"
#include "curves.h"
#include "mesh.h"
#include "object.h"
#include "scene.h"

#include "blender_sync.h"
#include "blender_util.h"

#include "util_foreach.h"
#include "util_logging.h"

CCL_NAMESPACE_BEGIN

/* Utilities */

/* Hair curve functions */

void curveinterp_v3_v3v3v3v3(float3 *p, float3 *v1, float3 *v2, float3 *v3, float3 *v4, const float w[4]);
void interp_weights(float t, float data[4]);
float shaperadius(float shape, float root, float tip, float time);
void InterpolateKeySegments(int seg, int segno, int key, int curve, float3 *keyloc, float *time, ParticleCurveData *CData);
bool ObtainCacheParticleUV(Mesh *mesh, BL::Mesh *b_mesh, BL::Object *b_ob, ParticleCurveData *CData, bool background, int uv_num);
bool ObtainCacheParticleVcol(Mesh *mesh, BL::Mesh *b_mesh, BL::Object *b_ob, ParticleCurveData *CData, bool background, int vcol_num);
bool ObtainCacheParticleData(Mesh *mesh, BL::Mesh *b_mesh, BL::Object *b_ob, ParticleCurveData *CData, bool background);
void ExportParticleCurveTrianglePlanes(Mesh *mesh, ParticleCurveData *CData, float3 RotCam, bool is_ortho);
void ExportParticleCurveTriangleGeometry(Mesh *mesh, ParticleCurveData *CData, int resolution);
void ExportParticleCurveTriangleUV(Mesh *mesh, ParticleCurveData *CData, int vert_offset, int resol, float3 *uvdata);
void ExportParticleCurveTriangleVcol(Mesh *mesh, ParticleCurveData *CData, int vert_offset, int resol, uchar4 *cdata);

ParticleCurveData::ParticleCurveData()
{
}

ParticleCurveData::~ParticleCurveData()
{
}

void interp_weights(float t, float data[4])
{
	/* Cardinal curve interpolation */
	float t2 = t * t;
	float t3 = t2 * t;
	float fc = 0.71f;

	data[0] = -fc          * t3  + 2.0f * fc          * t2 - fc * t;
	data[1] =  (2.0f - fc) * t3  + (fc - 3.0f)        * t2 + 1.0f;
	data[2] =  (fc - 2.0f) * t3  + (3.0f - 2.0f * fc) * t2 + fc * t;
	data[3] =  fc          * t3  - fc * t2;
}

void curveinterp_v3_v3v3v3v3(float3 *p, float3 *v1, float3 *v2, float3 *v3, float3 *v4, const float w[4])
{
	p->x = v1->x * w[0] + v2->x * w[1] + v3->x * w[2] + v4->x * w[3];
	p->y = v1->y * w[0] + v2->y * w[1] + v3->y * w[2] + v4->y * w[3];
	p->z = v1->z * w[0] + v2->z * w[1] + v3->z * w[2] + v4->z * w[3];
}

float shaperadius(float shape, float root, float tip, float time)
{
	float radius = 1.0f - time;
	
	if(shape != 0.0f) {
		if(shape < 0.0f)
			radius = powf(radius, 1.0f + shape);
		else
			radius = powf(radius, 1.0f / (1.0f - shape));
	}
	return (radius * (root - tip)) + tip;
}

/* curve functions */

void InterpolateKeySegments(int seg, int segno, int key, int curve, float3 *keyloc, float *time, ParticleCurveData *CData)
{
	float3 ckey_loc1 = CData->curvekey_co[key];
	float3 ckey_loc2 = ckey_loc1;
	float3 ckey_loc3 = CData->curvekey_co[key+1];
	float3 ckey_loc4 = ckey_loc3;

	if(key > CData->curve_firstkey[curve])
		ckey_loc1 = CData->curvekey_co[key - 1];

	if(key < CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 2)
		ckey_loc4 = CData->curvekey_co[key + 2];

	float time1 = CData->curvekey_time[key]/CData->curve_length[curve];
	float time2 = CData->curvekey_time[key + 1]/CData->curve_length[curve];

	float dfra = (time2 - time1) / (float)segno;

	if(time)
		*time = (dfra * seg) + time1;

	float t[4];

	interp_weights((float)seg / (float)segno, t);

	if(keyloc)
		curveinterp_v3_v3v3v3v3(keyloc, &ckey_loc1, &ckey_loc2, &ckey_loc3, &ckey_loc4, t);
}

bool ObtainCacheParticleData(Mesh *mesh, BL::Mesh *b_mesh, BL::Object *b_ob, ParticleCurveData *CData, bool background)
{
	int curvenum = 0;
	int keyno = 0;

	if(!(mesh && b_mesh && b_ob && CData))
		return false;

	Transform tfm = get_transform(b_ob->matrix_world());
	Transform itfm = transform_quick_inverse(tfm);

	BL::Object::modifiers_iterator b_mod;
	for(b_ob->modifiers.begin(b_mod); b_mod != b_ob->modifiers.end(); ++b_mod) {
		if((b_mod->type() == b_mod->type_PARTICLE_SYSTEM) && (background ? b_mod->show_render() : b_mod->show_viewport())) {
			BL::ParticleSystemModifier psmd((const PointerRNA)b_mod->ptr);
			BL::ParticleSystem b_psys((const PointerRNA)psmd.particle_system().ptr);
			BL::ParticleSettings b_part((const PointerRNA)b_psys.settings().ptr);

			if((b_part.render_type() == BL::ParticleSettings::render_type_PATH) && (b_part.type() == BL::ParticleSettings::type_HAIR)) {
				int mi = clamp(b_part.material()-1, 0, mesh->used_shaders.size()-1);
				int shader = mesh->used_shaders[mi];
				int draw_step = background ? b_part.render_step() : b_part.draw_step();
				int ren_step = (int)powf(2.0f, (float)draw_step);
				int totparts = b_psys.particles.length();
				int totchild = background ? b_psys.child_particles.length() : (int)((float)b_psys.child_particles.length() * (float)b_part.draw_percentage() / 100.0f);
				int totcurves = totchild;
				
				if(b_part.child_type() == 0)
					totcurves += totparts;

				if(totcurves == 0)
					continue;

				PointerRNA cpsys = RNA_pointer_get(&b_part.ptr, "cycles");

				CData->psys_firstcurve.push_back(curvenum);
				CData->psys_curvenum.push_back(totcurves);
				CData->psys_shader.push_back(shader);

				float radius = get_float(cpsys, "radius_scale") * 0.5f;
	
				CData->psys_rootradius.push_back(radius * get_float(cpsys, "root_width"));
				CData->psys_tipradius.push_back(radius * get_float(cpsys, "tip_width"));
				CData->psys_shape.push_back(get_float(cpsys, "shape"));
				CData->psys_closetip.push_back(get_boolean(cpsys, "use_closetip"));

				int pa_no = 0;
				if(!(b_part.child_type() == 0))
					pa_no = totparts;

				int num_add = (totparts+totchild - pa_no);
				CData->curve_firstkey.reserve(CData->curve_firstkey.size() + num_add);
				CData->curve_keynum.reserve(CData->curve_keynum.size() + num_add);
				CData->curve_length.reserve(CData->curve_length.size() + num_add);
				CData->curvekey_co.reserve(CData->curvekey_co.size() + num_add*(ren_step+1));
				CData->curvekey_time.reserve(CData->curvekey_time.size() + num_add*(ren_step+1));

				for(; pa_no < totparts+totchild; pa_no++) {
					int keynum = 0;
					CData->curve_firstkey.push_back(keyno);
					
					float curve_length = 0.0f;
					float3 pcKey;
					for(int step_no = 0; step_no <= ren_step; step_no++) {
						float nco[3];
						b_psys.co_hair(*b_ob, pa_no, step_no, nco);
						float3 cKey = make_float3(nco[0], nco[1], nco[2]);
						cKey = transform_point(&itfm, cKey);
						if(step_no > 0) {
							float step_length = len(cKey - pcKey);
							if(step_length == 0.0f)
								continue;
							curve_length += step_length;
						}
						CData->curvekey_co.push_back(cKey);
						CData->curvekey_time.push_back(curve_length);
						pcKey = cKey;
						keyno++;
						keynum++;
					}

					CData->curve_keynum.push_back(keynum);
					CData->curve_length.push_back(curve_length);
					curvenum++;
				}
			}
		}
	}

	return true;
}

bool ObtainCacheParticleUV(Mesh *mesh, BL::Mesh *b_mesh, BL::Object *b_ob, ParticleCurveData *CData, bool background, int uv_num)
{
	if(!(mesh && b_mesh && b_ob && CData))
		return false;

	CData->curve_uv.clear();

	BL::Object::modifiers_iterator b_mod;
	for(b_ob->modifiers.begin(b_mod); b_mod != b_ob->modifiers.end(); ++b_mod) {
		if((b_mod->type() == b_mod->type_PARTICLE_SYSTEM) && (background ? b_mod->show_render() : b_mod->show_viewport())) {
			BL::ParticleSystemModifier psmd((const PointerRNA)b_mod->ptr);
			BL::ParticleSystem b_psys((const PointerRNA)psmd.particle_system().ptr);
			BL::ParticleSettings b_part((const PointerRNA)b_psys.settings().ptr);

			if((b_part.render_type() == BL::ParticleSettings::render_type_PATH) && (b_part.type() == BL::ParticleSettings::type_HAIR)) {
				int totparts = b_psys.particles.length();
				int totchild = background ? b_psys.child_particles.length() : (int)((float)b_psys.child_particles.length() * (float)b_part.draw_percentage() / 100.0f);
				int totcurves = totchild;
				
				if (b_part.child_type() == 0)
					totcurves += totparts;

				if (totcurves == 0)
					continue;

				int pa_no = 0;
				if(!(b_part.child_type() == 0))
					pa_no = totparts;

				int num_add = (totparts+totchild - pa_no);
				CData->curve_uv.reserve(CData->curve_uv.size() + num_add);

				BL::ParticleSystem::particles_iterator b_pa;
				b_psys.particles.begin(b_pa);
				for(; pa_no < totparts+totchild; pa_no++) {
					/* Add UVs */
					BL::Mesh::tessface_uv_textures_iterator l;
					b_mesh->tessface_uv_textures.begin(l);

					float3 uv = make_float3(0.0f, 0.0f, 0.0f);
					if(b_mesh->tessface_uv_textures.length())
						b_psys.uv_on_emitter(psmd, *b_pa, pa_no, uv_num, &uv.x);
					CData->curve_uv.push_back(uv);

					if(pa_no < totparts && b_pa != b_psys.particles.end())
						++b_pa;
				}
			}
		}
	}

	return true;
}

bool ObtainCacheParticleVcol(Mesh *mesh, BL::Mesh *b_mesh, BL::Object *b_ob, ParticleCurveData *CData, bool background, int vcol_num)
{
	if(!(mesh && b_mesh && b_ob && CData))
		return false;

	CData->curve_vcol.clear();

	BL::Object::modifiers_iterator b_mod;
	for(b_ob->modifiers.begin(b_mod); b_mod != b_ob->modifiers.end(); ++b_mod) {
		if((b_mod->type() == b_mod->type_PARTICLE_SYSTEM) && (background ? b_mod->show_render() : b_mod->show_viewport())) {
			BL::ParticleSystemModifier psmd((const PointerRNA)b_mod->ptr);
			BL::ParticleSystem b_psys((const PointerRNA)psmd.particle_system().ptr);
			BL::ParticleSettings b_part((const PointerRNA)b_psys.settings().ptr);

			if((b_part.render_type() == BL::ParticleSettings::render_type_PATH) && (b_part.type() == BL::ParticleSettings::type_HAIR)) {
				int totparts = b_psys.particles.length();
				int totchild = background ? b_psys.child_particles.length() : (int)((float)b_psys.child_particles.length() * (float)b_part.draw_percentage() / 100.0f);
				int totcurves = totchild;
				
				if (b_part.child_type() == 0)
					totcurves += totparts;

				if (totcurves == 0)
					continue;

				int pa_no = 0;
				if(!(b_part.child_type() == 0))
					pa_no = totparts;

				int num_add = (totparts+totchild - pa_no);
				CData->curve_vcol.reserve(CData->curve_vcol.size() + num_add);

				BL::ParticleSystem::particles_iterator b_pa;
				b_psys.particles.begin(b_pa);
				for(; pa_no < totparts+totchild; pa_no++) {
					/* Add vertex colors */
					BL::Mesh::tessface_vertex_colors_iterator l;
					b_mesh->tessface_vertex_colors.begin(l);

					float3 vcol = make_float3(0.0f, 0.0f, 0.0f);
					if(b_mesh->tessface_vertex_colors.length())
						b_psys.mcol_on_emitter(psmd, *b_pa, pa_no, vcol_num, &vcol.x);
					CData->curve_vcol.push_back(vcol);

					if(pa_no < totparts && b_pa != b_psys.particles.end())
						++b_pa;
				}
			}
		}
	}

	return true;
}

static void set_resolution(Mesh *mesh, BL::Mesh *b_mesh, BL::Object *b_ob, BL::Scene *scene, bool render)
{
	BL::Object::modifiers_iterator b_mod;
	for(b_ob->modifiers.begin(b_mod); b_mod != b_ob->modifiers.end(); ++b_mod) {
		if ((b_mod->type() == b_mod->type_PARTICLE_SYSTEM) && ((b_mod->show_viewport()) || (b_mod->show_render()))) {
			BL::ParticleSystemModifier psmd((const PointerRNA)b_mod->ptr);
			BL::ParticleSystem b_psys((const PointerRNA)psmd.particle_system().ptr);
			b_psys.set_resolution(*scene, *b_ob, (render)? 2: 1);
		}
	}
}

void ExportParticleCurveTrianglePlanes(Mesh *mesh, ParticleCurveData *CData, float3 RotCam, bool is_ortho)
{
	int vertexno = mesh->verts.size();
	int vertexindex = vertexno;
	int numverts = 0, numtris = 0;

	/* compute and reserve size of arrays */
	for(int sys = 0; sys < CData->psys_firstcurve.size() ; sys++) {
		for(int curve = CData->psys_firstcurve[sys]; curve < CData->psys_firstcurve[sys] + CData->psys_curvenum[sys] ; curve++) {
			if(CData->curve_keynum[curve] <= 1 || CData->curve_length[curve] == 0.0f)
				continue;

			numverts += 2 + (CData->curve_keynum[curve] - 1)*2;
			numtris += (CData->curve_keynum[curve] - 1)*2;
		}
	}

	mesh->verts.reserve(mesh->verts.size() + numverts);
	mesh->triangles.reserve(mesh->triangles.size() + numtris);
	mesh->shader.reserve(mesh->shader.size() + numtris);
	mesh->smooth.reserve(mesh->smooth.size() + numtris);

	/* actually export */
	for(int sys = 0; sys < CData->psys_firstcurve.size() ; sys++) {
		for(int curve = CData->psys_firstcurve[sys]; curve < CData->psys_firstcurve[sys] + CData->psys_curvenum[sys] ; curve++) {
			if(CData->curve_keynum[curve] <= 1 || CData->curve_length[curve] == 0.0f)
				continue;

			float3 xbasis;
			float3 v1;
			float time = 0.0f;
			float3 ickey_loc = CData->curvekey_co[CData->curve_firstkey[curve]];
			float radius = shaperadius(CData->psys_shape[sys], CData->psys_rootradius[sys], CData->psys_tipradius[sys], 0.0f);
			v1 = CData->curvekey_co[CData->curve_firstkey[curve] + 1] - CData->curvekey_co[CData->curve_firstkey[curve]];
			if(is_ortho)
				xbasis = normalize(cross(RotCam, v1));
			else
				xbasis = normalize(cross(RotCam - ickey_loc, v1));
			float3 ickey_loc_shfl = ickey_loc - radius * xbasis;
			float3 ickey_loc_shfr = ickey_loc + radius * xbasis;
			mesh->verts.push_back(ickey_loc_shfl);
			mesh->verts.push_back(ickey_loc_shfr);
			vertexindex += 2;

			for(int curvekey = CData->curve_firstkey[curve] + 1; curvekey < CData->curve_firstkey[curve] + CData->curve_keynum[curve]; curvekey++) {
				ickey_loc = CData->curvekey_co[curvekey];

				if(curvekey == CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 1)
					v1 = CData->curvekey_co[curvekey] - CData->curvekey_co[max(curvekey - 1, CData->curve_firstkey[curve])];
				else 
					v1 = CData->curvekey_co[curvekey + 1] - CData->curvekey_co[curvekey - 1];

				time = CData->curvekey_time[curvekey]/CData->curve_length[curve];
				radius = shaperadius(CData->psys_shape[sys], CData->psys_rootradius[sys], CData->psys_tipradius[sys], time);

				if(curvekey == CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 1)
					radius = shaperadius(CData->psys_shape[sys], CData->psys_rootradius[sys], CData->psys_tipradius[sys], 0.95f);

				if(CData->psys_closetip[sys] && (curvekey == CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 1))
					radius = shaperadius(CData->psys_shape[sys], CData->psys_rootradius[sys], 0.0f, 0.95f);

				if(is_ortho)
					xbasis = normalize(cross(RotCam, v1));
				else
					xbasis = normalize(cross(RotCam - ickey_loc, v1));
				float3 ickey_loc_shfl = ickey_loc - radius * xbasis;
				float3 ickey_loc_shfr = ickey_loc + radius * xbasis;
				mesh->verts.push_back(ickey_loc_shfl);
				mesh->verts.push_back(ickey_loc_shfr);
				mesh->add_triangle(vertexindex-2, vertexindex, vertexindex-1, CData->psys_shader[sys], true);
				mesh->add_triangle(vertexindex+1, vertexindex-1, vertexindex, CData->psys_shader[sys], true);
				vertexindex += 2;
			}
		}
	}

	mesh->reserve(mesh->verts.size(), mesh->triangles.size(), 0, 0);
	mesh->attributes.remove(ATTR_STD_VERTEX_NORMAL);
	mesh->attributes.remove(ATTR_STD_FACE_NORMAL);
	mesh->add_face_normals();
	mesh->add_vertex_normals();
	mesh->attributes.remove(ATTR_STD_FACE_NORMAL);

	/* texture coords still needed */
}

void ExportParticleCurveTriangleGeometry(Mesh *mesh, ParticleCurveData *CData, int resolution)
{
	int vertexno = mesh->verts.size();
	int vertexindex = vertexno;
	int numverts = 0, numtris = 0;

	/* compute and reserve size of arrays */
	for(int sys = 0; sys < CData->psys_firstcurve.size() ; sys++) {
		for(int curve = CData->psys_firstcurve[sys]; curve < CData->psys_firstcurve[sys] + CData->psys_curvenum[sys] ; curve++) {
			if(CData->curve_keynum[curve] <= 1 || CData->curve_length[curve] == 0.0f)
				continue;

			numverts += (CData->curve_keynum[curve] - 2)*2*resolution + resolution;
			numtris += (CData->curve_keynum[curve] - 2)*resolution;
		}
	}

	mesh->verts.reserve(mesh->verts.size() + numverts);
	mesh->triangles.reserve(mesh->triangles.size() + numtris);
	mesh->shader.reserve(mesh->shader.size() + numtris);
	mesh->smooth.reserve(mesh->smooth.size() + numtris);

	/* actually export */
	for(int sys = 0; sys < CData->psys_firstcurve.size() ; sys++) {
		for(int curve = CData->psys_firstcurve[sys]; curve < CData->psys_firstcurve[sys] + CData->psys_curvenum[sys] ; curve++) {
			if(CData->curve_keynum[curve] <= 1 || CData->curve_length[curve] == 0.0f)
				continue;

			float3 firstxbasis = cross(make_float3(1.0f,0.0f,0.0f),CData->curvekey_co[CData->curve_firstkey[curve]+1] - CData->curvekey_co[CData->curve_firstkey[curve]]);
			if(!is_zero(firstxbasis))
				firstxbasis = normalize(firstxbasis);
			else
				firstxbasis = normalize(cross(make_float3(0.0f,1.0f,0.0f),CData->curvekey_co[CData->curve_firstkey[curve]+1] - CData->curvekey_co[CData->curve_firstkey[curve]]));

			for(int curvekey = CData->curve_firstkey[curve]; curvekey < CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 1; curvekey++) {
				float3 xbasis = firstxbasis;
				float3 v1;
				float3 v2;

				if(curvekey == CData->curve_firstkey[curve]) {
					v1 = CData->curvekey_co[min(curvekey+2,CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 1)] - CData->curvekey_co[curvekey+1];
					v2 = CData->curvekey_co[curvekey+1] - CData->curvekey_co[curvekey];
				}
				else if(curvekey == CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 1) {
					v1 = CData->curvekey_co[curvekey] - CData->curvekey_co[curvekey-1];
					v2 = CData->curvekey_co[curvekey-1] - CData->curvekey_co[max(curvekey-2,CData->curve_firstkey[curve])];
				}
				else {
					v1 = CData->curvekey_co[curvekey+1] - CData->curvekey_co[curvekey];
					v2 = CData->curvekey_co[curvekey] - CData->curvekey_co[curvekey-1];
				}

				xbasis = cross(v1, v2);

				if(len_squared(xbasis) >= 0.05f * len_squared(v1) * len_squared(v2)) {
					firstxbasis = normalize(xbasis);
					break;
				}
			}

			for(int curvekey = CData->curve_firstkey[curve]; curvekey < CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 1; curvekey++) {
				int subv = 1;
				float3 xbasis;
				float3 ybasis;
				float3 v1;
				float3 v2;

				if(curvekey == CData->curve_firstkey[curve]) {
					subv = 0;
					v1 = CData->curvekey_co[min(curvekey+2,CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 1)] - CData->curvekey_co[curvekey+1];
					v2 = CData->curvekey_co[curvekey+1] - CData->curvekey_co[curvekey];
				}
				else if(curvekey == CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 1) {
					v1 = CData->curvekey_co[curvekey] - CData->curvekey_co[curvekey-1];
					v2 = CData->curvekey_co[curvekey-1] - CData->curvekey_co[max(curvekey-2,CData->curve_firstkey[curve])];
				}
				else {
					v1 = CData->curvekey_co[curvekey+1] - CData->curvekey_co[curvekey];
					v2 = CData->curvekey_co[curvekey] - CData->curvekey_co[curvekey-1];
				}

				xbasis = cross(v1, v2);

				if(len_squared(xbasis) >= 0.05f * len_squared(v1) * len_squared(v2)) {
					xbasis = normalize(xbasis);
					firstxbasis = xbasis;
				}
				else
					xbasis = firstxbasis;

				ybasis = normalize(cross(xbasis, v2));

				for (; subv <= 1; subv++) {
					float3 ickey_loc = make_float3(0.0f,0.0f,0.0f);
					float time = 0.0f;

					InterpolateKeySegments(subv, 1, curvekey, curve, &ickey_loc, &time, CData);

					float radius = shaperadius(CData->psys_shape[sys], CData->psys_rootradius[sys], CData->psys_tipradius[sys], time);

					if((curvekey == CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 2) && (subv == 1))
						radius = shaperadius(CData->psys_shape[sys], CData->psys_rootradius[sys], CData->psys_tipradius[sys], 0.95f);

					if(CData->psys_closetip[sys] && (subv == 1) && (curvekey == CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 2))
						radius = shaperadius(CData->psys_shape[sys], CData->psys_rootradius[sys], 0.0f, 0.95f);

					float angle = M_2PI_F / (float)resolution;
					for(int section = 0; section < resolution; section++) {
						float3 ickey_loc_shf = ickey_loc + radius * (cosf(angle * section) * xbasis + sinf(angle * section) * ybasis);
						mesh->verts.push_back(ickey_loc_shf);
					}

					if(subv != 0) {
						for(int section = 0; section < resolution - 1; section++) {
							mesh->add_triangle(vertexindex - resolution + section, vertexindex + section, vertexindex - resolution + section + 1, CData->psys_shader[sys], true);
							mesh->add_triangle(vertexindex + section + 1, vertexindex - resolution + section + 1, vertexindex + section, CData->psys_shader[sys], true);
						}
						mesh->add_triangle(vertexindex-1, vertexindex + resolution - 1, vertexindex - resolution, CData->psys_shader[sys], true);
						mesh->add_triangle(vertexindex, vertexindex - resolution , vertexindex + resolution - 1, CData->psys_shader[sys], true);
					}
					vertexindex += resolution;
				}
			}
		}
	}

	mesh->reserve(mesh->verts.size(), mesh->triangles.size(), 0, 0);
	mesh->attributes.remove(ATTR_STD_VERTEX_NORMAL);
	mesh->attributes.remove(ATTR_STD_FACE_NORMAL);
	mesh->add_face_normals();
	mesh->add_vertex_normals();
	mesh->attributes.remove(ATTR_STD_FACE_NORMAL);

	/* texture coords still needed */
}

static void ExportParticleCurveSegments(Scene *scene, Mesh *mesh, ParticleCurveData *CData)
{
	VLOG(1) << "Exporting curve segments for mesh " << mesh->name;
	int num_keys = 0;
	int num_curves = 0;

	if(!(mesh->curves.empty() && mesh->curve_keys.empty()))
		return;

	Attribute *attr_intercept = NULL;
	
	if(mesh->need_attribute(scene, ATTR_STD_CURVE_INTERCEPT))
		attr_intercept = mesh->curve_attributes.add(ATTR_STD_CURVE_INTERCEPT);

	/* compute and reserve size of arrays */
	for(int sys = 0; sys < CData->psys_firstcurve.size() ; sys++) {
		for(int curve = CData->psys_firstcurve[sys]; curve < CData->psys_firstcurve[sys] + CData->psys_curvenum[sys] ; curve++) {
			if(CData->curve_keynum[curve] <= 1 || CData->curve_length[curve] == 0.0f)
				continue;

			num_keys += CData->curve_keynum[curve];
			num_curves++;
		}
	}

	mesh->curve_keys.reserve(mesh->curve_keys.size() + num_keys);
	mesh->curves.reserve(mesh->curves.size() + num_curves);

	num_keys = 0;
	num_curves = 0;

	/* actually export */
	for(int sys = 0; sys < CData->psys_firstcurve.size() ; sys++) {
		for(int curve = CData->psys_firstcurve[sys]; curve < CData->psys_firstcurve[sys] + CData->psys_curvenum[sys] ; curve++) {
			if(CData->curve_keynum[curve] <= 1 || CData->curve_length[curve] == 0.0f)
				continue;

			size_t num_curve_keys = 0;

			for(int curvekey = CData->curve_firstkey[curve]; curvekey < CData->curve_firstkey[curve] + CData->curve_keynum[curve]; curvekey++) {
				float3 ickey_loc = CData->curvekey_co[curvekey];
				float time = CData->curvekey_time[curvekey]/CData->curve_length[curve];
				float radius = shaperadius(CData->psys_shape[sys], CData->psys_rootradius[sys], CData->psys_tipradius[sys], time);

				if(CData->psys_closetip[sys] && (curvekey == CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 1))
					radius = 0.0f;

				mesh->add_curve_key(ickey_loc, radius);
				if(attr_intercept)
					attr_intercept->add(time);

				num_curve_keys++;
			}

			mesh->add_curve(num_keys, num_curve_keys, CData->psys_shader[sys]);
			num_keys += num_curve_keys;
			num_curves++;
		}
	}

	/* check allocation*/
	if((mesh->curve_keys.size() !=  num_keys) || (mesh->curves.size() !=  num_curves)) {
		/* allocation failed -> clear data */
		mesh->curve_keys.clear();
		mesh->curves.clear();
		mesh->curve_attributes.clear();
	}
}

static void ExportParticleCurveSegmentsMotion(Scene *scene, Mesh *mesh, ParticleCurveData *CData, int time_index)
{
	VLOG(1) << "Exporting curve motion segments for mesh " << mesh->name
	        << ", time index " << time_index;

	/* find attribute */
	Attribute *attr_mP = mesh->curve_attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);
	bool new_attribute = false;

	/* add new attribute if it doesn't exist already */
	if(!attr_mP) {
		VLOG(1) << "Creating new motion vertex position attribute";
		attr_mP = mesh->curve_attributes.add(ATTR_STD_MOTION_VERTEX_POSITION);
		new_attribute = true;
	}

	/* export motion vectors for curve keys */
	size_t numkeys = mesh->curve_keys.size();
	float4 *mP = attr_mP->data_float4() + time_index*numkeys;
	bool have_motion = false;
	int i = 0;

	for(int sys = 0; sys < CData->psys_firstcurve.size(); sys++) {
		if(CData->psys_curvenum[sys] == 0)
			continue;

		for(int curve = CData->psys_firstcurve[sys]; curve < CData->psys_firstcurve[sys] + CData->psys_curvenum[sys]; curve++) {
			if(CData->curve_keynum[curve] <= 1 || CData->curve_length[curve] == 0.0f)
				continue;

			for(int curvekey = CData->curve_firstkey[curve]; curvekey < CData->curve_firstkey[curve] + CData->curve_keynum[curve]; curvekey++) {
				if(i < mesh->curve_keys.size()) {
					float3 ickey_loc = CData->curvekey_co[curvekey];
					float time = CData->curvekey_time[curvekey]/CData->curve_length[curve];
					float radius = shaperadius(CData->psys_shape[sys], CData->psys_rootradius[sys], CData->psys_tipradius[sys], time);

					if(CData->psys_closetip[sys] && (curvekey == CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 1))
						radius = 0.0f;

					mP[i] = float3_to_float4(ickey_loc);
					mP[i].w = radius;

					/* unlike mesh coordinates, these tend to be slightly different
					 * between frames due to particle transforms into/out of object
					 * space, so we use an epsilon to detect actual changes */
					if(len_squared(mP[i] - mesh->curve_keys[i]) > 1e-5f*1e-5f)
						have_motion = true;
				}

				i++;
			}
		}
	}

	/* in case of new attribute, we verify if there really was any motion */
	if(new_attribute) {
		if(i != numkeys || !have_motion) {
			/* no motion, remove attributes again */
			VLOG(1) << "No motion, removing attribute";
			mesh->curve_attributes.remove(ATTR_STD_MOTION_VERTEX_POSITION);
		}
		else if(time_index > 0) {
			VLOG(1) << "Filling in new motion vertex position for time_index"
			        << time_index;
			/* motion, fill up previous steps that we might have skipped because
			 * they had no motion, but we need them anyway now */
			for(int step = 0; step < time_index; step++) {
				float4 *mP = attr_mP->data_float4() + step*numkeys;

				for(int key = 0; key < numkeys; key++)
					mP[key] = mesh->curve_keys[key];
			}
		}
	}
}

static void hair_step_iter_eval(BL::HairRenderStepIterator b_step_iter, float3 &co, float3 frame[3], float &param)
{
	float fco[3], fnor[3], ftan[3], fcotan[3], fhair_radius;
	
	param = b_step_iter.parameter();
	
	b_step_iter.eval(fco, &fhair_radius);
	co = make_float3(fco[0], fco[1], fco[2]);
	
	b_step_iter.eval_frame(fnor, ftan, fcotan);
	frame[0] = make_float3(fnor[0], fnor[1], fnor[2]);
	frame[1] = make_float3(ftan[0], ftan[1], ftan[2]);
	frame[2] = make_float3(fcotan[0], fcotan[1], fcotan[2]);
}

/* evaluate curve at each step:
 *   v_p, v, v_n:       previous/current/next location
 *   d, d_n:            current/next direction
 *   param, param_n:    current/next curve parameter
 */
static void hair_curve_triangle_step(int step, BL::HairRenderStepIterator b_step_iter, float3 &v_p, float3 &v, float3 &v_n, float3 &d, float3 &d_n,
                                     float &param, float &param_n, float3 frame[3], float3 frame_n[3])
{
	if (step == 0) {
		hair_step_iter_eval(b_step_iter, v, frame, param);
		v_p         = v;
		b_step_iter.next();
		
		hair_step_iter_eval(b_step_iter, v_n, frame_n, param_n);
		d = d_n     = v_n - v;
		b_step_iter.next();
	}
	else if (step < b_step_iter.totsteps() - 1) {
		v_p         = v;
		v           = v_n;
		param       = param_n;
		d           = d_n;
		
		hair_step_iter_eval(b_step_iter, v_n, frame_n, param_n);
		d_n         = v_n - v;
		b_step_iter.next();
	}
	else {
		param       = param_n;
		v_p         = v;
		v           = v_n;
		d           = d_n;
	}
}

static void ExportHairCurveTrianglePlanes(Mesh *mesh, BL::HairSystem b_hsys, float3 RotCam, bool is_ortho)
{
	BL::HairRenderSettings b_render(b_hsys.params().render());
	float shape = b_render.shape();
	float root_width = b_render.root_width() * b_render.radius_scale();
	float tip_width = b_render.tip_width() * b_render.radius_scale();
	bool closetip = b_render.use_closetip();
	int shader = mesh->used_shaders[clamp(b_render.material_slot()-1, 0, mesh->used_shaders.size()-1)];
	
	int vertexno = mesh->verts.size();
	int vertexindex = vertexno;
	int numverts = 0, numtris = 0;
	
	BL::HairRenderIterator b_hair_iter = b_hsys.render_iterator();
	b_hair_iter.init();
	
	/* compute and reserve size of arrays */
	int num_curves, num_keys;
	b_hair_iter.count(&num_curves, &num_keys);
	numverts += 2 + (num_keys - 1)*2;
	numtris += (num_keys - 1)*2;
	
	mesh->verts.reserve(mesh->verts.size() + numverts);
	mesh->triangles.reserve(mesh->triangles.size() + numtris);
	mesh->shader.reserve(mesh->shader.size() + numtris);
	mesh->smooth.reserve(mesh->smooth.size() + numtris);
	
	/* actually export */
	for (; b_hair_iter.valid(); b_hair_iter.next()) {
		BL::HairRenderStepIterator b_step_iter = b_hair_iter.step_init();
		int totsteps = b_step_iter.totsteps();
		if (totsteps <= 1)
			continue;
		
		float3 v_p, v, v_n, d, d_n, frame[3], frame_n[3];
		float param, param_n;
		
		for (int step = 0; step < totsteps; ++step) {
			hair_curve_triangle_step(step, b_step_iter, v_p, v, v_n, d, d_n, param, param_n, frame, frame_n);
			
			float radius = shaperadius(shape, root_width, tip_width, param);
			if (closetip && step == totsteps - 1)
				radius = 0.0f;
			
			float3 ickey_loc = v;
			float3 xbasis;
			if(is_ortho)
				xbasis = normalize(cross(RotCam, d));
			else
				xbasis = normalize(cross(RotCam - ickey_loc, d));
			float3 ickey_loc_shfl = ickey_loc - radius * xbasis;
			float3 ickey_loc_shfr = ickey_loc + radius * xbasis;
			mesh->verts.push_back(ickey_loc_shfl);
			mesh->verts.push_back(ickey_loc_shfr);
			if (step > 0) {
				mesh->add_triangle(vertexindex-2, vertexindex, vertexindex-1, shader, true);
				mesh->add_triangle(vertexindex+1, vertexindex-1, vertexindex, shader, true);
			}
			vertexindex += 2;
		}
	}
	b_hair_iter.end();

	mesh->reserve(mesh->verts.size(), mesh->triangles.size(), 0, 0);
	mesh->attributes.remove(ATTR_STD_VERTEX_NORMAL);
	mesh->attributes.remove(ATTR_STD_FACE_NORMAL);
	mesh->add_face_normals();
	mesh->add_vertex_normals();
	mesh->attributes.remove(ATTR_STD_FACE_NORMAL);

	/* texture coords still needed */
}

static void ExportHairCurveTriangleGeometry(Mesh *mesh, BL::HairSystem b_hsys, int resolution)
{
	BL::HairRenderSettings b_render(b_hsys.params().render());
	float shape = b_render.shape();
	float root_width = b_render.root_width() * b_render.radius_scale();
	float tip_width = b_render.tip_width() * b_render.radius_scale();
	bool closetip = b_render.use_closetip();
	int shader = mesh->used_shaders[clamp(b_render.material_slot()-1, 0, mesh->used_shaders.size()-1)];
	
	int vertexno = mesh->verts.size();
	int vertexindex = vertexno;
	int numverts = 0, numtris = 0;
	
	BL::HairRenderIterator b_hair_iter = b_hsys.render_iterator();
	b_hair_iter.init();
	
	/* compute and reserve size of arrays */
	int num_curves, num_keys;
	b_hair_iter.count(&num_curves, &num_keys);
	numverts += (num_keys - 2)*2*resolution + resolution;
	numtris += (num_keys - 2)*resolution;
	
	mesh->verts.reserve(mesh->verts.size() + numverts);
	mesh->triangles.reserve(mesh->triangles.size() + numtris);
	mesh->shader.reserve(mesh->shader.size() + numtris);
	mesh->smooth.reserve(mesh->smooth.size() + numtris);
	
	/* actually export */
	for (; b_hair_iter.valid(); b_hair_iter.next()) {
		BL::HairRenderStepIterator b_step_iter = b_hair_iter.step_init();
		int totsteps = b_step_iter.totsteps();
		if (totsteps <= 1)
			continue;
		
		float3 v_p, v, v_n, d, d_n, frame[3], frame_n[3];
		float param, param_n;
		
		for (int step = 0; step < totsteps; ++step) {
			hair_curve_triangle_step(step, b_step_iter, v_p, v, v_n, d, d_n, param, param_n, frame, frame_n);
			
			float radius = shaperadius(shape, root_width, tip_width, param);
			if (closetip && step == totsteps - 1)
				radius = 0.0f;
			
			float3 xbasis = frame[1];
			float3 ybasis = frame[2];
			
			float3 ickey_loc = v;
			
			float angle = M_2PI_F / (float)resolution;
			for(int section = 0; section < resolution; section++) {
				float3 ickey_loc_shf = ickey_loc + radius * (cosf(angle * section) * xbasis + sinf(angle * section) * ybasis);
				mesh->verts.push_back(ickey_loc_shf);
			}
			
			if(step > 0) {
				for(int section = 0; section < resolution - 1; section++) {
					mesh->add_triangle(vertexindex - resolution + section, vertexindex + section, vertexindex - resolution + section + 1, shader, true);
					mesh->add_triangle(vertexindex + section + 1, vertexindex - resolution + section + 1, vertexindex + section, shader, true);
				}
				mesh->add_triangle(vertexindex-1, vertexindex + resolution - 1, vertexindex - resolution, shader, true);
				mesh->add_triangle(vertexindex, vertexindex - resolution , vertexindex + resolution - 1, shader, true);
			}
			vertexindex += resolution;
		}
	}
	b_hair_iter.end();

	mesh->reserve(mesh->verts.size(), mesh->triangles.size(), 0, 0);
	mesh->attributes.remove(ATTR_STD_VERTEX_NORMAL);
	mesh->attributes.remove(ATTR_STD_FACE_NORMAL);
	mesh->add_face_normals();
	mesh->add_vertex_normals();
	mesh->attributes.remove(ATTR_STD_FACE_NORMAL);

	/* texture coords still needed */
}

static void ExportHairCurveSegments(Scene *scene, Mesh *mesh, BL::HairSystem b_hsys)
{
	BL::HairRenderSettings b_render(b_hsys.params().render());
	float shape = b_render.shape();
	float root_width = b_render.root_width() * b_render.radius_scale();
	float tip_width = b_render.tip_width() * b_render.radius_scale();
	bool closetip = b_render.use_closetip();
	int shader = mesh->used_shaders[clamp(b_render.material_slot()-1, 0, mesh->used_shaders.size()-1)];
	
	int num_keys = 0;
	int num_curves = 0;
	
	if(!(mesh->curves.empty() && mesh->curve_keys.empty()))
		return;
	
	Attribute *attr_intercept = NULL;
	
	if(mesh->need_attribute(scene, ATTR_STD_CURVE_INTERCEPT))
		attr_intercept = mesh->curve_attributes.add(ATTR_STD_CURVE_INTERCEPT);
	
	BL::HairRenderIterator b_hair_iter = b_hsys.render_iterator();
	b_hair_iter.init();
	
	/* compute and reserve size of arrays */
	b_hair_iter.count(&num_curves, &num_keys);
	mesh->curve_keys.reserve(mesh->curve_keys.size() + num_keys);
	mesh->curves.reserve(mesh->curves.size() + num_curves);
	
	num_keys = 0;
	num_curves = 0;
	for (; b_hair_iter.valid(); b_hair_iter.next()) {
		BL::HairRenderStepIterator b_step_iter = b_hair_iter.step_init();
		if (b_step_iter.totsteps() <= 1)
			continue;
		
		int num_curve_keys = 0;
		for (; b_step_iter.valid(); b_step_iter.next()) {
			float param = b_step_iter.parameter();
			float radius = shaperadius(shape, root_width, tip_width, param);
			if (closetip && b_step_iter.index() == b_step_iter.totsteps() - 1)
				radius = 0.0f;
			
			float co[3], hair_radius;
			b_step_iter.eval(co, &hair_radius);
			
			mesh->add_curve_key(make_float3(co[0], co[1], co[2]), radius);
			if(attr_intercept)
				attr_intercept->add(param);
			
			++num_curve_keys;
		}
		
		mesh->add_curve(num_keys, num_curve_keys, shader);
		num_keys += num_curve_keys;
		num_curves++;
	}
	b_hair_iter.end();
	
	/* check allocation*/
	if((mesh->curve_keys.size() !=  num_keys) || (mesh->curves.size() !=  num_curves)) {
		/* allocation failed -> clear data */
		mesh->curve_keys.clear();
		mesh->curves.clear();
		mesh->curve_attributes.clear();
	}
}

static void ExportHairCurveSegmentsMotion(Scene *scene, Mesh *mesh, BL::HairSystem b_hsys, int time_index)
{
	BL::HairRenderSettings b_render(b_hsys.params().render());
	float shape = b_render.shape();
	float root_width = b_render.root_width() * b_render.radius_scale();
	float tip_width = b_render.tip_width() * b_render.radius_scale();
	bool closetip = b_render.use_closetip();
	
	/* find attribute */
	Attribute *attr_mP = mesh->curve_attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);
	bool new_attribute = false;
	
	/* add new attribute if it doesn't exist already */
	if(!attr_mP) {
		attr_mP = mesh->curve_attributes.add(ATTR_STD_MOTION_VERTEX_POSITION);
		new_attribute = true;
	}
	
	/* export motion vectors for curve keys */
	size_t numkeys = mesh->curve_keys.size();
	float4 *mP = attr_mP->data_float4() + time_index * numkeys;
	bool have_motion = false;
	int i = 0;
	
	BL::HairRenderIterator b_hair_iter = b_hsys.render_iterator();
	b_hair_iter.init();
	
	for (; b_hair_iter.valid(); b_hair_iter.next()) {
		BL::HairRenderStepIterator b_step_iter = b_hair_iter.step_init();
		if (b_step_iter.totsteps() <= 1)
			continue;
		
		for (; b_step_iter.valid(); b_step_iter.next()) {
			if(i < mesh->curve_keys.size()) {
				float param = b_step_iter.parameter();
				float radius = shaperadius(shape, root_width, tip_width, param);
				if (closetip && b_step_iter.index() == b_step_iter.totsteps() - 1)
					radius = 0.0f;
				
				float co[3], hair_radius;
				b_step_iter.eval(co, &hair_radius);
				
				mP[i].x = co[0];
				mP[i].y = co[1];
				mP[i].z = co[2];
				mP[i].w = radius;
				
				/* unlike mesh coordinates, these tend to be slightly different
				 * between frames due to particle transforms into/out of object
				 * space, so we use an epsilon to detect actual changes */
				if(len_squared(mP[i] - mesh->curve_keys[i]) > 1e-5f*1e-5f)
					have_motion = true;
			}
			
			i++;
		}
	}
	
	/* in case of new attribute, we verify if there really was any motion */
	if(new_attribute) {
		if(i != numkeys || !have_motion) {
			/* no motion, remove attributes again */
			mesh->curve_attributes.remove(ATTR_STD_MOTION_VERTEX_POSITION);
		}
		else if(time_index > 0) {
			/* motion, fill up previous steps that we might have skipped because
			 * they had no motion, but we need them anyway now */
			for(int step = 0; step < time_index; step++) {
				float4 *mP = attr_mP->data_float4() + step * numkeys;
				
				for(int key = 0; key < numkeys; key++)
					mP[key] = mesh->curve_keys[key];
			}
		}
	}
}

void ExportParticleCurveTriangleUV(Mesh *mesh, ParticleCurveData *CData, int vert_offset, int resol, float3 *uvdata)
{
	if(uvdata == NULL)
		return;

	float time = 0.0f;
	float prevtime = 0.0f;

	int vertexindex = vert_offset;

	for(int sys = 0; sys < CData->psys_firstcurve.size() ; sys++) {
		for(int curve = CData->psys_firstcurve[sys]; curve < CData->psys_firstcurve[sys] + CData->psys_curvenum[sys] ; curve++) {
			if(CData->curve_keynum[curve] <= 1 || CData->curve_length[curve] == 0.0f)
				continue;

			for(int curvekey = CData->curve_firstkey[curve]; curvekey < CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 1; curvekey++) {
				time = CData->curvekey_time[curvekey]/CData->curve_length[curve];

				for(int section = 0; section < resol; section++) {
					uvdata[vertexindex] = CData->curve_uv[curve];
					uvdata[vertexindex].z = prevtime;
					vertexindex++;
					uvdata[vertexindex] = CData->curve_uv[curve];
					uvdata[vertexindex].z = time;
					vertexindex++;
					uvdata[vertexindex] = CData->curve_uv[curve];
					uvdata[vertexindex].z = prevtime;
					vertexindex++;
					uvdata[vertexindex] = CData->curve_uv[curve];
					uvdata[vertexindex].z = time;
					vertexindex++;
					uvdata[vertexindex] = CData->curve_uv[curve];
					uvdata[vertexindex].z = prevtime;
					vertexindex++;
					uvdata[vertexindex] = CData->curve_uv[curve];
					uvdata[vertexindex].z = time;
					vertexindex++;
				}

				prevtime = time;
			}
		}
	}
}

void ExportParticleCurveTriangleVcol(Mesh *mesh, ParticleCurveData *CData, int vert_offset, int resol, uchar4 *cdata)
{
	if(cdata == NULL)
		return;

	int vertexindex = vert_offset;

	for(int sys = 0; sys < CData->psys_firstcurve.size() ; sys++) {
		for(int curve = CData->psys_firstcurve[sys]; curve < CData->psys_firstcurve[sys] + CData->psys_curvenum[sys] ; curve++) {
			if(CData->curve_keynum[curve] <= 1 || CData->curve_length[curve] == 0.0f)
				continue;

			for(int curvekey = CData->curve_firstkey[curve]; curvekey < CData->curve_firstkey[curve] + CData->curve_keynum[curve] - 1; curvekey++) {
				for(int section = 0; section < resol; section++) {
					cdata[vertexindex] = color_float_to_byte(color_srgb_to_scene_linear(CData->curve_vcol[curve]));
					vertexindex++;
					cdata[vertexindex] = color_float_to_byte(color_srgb_to_scene_linear(CData->curve_vcol[curve]));
					vertexindex++;
					cdata[vertexindex] = color_float_to_byte(color_srgb_to_scene_linear(CData->curve_vcol[curve]));
					vertexindex++;
					cdata[vertexindex] = color_float_to_byte(color_srgb_to_scene_linear(CData->curve_vcol[curve]));
					vertexindex++;
					cdata[vertexindex] = color_float_to_byte(color_srgb_to_scene_linear(CData->curve_vcol[curve]));
					vertexindex++;
					cdata[vertexindex] = color_float_to_byte(color_srgb_to_scene_linear(CData->curve_vcol[curve]));
					vertexindex++;
				}
			}
		}
	}
}

/* Hair Curve Sync */

void BlenderSync::sync_curve_settings()
{
	PointerRNA csscene = RNA_pointer_get(&b_scene.ptr, "cycles_curves");

	CurveSystemManager *curve_system_manager = scene->curve_system_manager;
	CurveSystemManager prev_curve_system_manager = *curve_system_manager;

	curve_system_manager->use_curves = get_boolean(csscene, "use_curves");
	curve_system_manager->minimum_width = get_float(csscene, "minimum_width");
	curve_system_manager->maximum_width = get_float(csscene, "maximum_width");

	curve_system_manager->primitive = get_enum(csscene, "primitive");
	curve_system_manager->curve_shape = get_enum(csscene, "shape");
	curve_system_manager->resolution = get_int(csscene, "resolution");
	curve_system_manager->subdivisions = get_int(csscene, "subdivisions");
	curve_system_manager->use_backfacing = !get_boolean(csscene, "cull_backfacing");

	if(curve_system_manager->primitive == CURVE_TRIANGLES && curve_system_manager->curve_shape == CURVE_RIBBON) {
		/* camera facing planes */
		curve_system_manager->triangle_method = CURVE_CAMERA_TRIANGLES;
		curve_system_manager->resolution = 1;
	}
	else if(curve_system_manager->primitive == CURVE_TRIANGLES && curve_system_manager->curve_shape == CURVE_THICK) {
		/* camera facing planes */
		curve_system_manager->triangle_method = CURVE_TESSELATED_TRIANGLES;
	}
	else if(curve_system_manager->primitive == CURVE_LINE_SEGMENTS && curve_system_manager->curve_shape == CURVE_RIBBON) {
		/* tangent shading */
		curve_system_manager->line_method = CURVE_UNCORRECTED;
		curve_system_manager->use_encasing = true;
		curve_system_manager->use_backfacing = false;
		curve_system_manager->use_tangent_normal_geometry = true;
	}
	else if(curve_system_manager->primitive == CURVE_LINE_SEGMENTS && curve_system_manager->curve_shape == CURVE_THICK) {
		curve_system_manager->line_method = CURVE_ACCURATE;
		curve_system_manager->use_encasing = false;
		curve_system_manager->use_tangent_normal_geometry = false;
	}
	else if(curve_system_manager->primitive == CURVE_SEGMENTS && curve_system_manager->curve_shape == CURVE_RIBBON) {
		curve_system_manager->primitive = CURVE_RIBBONS;
		curve_system_manager->use_backfacing = false;
	}

	if(curve_system_manager->modified_mesh(prev_curve_system_manager)) {
		BL::BlendData::objects_iterator b_ob;

		for(b_data.objects.begin(b_ob); b_ob != b_data.objects.end(); ++b_ob) {
			if(object_is_mesh(*b_ob)) {
				BL::Object::particle_systems_iterator b_psys;
				for(b_ob->particle_systems.begin(b_psys); b_psys != b_ob->particle_systems.end(); ++b_psys) {
					if((b_psys->settings().render_type()==BL::ParticleSettings::render_type_PATH)&&(b_psys->settings().type()==BL::ParticleSettings::type_HAIR)) {
						BL::ID key = BKE_object_is_modified(*b_ob)? *b_ob: b_ob->data();
						mesh_map.set_recalc(key);
						object_map.set_recalc(*b_ob);
					}
				}
			}
		}
	}

	if(curve_system_manager->modified(prev_curve_system_manager))
		curve_system_manager->tag_update(scene);
}

bool BlenderSync::sync_particle_curves(Mesh *mesh, BL::Mesh b_mesh, BL::Object b_ob, bool motion, int time_index)
{
	if(b_ob.mode() == b_ob.mode_PARTICLE_EDIT)
		return false;
	
	if(!preview)
		set_resolution(mesh, &b_mesh, &b_ob, &b_scene, true);
	
	int primitive = scene->curve_system_manager->primitive;
	int triangle_method = scene->curve_system_manager->triangle_method;
	int resolution = scene->curve_system_manager->resolution;
	size_t vert_num = mesh->verts.size();
	size_t tri_num = mesh->triangles.size();
	int used_res = 1;

	/* extract particle hair data - should be combined with connecting to mesh later*/

	ParticleCurveData CData;
	ObtainCacheParticleData(mesh, &b_mesh, &b_ob, &CData, !preview);

	/* add hair geometry to mesh */
	if(primitive == CURVE_TRIANGLES) {
		if(triangle_method == CURVE_CAMERA_TRIANGLES) {
			/* obtain camera parameters */
			float3 RotCam;
			Camera *camera = scene->camera;
			Transform &ctfm = camera->matrix;
			if(camera->type == CAMERA_ORTHOGRAPHIC) {
				RotCam = -make_float3(ctfm.x.z, ctfm.y.z, ctfm.z.z);
			}
			else {
				Transform tfm = get_transform(b_ob.matrix_world());
				Transform itfm = transform_quick_inverse(tfm);
				RotCam = transform_point(&itfm, make_float3(ctfm.x.w,
				                                            ctfm.y.w,
				                                            ctfm.z.w));
			}
			bool is_ortho = camera->type == CAMERA_ORTHOGRAPHIC;
			ExportParticleCurveTrianglePlanes(mesh, &CData, RotCam, is_ortho);
		}
		else {
			ExportParticleCurveTriangleGeometry(mesh, &CData, resolution);
			used_res = resolution;
		}
	}
	else {
		if(motion)
			ExportParticleCurveSegmentsMotion(scene, mesh, &CData, time_index);
		else
			ExportParticleCurveSegments(scene, mesh, &CData);
	}

	/* generated coordinates from first key. we should ideally get this from
	 * blender to handle deforming objects */
	if(!motion) {
		if(mesh->need_attribute(scene, ATTR_STD_GENERATED)) {
			float3 loc, size;
			mesh_texture_space(b_mesh, loc, size);

			if(primitive == CURVE_TRIANGLES) {
				mesh->attributes.reserve(); /* make sure the buffer size is correct, in case the attribute already exists */
				Attribute *attr_generated = mesh->attributes.add(ATTR_STD_GENERATED);
				float3 *generated = attr_generated->data_float3();

				for(size_t i = vert_num; i < mesh->verts.size(); i++)
					generated[i] = mesh->verts[i]*size - loc;
			}
			else {
				mesh->curve_attributes.reserve(); /* make sure the buffer size is correct, in case the attribute already exists */
				Attribute *attr_generated = mesh->curve_attributes.add(ATTR_STD_GENERATED);
				float3 *generated = attr_generated->data_float3();
				size_t i = 0;

				foreach(Mesh::Curve& curve, mesh->curves) {
					float3 co = float4_to_float3(mesh->curve_keys[curve.first_key]);
					generated[i++] = co*size - loc;
				}
			}
		}
	}

	/* create vertex color attributes */
	if(!motion) {
		BL::Mesh::tessface_vertex_colors_iterator l;
		int vcol_num = 0;

		for(b_mesh.tessface_vertex_colors.begin(l); l != b_mesh.tessface_vertex_colors.end(); ++l, vcol_num++) {
			if(!mesh->need_attribute(scene, ustring(l->name().c_str())))
				continue;

			ObtainCacheParticleVcol(mesh, &b_mesh, &b_ob, &CData, !preview, vcol_num);

			if(primitive == CURVE_TRIANGLES) {
				Attribute *attr_vcol = mesh->attributes.add(
					ustring(l->name().c_str()), TypeDesc::TypeColor, ATTR_ELEMENT_CORNER_BYTE);

				uchar4 *cdata = attr_vcol->data_uchar4();

				ExportParticleCurveTriangleVcol(mesh, &CData, tri_num * 3, used_res, cdata);
			}
			else {
				Attribute *attr_vcol = mesh->curve_attributes.add(
					ustring(l->name().c_str()), TypeDesc::TypeColor, ATTR_ELEMENT_CURVE);

				float3 *fdata = attr_vcol->data_float3();

				if(fdata) {
					size_t i = 0;

					for(size_t curve = 0; curve < CData.curve_vcol.size(); curve++)
						if(!(CData.curve_keynum[curve] <= 1 || CData.curve_length[curve] == 0.0f))
							fdata[i++] = color_srgb_to_scene_linear(CData.curve_vcol[curve]);
				}
			}
		}
	}

	/* create UV attributes */
	if(!motion) {
		BL::Mesh::tessface_uv_textures_iterator l;
		int uv_num = 0;

		for(b_mesh.tessface_uv_textures.begin(l); l != b_mesh.tessface_uv_textures.end(); ++l, uv_num++) {
			bool active_render = l->active_render();
			AttributeStandard std = (active_render)? ATTR_STD_UV: ATTR_STD_NONE;
			ustring name = ustring(l->name().c_str());

			/* UV map */
			if(mesh->need_attribute(scene, name) || mesh->need_attribute(scene, std)) {
				Attribute *attr_uv;

				ObtainCacheParticleUV(mesh, &b_mesh, &b_ob, &CData, !preview, uv_num);

				if(primitive == CURVE_TRIANGLES) {
					if(active_render)
						attr_uv = mesh->attributes.add(std, name);
					else
						attr_uv = mesh->attributes.add(name, TypeDesc::TypePoint, ATTR_ELEMENT_CORNER);

					float3 *uv = attr_uv->data_float3();

					ExportParticleCurveTriangleUV(mesh, &CData, tri_num * 3, used_res, uv);
				}
				else {
					if(active_render)
						attr_uv = mesh->curve_attributes.add(std, name);
					else
						attr_uv = mesh->curve_attributes.add(name, TypeDesc::TypePoint,  ATTR_ELEMENT_CURVE);

					float3 *uv = attr_uv->data_float3();

					if(uv) {
						size_t i = 0;

						for(size_t curve = 0; curve < CData.curve_uv.size(); curve++)
							if(!(CData.curve_keynum[curve] <= 1 || CData.curve_length[curve] == 0.0f))
								uv[i++] = CData.curve_uv[curve];
					}
				}
			}
		}
	}
	
	if(!preview)
		set_resolution(mesh, &b_mesh, &b_ob, &b_scene, false);
	
	return true;
}

bool BlenderSync::sync_hair_curves(Mesh *mesh, BL::Mesh b_mesh, BL::Object b_ob, bool motion, int time_index)
{
	int primitive = scene->curve_system_manager->primitive;
	int triangle_method = scene->curve_system_manager->triangle_method;
	int resolution = scene->curve_system_manager->resolution;
	size_t vert_num = mesh->verts.size();
	size_t tri_num = mesh->triangles.size();
	int used_res = 1;
	bool is_ortho = false;
	float3 RotCam;

	/* extract hair data - should be combined with connecting to mesh later*/

//	ParticleCurveData CData;
//	ObtainCacheParticleData(mesh, &b_mesh, &b_ob, &CData, !preview);

	/* add hair geometry to mesh */
	if(primitive == CURVE_TRIANGLES) {
		if(triangle_method == CURVE_CAMERA_TRIANGLES) {
			/* obtain camera parameters */
			Camera *camera = scene->camera;
			Transform &ctfm = camera->matrix;
			if(camera->type == CAMERA_ORTHOGRAPHIC) {
				RotCam = -make_float3(ctfm.x.z, ctfm.y.z, ctfm.z.z);
			}
			else {
				Transform tfm = get_transform(b_ob.matrix_world());
				Transform itfm = transform_quick_inverse(tfm);
				RotCam = transform_point(&itfm, make_float3(ctfm.x.w,
				                                            ctfm.y.w,
				                                            ctfm.z.w));
			}
			is_ortho = camera->type == CAMERA_ORTHOGRAPHIC;
		}
	}
	
	BL::Object::modifiers_iterator b_mod_iter;
	for (b_ob.modifiers.begin(b_mod_iter); b_mod_iter != b_ob.modifiers.end(); ++b_mod_iter) {
		if (b_mod_iter->type() != BL::Modifier::type_HAIR)
			continue;
		
		BL::HairModifier b_mod_hair(*b_mod_iter);
		
		/* add hair geometry to mesh */
		if(primitive == CURVE_TRIANGLES) {
			if(triangle_method == CURVE_CAMERA_TRIANGLES) {
				ExportHairCurveTrianglePlanes(mesh, b_mod_hair.hair_system(), RotCam, is_ortho);
			}
			else {
				ExportHairCurveTriangleGeometry(mesh, b_mod_hair.hair_system(), resolution);
				used_res = resolution;
			}
		}
		else {
			if(motion)
				ExportHairCurveSegmentsMotion(scene, mesh, b_mod_hair.hair_system(), time_index);
			else
				ExportHairCurveSegments(scene, mesh, b_mod_hair.hair_system());
		}

		/* generated coordinates from first key. we should ideally get this from
		 * blender to handle deforming objects */
		if(!motion) {
			if(mesh->need_attribute(scene, ATTR_STD_GENERATED)) {
				float3 loc, size;
				mesh_texture_space(b_mesh, loc, size);
				
				if(primitive == CURVE_TRIANGLES) {
					mesh->attributes.reserve(); /* make sure the buffer size is correct, in case the attribute already exists */
					Attribute *attr_generated = mesh->attributes.add(ATTR_STD_GENERATED);
					float3 *generated = attr_generated->data_float3();
					
					for(size_t i = vert_num; i < mesh->verts.size(); i++)
						generated[i] = mesh->verts[i]*size - loc;
				}
				else {
					mesh->curve_attributes.reserve(); /* make sure the buffer size is correct, in case the attribute already exists */
					Attribute *attr_generated = mesh->curve_attributes.add(ATTR_STD_GENERATED);
					float3 *generated = attr_generated->data_float3();
					size_t i = 0;
					
					foreach(Mesh::Curve& curve, mesh->curves) {
						float3 co = float4_to_float3(mesh->curve_keys[curve.first_key]);
						generated[i++] = co*size - loc;
					}
				}
			}
		}
		
#if 0
		/* create vertex color attributes */
		if(!motion) {
			BL::Mesh::tessface_vertex_colors_iterator l;
			int vcol_num = 0;
			
			for(b_mesh.tessface_vertex_colors.begin(l); l != b_mesh.tessface_vertex_colors.end(); ++l, vcol_num++) {
				if(!mesh->need_attribute(scene, ustring(l->name().c_str())))
					continue;
				
				ObtainCacheParticleVcol(mesh, &b_mesh, &b_ob, &CData, !preview, vcol_num);
				
				if(primitive == CURVE_TRIANGLES) {
					Attribute *attr_vcol = mesh->attributes.add(
					                           ustring(l->name().c_str()), TypeDesc::TypeColor, ATTR_ELEMENT_CORNER_BYTE);
					
					uchar4 *cdata = attr_vcol->data_uchar4();
					
					ExportCurveTriangleVcol(mesh, &CData, tri_num * 3, used_res, cdata);
				}
				else {
					Attribute *attr_vcol = mesh->curve_attributes.add(
					                           ustring(l->name().c_str()), TypeDesc::TypeColor, ATTR_ELEMENT_CURVE);
					
					float3 *fdata = attr_vcol->data_float3();
					
					if(fdata) {
						size_t i = 0;
						
						for(size_t curve = 0; curve < CData.curve_vcol.size(); curve++)
							if(!(CData.curve_keynum[curve] <= 1 || CData.curve_length[curve] == 0.0f))
								fdata[i++] = color_srgb_to_scene_linear(CData.curve_vcol[curve]);
					}
				}
			}
		}
		
		/* create UV attributes */
		if(!motion) {
			BL::Mesh::tessface_uv_textures_iterator l;
			int uv_num = 0;
			
			for(b_mesh.tessface_uv_textures.begin(l); l != b_mesh.tessface_uv_textures.end(); ++l, uv_num++) {
				bool active_render = l->active_render();
				AttributeStandard std = (active_render)? ATTR_STD_UV: ATTR_STD_NONE;
				ustring name = ustring(l->name().c_str());
				
				/* UV map */
				if(mesh->need_attribute(scene, name) || mesh->need_attribute(scene, std)) {
					Attribute *attr_uv;
					
					ObtainCacheParticleUV(mesh, &b_mesh, &b_ob, &CData, !preview, uv_num);
					
					if(primitive == CURVE_TRIANGLES) {
						if(active_render)
							attr_uv = mesh->attributes.add(std, name);
						else
							attr_uv = mesh->attributes.add(name, TypeDesc::TypePoint, ATTR_ELEMENT_CORNER);
						
						float3 *uv = attr_uv->data_float3();
						
						ExportCurveTriangleUV(mesh, &CData, tri_num * 3, used_res, uv);
					}
					else {
						if(active_render)
							attr_uv = mesh->curve_attributes.add(std, name);
						else
							attr_uv = mesh->curve_attributes.add(name, TypeDesc::TypePoint,  ATTR_ELEMENT_CURVE);
						
						float3 *uv = attr_uv->data_float3();
						
						if(uv) {
							size_t i = 0;
							
							for(size_t curve = 0; curve < CData.curve_uv.size(); curve++)
								if(!(CData.curve_keynum[curve] <= 1 || CData.curve_length[curve] == 0.0f))
									uv[i++] = CData.curve_uv[curve];
						}
					}
				}
			}
		}
#endif
		
	} /* modifiers */
	
	return true;
}

void BlenderSync::sync_curves(Mesh *mesh, BL::Mesh b_mesh, BL::Object b_ob, bool motion, int time_index)
{
	if(!motion) {
		/* Clear stored curve data */
		mesh->curve_keys.clear();
		mesh->curves.clear();
		mesh->curve_attributes.clear();
	}
	
	if(!scene->curve_system_manager->use_curves) {
		if(!motion)
			mesh->compute_bounds();
		return;
	}
	
	bool has_particle_curves = sync_particle_curves(mesh, b_mesh, b_ob, motion, time_index);
	bool has_hair_curves = sync_hair_curves(mesh, b_mesh, b_ob, motion, time_index);
	
	if (!motion || has_particle_curves || has_hair_curves)
		mesh->compute_bounds();
}

CCL_NAMESPACE_END

