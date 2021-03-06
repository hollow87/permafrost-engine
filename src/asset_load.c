/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2020 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 *  Linking this software statically or dynamically with other modules is making 
 *  a combined work based on this software. Thus, the terms and conditions of 
 *  the GNU General Public License cover the whole combination. 
 *  
 *  As a special exception, the copyright holders of Permafrost Engine give 
 *  you permission to link Permafrost Engine with independent modules to produce 
 *  an executable, regardless of the license terms of these independent 
 *  modules, and to copy and distribute the resulting executable under 
 *  terms of your choice, provided that you also meet, for each linked 
 *  independent module, the terms and conditions of the license of that 
 *  module. An independent module is a module which is not derived from 
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
 *  extend this exception to your version of Permafrost Engine, but you are not 
 *  obliged to do so. If you do not wish to do so, delete this exception 
 *  statement from your version.
 *
 */

#include "asset_load.h"
#include "entity.h"
#include "main.h"

#include "render/public/render_al.h"
#include "anim/public/anim.h"
#include "game/public/game.h"
#include "lib/public/attr.h"
#include "map/public/map.h"
#include "lib/public/khash.h"
#include "lib/public/pf_string.h"

#include <SDL.h>

#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h> 


#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

struct shared_resource{
    uint32_t     ent_flags;
    void        *render_private;
    void        *anim_private;
    struct aabb  aabb;
};

KHASH_MAP_INIT_STR(entity_res, struct shared_resource)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(entity_res) *s_name_resource_table;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool al_parse_pfobj_header(SDL_RWops *stream, struct pfobj_hdr *out)
{
    char line[MAX_LINE_LEN];

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "version %f", &out->version))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_verts %d", &out->num_verts))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_joints %d", &out->num_joints))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_materials %d", &out->num_materials))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_as %d", &out->num_as))
        goto fail;

    if(out->num_as > MAX_ANIM_SETS)
        goto fail;

    READ_LINE(stream, line, fail);
    if(!(strstr(line, "frame_counts")))
        goto fail;

    char *string = line;
    char *saveptr;

    /* Consume the first token, the property name 'frame_counts' */
    string = pf_strtok_r(line, " \t", &saveptr);
    for(int i = 0; i < out->num_as; i++) {

        string = pf_strtok_r(NULL, " \t", &saveptr);
        if(!string)
            goto fail;

        if(!sscanf(string, "%d", &out->frame_counts[i]))
            goto fail;
    }

    int tmp;
    READ_LINE(stream, line, fail);
    if(!sscanf(line, "has_collision %d", &tmp))
        goto fail;
    out->has_collision = tmp;

    return true;

fail:
    return false;
}

static bool al_parse_pfmap_header(SDL_RWops *stream, struct pfmap_hdr *out)
{
    char line[MAX_LINE_LEN];

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "version %f", &out->version))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_materials %d", &out->num_materials))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_rows %d", &out->num_rows))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_cols %d", &out->num_cols))
        goto fail;

    return true;

fail:
    return false;
}

static void al_set_ent_defaults(struct entity *ent)
{
    ent->flags = 0;
    ent->max_hp = 0;
    ent->scale = (vec3_t){1.0f, 1.0f, 1.0f};
    ent->rotation = (quat_t){0.0f, 0.0f, 0.0f, 1.0f};
    ent->selection_radius = 0.0f;
    ent->max_speed = 0.0f;
    ent->faction_id = 0; 
    ent->vision_range = 0.0f;
}

static bool al_get_resource(const char *path, const char *basedir, 
                            const char *pfobj_name, struct shared_resource *out)
{
    SDL_RWops *stream;
    struct pfobj_hdr header;

    khiter_t k = kh_get(entity_res, s_name_resource_table, pfobj_name);
    if(k != kh_end(s_name_resource_table)) {

        *out = kh_value(s_name_resource_table, k);
        return true;
    }

    stream = SDL_RWFromFile(path, "r");
    if(!stream)
        goto fail_init; 

    if(!al_parse_pfobj_header(stream, &header))
        goto fail_parse;

    out->ent_flags = 0;
    out->render_private = R_AL_PrivFromStream(basedir, &header, stream);
    if(!out->render_private)
        goto fail_parse;

    out->anim_private = A_AL_PrivFromStream(&header, stream);
    if(!out->anim_private)
        goto fail_parse;

    if(header.num_as > 0) {
        out->ent_flags |= ENTITY_FLAG_ANIMATED;
    }

    if(!header.has_collision) {
        fprintf(stderr, "Imported entities required to have bounding boxes.\n");
        goto fail_parse;
    }

    if(!AL_ParseAABB(stream, &out->aabb))
        goto fail_parse;

    int put_ret;
    k = kh_put(entity_res, s_name_resource_table, pf_strdup(pfobj_name), &put_ret);
    assert(put_ret != -1 && put_ret != 0);
    kh_value(s_name_resource_table, k) = *out;

    SDL_RWclose(stream);
    return true;

fail_parse:
    SDL_RWclose(stream);
fail_init:
    return false;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

struct entity *AL_EntityFromPFObj(const char *base_path, const char *pfobj_name, 
                                  const char *name, uint32_t uid)
{
    struct shared_resource res;
    char abs_basepath[512], pfobj_path[512];

    size_t alloc_size = sizeof(struct entity) + A_AL_CtxBuffSize();
    struct entity *ret = malloc(alloc_size);
    if(!ret)
        goto fail_alloc;

    al_set_ent_defaults(ret);
    ret->anim_ctx = (void*)(ret + 1);

    ret->name = pf_strdup(name);
    ret->filename = pf_strdup(pfobj_name);
    ret->basedir = pf_strdup(base_path);

    if(!ret->name || !ret->filename || !ret->basedir)
        goto fail_init;

    pf_snprintf(abs_basepath, sizeof(abs_basepath), "%s/%s", g_basepath, base_path);
    pf_snprintf(pfobj_path, sizeof(pfobj_path), "%s/%s/%s", g_basepath, base_path, pfobj_name);

    if(!al_get_resource(pfobj_path, abs_basepath, pfobj_name, &res))
        goto fail_init;

    ret->flags |= res.ent_flags;
    ret->render_private = res.render_private;
    ret->anim_private = res.anim_private;
    ret->identity_aabb = res.aabb;
    ret->uid = uid;

    if(ret->flags & ENTITY_FLAG_ANIMATED) {
        A_InitCtx(ret, A_GetClip(ret, 0), 24);
    }

    return ret;

fail_init:
    free((void*)ret->basedir);
    free((void*)ret->filename);
    free((void*)ret->name);
    free(ret);
fail_alloc:
    return NULL;
}

bool AL_EntitySetPFObj(struct entity *ent, const char *base_path, const char *pfobj_name)
{
    struct shared_resource res;
    char abs_basepath[512], pfobj_path[512];

    pf_snprintf(abs_basepath, sizeof(abs_basepath), "%s/%s", g_basepath, base_path);
    pf_snprintf(pfobj_path, sizeof(pfobj_path), "%s/%s/%s", g_basepath, base_path, pfobj_name);

    if(!al_get_resource(pfobj_path, abs_basepath, pfobj_name, &res))
        goto fail_init;

    const char *newdir = pf_strdup(base_path);
    const char *newobj = pf_strdup(pfobj_name);

    if(!newdir || !newobj)
        goto fail_alloc;

    free((void*)ent->basedir);
    free((void*)ent->filename);
    ent->basedir = newdir;
    ent->filename = newobj;

    ent->render_private = res.render_private;
    ent->anim_private = res.anim_private;
    ent->identity_aabb = res.aabb;

    if(ent->flags & ENTITY_FLAG_ANIMATED) {
        A_InitCtx(ent, A_GetClip(ent, 0), 24);
    }
    return true;

fail_alloc:
    free((void*)newdir);
    free((void*)newobj);
fail_init:
    return false;
}

void AL_EntityFree(struct entity *entity)
{
    free((void*)entity->basedir);
    free((void*)entity->filename);
    free((void*)entity->name);
    free(entity);
}

struct map *AL_MapFromPFMapStream(SDL_RWops *stream, bool update_navgrid)
{
    struct map *ret;
    struct pfmap_hdr header;

    if(!al_parse_pfmap_header(stream, &header))
        goto fail_parse;

    ret = malloc(M_AL_BuffSizeFromHeader(&header));
    if(!ret)
        goto fail_alloc;

    if(!M_AL_InitMapFromStream(&header, g_basepath, stream, ret, update_navgrid))
        goto fail_init;

    return ret;

fail_init:
    free(ret);
fail_alloc:
fail_parse:
    return NULL;
}

size_t AL_MapShallowCopySize(SDL_RWops *stream)
{
    struct pfmap_hdr header;
    size_t ret = 0;
    size_t pos = SDL_RWseek(stream, 0, RW_SEEK_CUR);

    if(!al_parse_pfmap_header(stream, &header))
        goto fail_parse;

    ret = M_AL_ShallowCopySize(header.num_rows, header.num_cols);

fail_parse:
    SDL_RWseek(stream, pos, RW_SEEK_SET);
    return ret;
}

void AL_MapFree(struct map *map)
{
    M_AL_FreePrivate(map);
    free(map);
}

bool AL_ReadLine(SDL_RWops *stream, char *outbuff)
{
    int idx = 0;
    do { 
        if(!SDL_RWread(stream, outbuff + idx, 1, 1))
            return false; 

        if(outbuff[idx] == '\n') {
            /* nuke the carriage return before the newline - to give a consistent 
             * output to client code regardless of platform */
            if(idx && outbuff[idx-1] == '\r') {
                outbuff[idx-1] = '\n';
                outbuff[idx] = '\0';
            }
            outbuff[++idx] = '\0';
            return true;
        }
        
        idx++; 
    }while(idx < MAX_LINE_LEN-1);

    return false;
}

bool AL_ParseAABB(SDL_RWops *stream, struct aabb *out)
{
    char line[MAX_LINE_LEN];

    READ_LINE(stream, line, fail);
    if(!sscanf(line, " x_bounds %f %f", &out->x_min, &out->x_max))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, " y_bounds %f %f", &out->y_min, &out->y_max))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, " z_bounds %f %f", &out->z_min, &out->z_max))
        goto fail;

    return true;

fail:
    return false;
}

bool AL_Init(void)
{
    s_name_resource_table = kh_init(entity_res);
    return (s_name_resource_table != NULL);
}

void AL_Shutdown(void)
{
    const char *key;
    struct shared_resource curr;

    kh_foreach(s_name_resource_table, key, curr, {
        free((void*)key);
        free(curr.render_private);
        free(curr.anim_private);
    });
    kh_destroy(entity_res, s_name_resource_table);
}

bool AL_SaveOBB(SDL_RWops *stream, const struct obb *obb)
{
    struct attr curr;

    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->center };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_center"));

    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->axes[0] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_x_axis"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->axes[1] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_y_axis"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->axes[2] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_z_axis"));

    curr = (struct attr){.type = TYPE_FLOAT, .val.as_float = obb->half_lengths[0] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_x_half_len"));
    curr = (struct attr){.type = TYPE_FLOAT, .val.as_float = obb->half_lengths[1] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_y_half_len"));
    curr = (struct attr){.type = TYPE_FLOAT, .val.as_float = obb->half_lengths[2] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_z_half_len"));

    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[0] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_0_corner"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[1] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_1_corner"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[2] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_2_corner"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[3] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_3_corner"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[4] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_4_corner"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[5] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_5_corner"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[6] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_6_corner"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[7] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_7_corner"));

    return true;
}

bool AL_LoadOBB(SDL_RWops *stream, struct obb *out)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->center = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->axes[0] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->axes[1] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->axes[2] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_FLOAT);
    out->half_lengths[0] = attr.val.as_float;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_FLOAT);
    out->half_lengths[1] = attr.val.as_float;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_FLOAT);
    out->half_lengths[2] = attr.val.as_float;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[0] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[1] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[2] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[3] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[4] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[5] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[6] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[7] = attr.val.as_vec3;

    return true;
}

