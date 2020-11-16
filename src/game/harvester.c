/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#include "harvester.h"
#include "movement.h"
#include "resource.h"
#include "game_private.h"
#include "public/game.h"
#include "../event.h"
#include "../entity.h"
#include "../cursor.h"
#include "../lib/public/vec.h"
#include "../lib/public/mpool.h"
#include "../lib/public/khash.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/string_intern.h"

#include <stddef.h>
#include <assert.h>
#include <string.h>

#define UID_NONE            (~((uint32_t)0))
#define REACQUIRE_RADIUS    (50.0)
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define MIN(a, b)           ((a) < (b) ? (a) : (b))

static void *pmalloc(size_t size);
static void *pcalloc(size_t n, size_t size);
static void *prealloc(void *ptr, size_t size);
static void  pfree(void *ptr);

#undef kmalloc
#undef kcalloc
#undef krealloc
#undef kfree

#define kmalloc  pmalloc
#define kcalloc  pcalloc
#define krealloc prealloc
#define kfree    pfree

KHASH_MAP_INIT_STR(int, int)

VEC_TYPE(name, const char*)
VEC_IMPL(static, name, const char*)

enum harvester_state{
    STATE_NOT_HARVESTING,
    STATE_HARVESTING_SEEK_RESOURCE,
    STATE_HARVESTING,
    STATE_HARVESTING_SEEK_STORAGE,
    STATE_TRANSPORT_GETTING,
    STATE_TRANSPORT_PUTTING,
};

struct hstate{
    enum harvester_state state;
    enum tstrategy strategy;
    uint32_t    ss_uid;
    uint32_t    res_uid;
    vec2_t      res_last_pos;
    const char *res_name;         /* borrowed */
    kh_int_t   *gather_speeds;    /* How much of each resource the entity gets each cycle */
    kh_int_t   *max_carry;        /* The maximum amount of each resource the entity can carry */
    kh_int_t   *curr_carry;       /* The amount of each resource the entity currently holds */
    vec_name_t  priority;         /* The order in which the harvester will transport resources */
    bool        drop_off_only;
    struct{
        enum{
            CMD_NONE,
            CMD_GATHER,
            CMD_TRANSPORT,
        }cmd;
        uint32_t uid_arg;
    }queued;
    uint32_t    transport_src_uid;
    uint32_t    transport_dest_uid;
};

struct searcharg{
    const struct entity *ent;
    const char *rname;
    enum tstrategy strat;
};

typedef char buff_t[512];

MPOOL_TYPE(buff, buff_t)
MPOOL_PROTOTYPES(static, buff, buff_t)
MPOOL_IMPL(static, buff, buff_t)

#undef kmalloc
#undef kcalloc
#undef krealloc
#undef kfree

#define kmalloc  malloc
#define kcalloc  calloc
#define krealloc realloc
#define kfree    free

KHASH_MAP_INIT_INT(state, struct hstate)

static void on_motion_begin_harvest(void *user, void *event);
static void on_motion_begin_travel(void *user, void *event);
static void on_arrive_at_resource(void *user, void *event);
static void on_arrive_at_storage(void *user, void *event);
static void on_arrive_at_transport_source(void *user, void *event);
static void on_arrive_at_transport_dest(void *user, void *event);
static void on_harvest_anim_finished(void *user, void *event);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static mp_buff_t         s_mpool;
static khash_t(stridx)  *s_stridx;
static mp_strbuff_t      s_stringpool;
static khash_t(state)   *s_entity_state_table;
static const struct map *s_map;

static bool              s_gather_on_lclick = false;
static bool              s_drop_off_on_lclick = false;
static bool              s_transport_on_lclick = false;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void *pmalloc(size_t size)
{
    mp_ref_t ref = mp_buff_alloc(&s_mpool);
    if(ref == 0)
        return NULL;
    return mp_buff_entry(&s_mpool, ref);
}

static void *pcalloc(size_t n, size_t size)
{
    void *ret = pmalloc(n * size);
    if(!ret)
        return NULL;
    memset(ret, 0, n * size);
    return ret;
}

static void *prealloc(void *ptr, size_t size)
{
    if(!ptr)
        return pmalloc(size);
    if(size <= sizeof(buff_t))
        return ptr;
    return NULL;
}

static void pfree(void *ptr)
{
    if(!ptr)
        return;
    mp_ref_t ref = mp_buff_ref(&s_mpool, ptr);
    mp_buff_free(&s_mpool, ref);
}

static struct hstate *hstate_get(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;

    return &kh_value(s_entity_state_table, k);
}

static bool hstate_set(uint32_t uid, struct hstate hs)
{
    int status;
    khiter_t k = kh_put(state, s_entity_state_table, uid, &status);
    if(status == -1 || status == 0)
        return false;
    kh_value(s_entity_state_table, k) = hs;
    return true;
}

static void hstate_remove(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k != kh_end(s_entity_state_table))
        kh_del(state, s_entity_state_table, k);
}

static void hstate_destroy(struct hstate *hs)
{
    vec_name_destroy(&hs->priority);
    kh_destroy(int, hs->gather_speeds);
    kh_destroy(int, hs->max_carry);
    kh_destroy(int, hs->curr_carry);
}

static bool hstate_init(struct hstate *hs)
{
    vec_name_init_alloc(&hs->priority, prealloc, pfree);
    if(!vec_name_resize(&hs->priority, sizeof(buff_t) / sizeof(char*)))
        return false;

    hs->gather_speeds = kh_init(int);
    hs->max_carry = kh_init(int);
    hs->curr_carry = kh_init(int);

    if(!hs->gather_speeds || !hs->max_carry || !hs->curr_carry) {

        hstate_destroy(hs);
        return false;
    }

    hs->ss_uid = UID_NONE;
    hs->res_uid = UID_NONE;
    hs->res_last_pos = (vec2_t){0};
    hs->res_name = NULL;
    hs->state = STATE_NOT_HARVESTING;
    hs->drop_off_only = false;
    hs->strategy = TRANSPORT_STRATEGY_NEAREST;
    hs->queued.cmd = CMD_NONE;
    hs->transport_src_uid = UID_NONE;
    hs->transport_dest_uid = UID_NONE;
    return true;
}

static bool hstate_set_key(khash_t(int) *table, const char *name, int val)
{
    const char *key = si_intern(name, &s_stringpool, s_stridx);
    if(!key)
        return false;

    khiter_t k = kh_get(int, table, key);
    if(k != kh_end(table)) {
        kh_value(table, k) = val;
        return true;
    }

    int status;
    k = kh_put(int, table, key, &status);
    if(status == -1) {
        return false;
    }

    assert(status == 1);
    kh_value(table, k) = val;
    return true;
}

static bool hstate_get_key(khash_t(int) *table, const char *name, int *out)
{
    const char *key = si_intern(name, &s_stringpool, s_stridx);
    if(!key)
        return false;

    khiter_t k = kh_get(int, table, key);
    if(k == kh_end(table))
        return false;
    *out = kh_value(table, k);
    return true;
}

static bool compare_keys(const char **a, const char **b)
{
    /* Since all strings are interned, we can use direct pointer comparision */
    return *a == *b;
}

static void hstate_remove_prio(struct hstate *hs, const char *name)
{
    int idx = vec_name_indexof(&hs->priority, name, compare_keys);
    if(idx == -1)
        return;
    vec_name_del(&hs->priority, idx);
}

static void hstate_insert_prio(struct hstate *hs, const char *name)
{
    int idx = vec_name_indexof(&hs->priority, name, compare_keys);
    if(idx != -1)
        return;

    for(idx = 0; idx < vec_size(&hs->priority); idx++) {
        if(strcmp(vec_AT(&hs->priority, idx), name) > 0)
            break;
    }

    if(idx < vec_size(&hs->priority)) {
        if(!vec_name_resize(&hs->priority, vec_size(&hs->priority) + 1))
            return;

        memmove(hs->priority.array + idx + 1, hs->priority.array + idx, 
            sizeof(const char*) * (vec_size(&hs->priority) - idx));

        hs->priority.array[idx] = name;
        hs->priority.size++;

    }else{
        vec_name_push(&hs->priority, name);
    }
}

static bool valid_storage_site_dropoff(const struct entity *curr, void *arg)
{
    struct searcharg *sarg = arg;
    struct hstate *hs = hstate_get(sarg->ent->uid);

    if(!(curr->flags & ENTITY_FLAG_STORAGE_SITE))
        return false;
    if(sarg->ent->faction_id != curr->faction_id)
        return false;

    int stored = G_StorageSite_GetCurr(curr->uid, sarg->rname);
    int cap = G_StorageSite_GetCapacity(curr->uid, sarg->rname);

    if(cap == 0)
        return false;
    if(stored == cap)
        return false;

    return true;
}

static bool valid_storage_site_source(const struct entity *curr, void *arg)
{
    struct searcharg *sarg = arg;
    struct hstate *hs = hstate_get(sarg->ent->uid);

    if(!(curr->flags & ENTITY_FLAG_STORAGE_SITE))
        return false;
    if(sarg->ent->faction_id != curr->faction_id)
        return false;
    if(curr == sarg->ent)
        return false;

    int stored = G_StorageSite_GetCurr(curr->uid, sarg->rname);
    int cap = G_StorageSite_GetCapacity(curr->uid, sarg->rname);
    int desired = G_StorageSite_GetDesired(curr->uid, sarg->rname);

    if(sarg->strat == TRANSPORT_STRATEGY_EXCESS && (stored >= desired))
        return false;
    if(cap == 0)
        return false;
    if(stored == 0)
        return false;

    return true;
}

static bool valid_resource(const struct entity *curr, void *arg)
{
    const char *name = arg;
    if(!(curr->flags & ENTITY_FLAG_RESOURCE))
        return false;
    if(strcmp(name, G_Resource_GetName(curr->uid)))
        return false;
    return true;
}

struct entity *nearest_storage_site_dropff(const struct entity *ent, const char *rname)
{
    vec2_t pos = G_Pos_GetXZ(ent->uid);
    struct searcharg arg = (struct searcharg){ent, rname};
    return G_Pos_NearestWithPred(pos, valid_storage_site_dropoff, (void*)&arg, 0.0f);
}

struct entity *nearest_storage_site_source(const struct entity *ent, const char *rname, enum tstrategy strat)
{
    vec2_t pos = G_Pos_GetXZ(ent->uid);
    struct searcharg arg = (struct searcharg){ent, rname, strat};
    struct entity *ret = G_Pos_NearestWithPred(pos, valid_storage_site_source, (void*)&arg, 0.0f);

    if(!ret && strat == TRANSPORT_STRATEGY_EXCESS) {
        arg = (struct searcharg){ent, rname, TRANSPORT_STRATEGY_NEAREST};
        ret = G_Pos_NearestWithPred(pos, valid_storage_site_source, (void*)&arg, 0.0f);
    }
    return ret;
}

struct entity *nearest_resource(const struct entity *ent, const char *name)
{
    vec2_t pos = G_Pos_GetXZ(ent->uid);
    return G_Pos_NearestWithPred(pos, valid_resource, (void*)name, REACQUIRE_RADIUS);
}

static void finish_harvesting(struct hstate *hs, uint32_t uid)
{
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished);
    E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin_harvest);

    hs->state = STATE_NOT_HARVESTING;
    hs->res_uid = UID_NONE;
    hs->res_last_pos = (vec2_t){0};
    hs->res_name = NULL;

    E_Entity_Notify(EVENT_HARVEST_END, uid, NULL, ES_ENGINE);
}

static const char *carried_resource_name(struct hstate *hs)
{
    const char *key;
    int curr;

    kh_foreach(hs->curr_carry, key, curr, {
        if(curr > 0)
            return key;
    });
    return NULL;
}

static void entity_drop_off(const struct entity *ent, struct entity *ss)
{
    struct hstate *hs = hstate_get(ent->uid);
    assert(hs);

    hs->state = STATE_HARVESTING_SEEK_STORAGE;
    hs->ss_uid = ss->uid;

    E_Entity_Register(EVENT_MOTION_END, ent->uid, on_arrive_at_storage, 
        (void*)((uintptr_t)ent->uid), G_RUNNING);
    E_Entity_Register(EVENT_MOVE_ISSUED, ent->uid, on_motion_begin_travel, 
        (void*)((uintptr_t)ent->uid), G_RUNNING);

    E_Entity_Notify(EVENT_STORAGE_TARGET_ACQUIRED, ent->uid, ss, ES_ENGINE);
    G_Move_SetSurroundEntity(ent, ss);
}

static void entity_try_drop_off(struct entity *ent)
{
    struct hstate *hs = hstate_get(ent->uid);
    assert(hs);

    if(!G_Harvester_GetCurrTotalCarry(ent->uid)) {
        hs->state = STATE_NOT_HARVESTING;
        return;
    }

    struct entity *ss = nearest_storage_site_dropff(ent, carried_resource_name(hs));
    if(!ss) {
        hs->state = STATE_NOT_HARVESTING;
    }else{
        entity_drop_off(ent, ss);
    }
}

static void entity_try_gather_nearest(struct entity *ent, const char *rname)
{
    struct entity *newtarget = nearest_resource(ent, rname);
    if(newtarget) {
        G_Harvester_Gather(ent, newtarget);
    }else{
        entity_try_drop_off(ent);
    }
}

static void entity_try_retarget(struct entity *ent)
{
    struct hstate *hs = hstate_get(ent->uid);
    const char *rname = hs->res_name;

    finish_harvesting(hs, ent->uid);
    entity_try_gather_nearest(ent, rname);
}

static struct entity *target_resource(struct hstate *hs)
{
    struct entity *resource = (hs->res_uid != UID_NONE) ? G_EntityForUID(hs->res_uid) : NULL;
    if(resource && (resource->flags & ENTITY_FLAG_ZOMBIE)) {
        resource = NULL;
    }
    if(!resource) {
        resource = G_Pos_NearestWithPred(hs->res_last_pos, valid_resource, 
            (void*)hs->res_name, REACQUIRE_RADIUS);
    }
    return resource;
}

static void clear_queued_command(struct entity *ent)
{
    struct hstate *hs = hstate_get(ent->uid);
    assert(hs);

    int cmd = hs->queued.cmd;
    uint32_t arg = hs->queued.uid_arg;
    hs->queued.cmd = CMD_NONE;

    if(cmd == CMD_GATHER) {
        struct entity *resource = G_EntityForUID(arg);
        if(resource) {
            G_Harvester_Gather(ent, resource);
        }
    }else if(cmd == CMD_TRANSPORT) {
        struct entity *storage = G_EntityForUID(arg);
        if(storage) {
            G_Harvester_Transport(ent, storage);
        }
    }
    hs->drop_off_only = false;
}

static struct entity *transport_source(struct entity *ent, struct entity *storage, const char *rname)
{
    struct hstate *hs = hstate_get(ent->uid);
    assert(hs);
    return nearest_storage_site_source(storage, rname, hs->strategy);
}

static void finish_transporing(struct hstate *hs)
{
    hs->transport_dest_uid = UID_NONE;
    hs->transport_src_uid = UID_NONE;
    hs->res_name = NULL;
    hs->state = STATE_NOT_HARVESTING;
}

static void on_harvest_anim_finished(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct entity *ent = G_EntityForUID(uid);

    struct hstate *hs = hstate_get(uid);
    assert(hs);

    struct entity *target = G_EntityForUID(hs->res_uid);
    if(!target || (target->flags & ENTITY_FLAG_ZOMBIE)) {
        /* If the resource was exhausted, switch targets to the nearest 
         * resource of the same type */
        entity_try_retarget(ent);
        return;
    }

    const char *rname = G_Resource_GetName(target->uid);
    int resource_left = G_Resource_GetAmount(target->uid);

    int gather_speed = G_Harvester_GetGatherSpeed(uid, rname);
    int old_carry = G_Harvester_GetCurrCarry(uid, rname);
    int max_carry = G_Harvester_GetMaxCarry(uid, rname);

    int new_carry = MIN(max_carry, old_carry + MIN(gather_speed, resource_left));
    resource_left = MAX(0, resource_left - (new_carry - old_carry));

    G_Resource_SetAmount(target->uid, resource_left);
    G_Harvester_SetCurrCarry(uid, rname, new_carry);

    if(resource_left == 0) {

        E_Entity_Notify(EVENT_RESOURCE_EXHAUSTED, target->uid, NULL, ES_ENGINE);
        G_Zombiefy(target);

        if(new_carry < max_carry) {
            entity_try_retarget(ent);
            return;
        }
    }

    /* Bring the resource to the nearest storage site, if possible */
    if(new_carry == max_carry) {

        E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished);
        E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin_harvest);

        E_Entity_Notify(EVENT_HARVEST_END, uid, NULL, ES_ENGINE);
        entity_try_drop_off(ent);
    }
}

static void on_motion_begin_harvest(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    const struct entity *ent = G_EntityForUID(uid);

    struct hstate *hs = hstate_get(uid);
    assert(hs);
    assert(hs->state == STATE_HARVESTING);

    finish_harvesting(hs, uid);
}

static void on_motion_begin_travel(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    const struct entity *ent = G_EntityForUID(uid);

    struct hstate *hs = hstate_get(uid);
    assert(hs);
    assert(hs->state == STATE_HARVESTING_SEEK_RESOURCE
        || hs->state == STATE_HARVESTING_SEEK_STORAGE
        || hs->state == STATE_TRANSPORT_GETTING
        || hs->state == STATE_TRANSPORT_PUTTING);

    E_Entity_Unregister(EVENT_MOVE_ISSUED, uid, on_motion_begin_travel);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_resource);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_storage);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_transport_source);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_transport_dest);
}

static void on_arrive_at_resource(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct entity *ent = G_EntityForUID(uid);

    if(!G_Move_Still(ent)) {
        return; 
    }

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_resource);
    E_Entity_Unregister(EVENT_MOVE_ISSUED, uid, on_motion_begin_travel);

    struct hstate *hs = hstate_get(uid);
    assert(hs);
    assert(hs->res_uid != UID_NONE);
    struct entity *target = G_EntityForUID(hs->res_uid);

    if(!target
    || (target->flags & ENTITY_FLAG_ZOMBIE)
    || !M_NavObjAdjacent(s_map, ent, target)) {

        /* harvester could not reach the resource */
        entity_try_gather_nearest(ent, hs->res_name);
        return; 
    }

    if(G_Harvester_GetCurrCarry(uid, hs->res_name) == G_Harvester_GetMaxCarry(uid, hs->res_name)) {

        /* harvester cannot carry any more of the resource */
        entity_try_drop_off(ent);
        return;
    }

    E_Entity_Notify(EVENT_HARVEST_BEGIN, uid, NULL, ES_ENGINE);
    hs->state = STATE_HARVESTING; 
    E_Entity_Register(EVENT_MOTION_START, uid, on_motion_begin_harvest, (void*)((uintptr_t)uid), G_RUNNING);
    E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished, 
        (void*)((uintptr_t)uid), G_RUNNING);
}

static void on_arrive_at_storage(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct entity *ent = G_EntityForUID(uid);

    if(!G_Move_Still(ent)) {
        return; 
    }

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_storage);
    E_Entity_Unregister(EVENT_MOVE_ISSUED, uid, on_motion_begin_travel);

    struct hstate *hs = hstate_get(uid);
    assert(hs);
    assert(hs->ss_uid != UID_NONE);
    struct entity *target = G_EntityForUID(hs->ss_uid);

    if(!target 
    || (target->flags & ENTITY_FLAG_ZOMBIE)
    || !M_NavObjAdjacent(s_map, ent, target)) {
        /* harvester could not reach the storage site */
        entity_try_drop_off(ent);
        return; 
    }

    const char *rname = carried_resource_name(hs);
    assert(rname);

    int carry = G_Harvester_GetCurrCarry(uid, rname);
    int cap = G_StorageSite_GetCapacity(target->uid, rname);
    int curr = G_StorageSite_GetCurr(target->uid, rname);
    int left = cap - curr;

    if(left > 0) {
        E_Entity_Notify(EVENT_RESOURCE_DROPPED_OFF, ent->uid, NULL, ES_ENGINE);
    }

    if(left >= carry) {
        G_Harvester_SetCurrCarry(uid, rname, 0);
        G_StorageSite_SetCurr(target, rname, curr + carry);

        struct entity *resource = target_resource(hs);
        if(resource && !hs->drop_off_only) {
            G_Harvester_Gather(ent, resource);
        }else{
            hs->state = STATE_NOT_HARVESTING;
            hs->ss_uid = UID_NONE;
            hs->res_name = NULL;
            clear_queued_command(ent);
        }

    }else{

        G_Harvester_SetCurrCarry(uid, rname, carry - left);
        G_StorageSite_SetCurr(target, rname, cap);

        entity_try_drop_off(ent);
    }
}

static void on_arrive_at_transport_source(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct entity *ent = G_EntityForUID(uid);

    if(!G_Move_Still(ent)) {
        return; 
    }

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_transport_source);
    E_Entity_Unregister(EVENT_MOVE_ISSUED, uid, on_motion_begin_travel);

    struct hstate *hs = hstate_get(uid);
    assert(hs);
    struct entity *src = G_EntityForUID(hs->transport_src_uid);

    if(!src
    || (src->flags & ENTITY_FLAG_ZOMBIE)
    || !M_NavObjAdjacent(s_map, ent, src)) {

        /* harvester could not reach the storage site */
        struct entity *dest = G_EntityForUID(hs->transport_dest_uid);
        finish_transporing(hs);

        /* If the destination is still there, re-try */
        if(dest) {
            G_Harvester_Transport(ent, dest);
        }
        return;
    }

    int store_cap = G_StorageSite_GetCapacity(src->uid, hs->res_name);
    int store_curr = G_StorageSite_GetCurr(src->uid, hs->res_name);
    int desired = G_StorageSite_GetDesired(src->uid, hs->res_name);
    int excess = store_curr - desired;

    int max_carry = G_Harvester_GetMaxCarry(uid, hs->res_name);
    int curr_carry = G_Harvester_GetCurrCarry(uid, hs->res_name);
    int carry_cap = max_carry - curr_carry;

    /* Figure out how much resource to take. Add it to our carry .
     */
    int take = 0;
    switch(hs->strategy) {
    case TRANSPORT_STRATEGY_NEAREST: {
        take = MIN(carry_cap, store_curr);
        break;
    }
    case TRANSPORT_STRATEGY_EXCESS: {
        take = MIN(carry_cap, excess);
        break;
    }
    default: assert(0);
    }

    G_Harvester_SetCurrCarry(uid, hs->res_name, curr_carry + take);
    G_StorageSite_SetCurr(src, hs->res_name, store_curr - take);

    if(take > 0) {
        E_Entity_Notify(EVENT_RESOURCE_PICKED_UP, uid, NULL, ES_ENGINE);
    }

    struct entity *dest = G_EntityForUID(hs->transport_dest_uid);
    if(!dest) {

        hs->transport_dest_uid = UID_NONE;
        hs->transport_src_uid = UID_NONE;
        hs->res_name = NULL;
        hs->state = STATE_NOT_HARVESTING;
        return;
    }

    /* If we're under our max carry, search for another eligible storage site
     * where we can get the rest of the desired resource. Make a stop there 
     * as well.
     */
    if(curr_carry + take < max_carry) {
    
        struct entity *newsrc = transport_source(ent, dest, hs->res_name);
        if(newsrc) {
            E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_transport_source, 
                (void*)((uintptr_t)uid), G_RUNNING);
            E_Entity_Register(EVENT_MOVE_ISSUED, uid, on_motion_begin_travel, 
                (void*)((uintptr_t)uid), G_RUNNING);

            G_Move_SetSurroundEntity(ent, newsrc);
            hs->transport_src_uid = newsrc->uid;
            return;
        }
    }

    /* Lastly, drop off our stuff at the destination
     */
    E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_transport_dest, 
        (void*)((uintptr_t)uid), G_RUNNING);
    E_Entity_Register(EVENT_MOVE_ISSUED, uid, on_motion_begin_travel, 
        (void*)((uintptr_t)uid), G_RUNNING);

    G_Move_SetSurroundEntity(ent, dest);
    hs->state = STATE_TRANSPORT_PUTTING;
    E_Entity_Notify(EVENT_TRANSPORT_TARGET_ACQUIRED, uid, dest, ES_ENGINE);
}

static void on_arrive_at_transport_dest(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct entity *ent = G_EntityForUID(uid);

    if(!G_Move_Still(ent)) {
        return; 
    }

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_transport_dest);
    E_Entity_Unregister(EVENT_MOVE_ISSUED, uid, on_motion_begin_travel);

    struct hstate *hs = hstate_get(uid);
    assert(hs);
    struct entity *dest = G_EntityForUID(hs->transport_dest_uid);

    if(!dest 
    || (dest->flags & ENTITY_FLAG_ZOMBIE)
    || !M_NavObjAdjacent(s_map, ent, dest)) {
        /* harvester could not reach the destination storage site */
        finish_transporing(hs);
        return;
    }

    const char *rname = carried_resource_name(hs);
    assert(rname);

    int carry = G_Harvester_GetCurrCarry(uid, rname);
    int cap = G_StorageSite_GetCapacity(dest->uid, rname);
    int curr = G_StorageSite_GetCurr(dest->uid, rname);
    int left = cap - curr;

    if(left > 0) {
        E_Entity_Notify(EVENT_RESOURCE_DROPPED_OFF, ent->uid, NULL, ES_ENGINE);
    }

    if(left >= carry) {
        G_Harvester_SetCurrCarry(uid, rname, 0);
        G_StorageSite_SetCurr(dest, rname, curr + carry);

    }else{

        G_Harvester_SetCurrCarry(uid, rname, carry - left);
        G_StorageSite_SetCurr(dest, rname, cap);
    }

    finish_transporing(hs);
    G_Harvester_Transport(ent, dest);
}

static void selection_try_order_gather(void)
{
    if(G_CurrContextualAction() != CTX_ACTION_GATHER)
        return;

    struct entity *target = G_Sel_GetHovered();
    assert(target && (target->flags & ENTITY_FLAG_RESOURCE));

    enum selection_type sel_type;
    const vec_pentity_t *sel = G_Sel_Get(&sel_type);
    size_t ngather = 0;
    const char *rname = G_Resource_GetName(target->uid);

    if(sel_type != SELECTION_TYPE_PLAYER)
        return;

    for(int i = 0; i < vec_size(sel); i++) {

        struct entity *curr = vec_AT(sel, i);
        if(!(curr->flags & ENTITY_FLAG_HARVESTER))
            continue;

        if(G_Harvester_GetMaxCarry(curr->uid, rname) == 0
        || G_Harvester_GetGatherSpeed(curr->uid, rname) == 0)
            continue;

        struct hstate *hs = hstate_get(curr->uid);
        assert(hs);

        finish_harvesting(hs, curr->uid);
        G_Harvester_Gather(curr, target);
        ngather++;
    }

    if(ngather) {
        Entity_Ping(target);
    }
}

static void selection_try_order_drop_off(void)
{
    if(G_CurrContextualAction() != CTX_ACTION_DROP_OFF)
        return;

    struct entity *target = G_Sel_GetHovered();
    assert(target && (target->flags & ENTITY_FLAG_STORAGE_SITE));

    enum selection_type sel_type;
    const vec_pentity_t *sel = G_Sel_Get(&sel_type);
    size_t ngather = 0;

    if(sel_type != SELECTION_TYPE_PLAYER)
        return;

    for(int i = 0; i < vec_size(sel); i++) {

        struct entity *curr = vec_AT(sel, i);
        if(!(curr->flags & ENTITY_FLAG_HARVESTER))
            continue;

        struct hstate *hs = hstate_get(curr->uid);
        assert(hs);

        if(G_Harvester_GetCurrTotalCarry(curr->uid) == 0)
            continue;

        finish_harvesting(hs, curr->uid);
        G_Harvester_DropOff(curr, target);
        ngather++;
    }

    if(ngather) {
        Entity_Ping(target);
    }
}

static void selection_try_order_transport(void)
{
    if(G_CurrContextualAction() != CTX_ACTION_TRANSPORT)
        return;

    struct entity *target = G_Sel_GetHovered();
    assert(target && (target->flags & ENTITY_FLAG_STORAGE_SITE));

    enum selection_type sel_type;
    const vec_pentity_t *sel = G_Sel_Get(&sel_type);
    size_t ntransport = 0;

    if(sel_type != SELECTION_TYPE_PLAYER)
        return;

    for(int i = 0; i < vec_size(sel); i++) {

        struct entity *curr = vec_AT(sel, i);
        if(!(curr->flags & ENTITY_FLAG_HARVESTER))
            continue;

        struct hstate *hs = hstate_get(curr->uid);
        assert(hs);

        finish_harvesting(hs, curr->uid);
        G_Harvester_Transport(curr, target);
        ntransport++;
    }

    if(ntransport) {
        Entity_Ping(target);
    }
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);

    bool targeting = G_Harvester_InTargetMode();
    bool right = (mouse_event->button == SDL_BUTTON_RIGHT);
    bool left = (mouse_event->button == SDL_BUTTON_LEFT);

    s_gather_on_lclick = false;
    s_drop_off_on_lclick = false;
    s_transport_on_lclick = false;

    if(G_MouseOverMinimap())
        return;

    if(S_UI_MouseOverWindow(mouse_event->x, mouse_event->y))
        return;

    if(right && targeting)
        return;

    if(left && !targeting)
        return;

    int action = G_CurrContextualAction();

    if(right 
    && (action != CTX_ACTION_GATHER) 
    && (action != CTX_ACTION_DROP_OFF) 
    && (action != CTX_ACTION_TRANSPORT))
        return;

    selection_try_order_gather();
    selection_try_order_drop_off();
    selection_try_order_transport();
}

static const char *transport_resource(struct hstate *hs, struct entity *target)
{
    const char *ret = NULL;
    for(int i = 0; i < vec_size(&hs->priority); i++) {

        const char *rname = vec_AT(&hs->priority, i);
        int desired = G_StorageSite_GetDesired(target->uid, rname);
        int stored = G_StorageSite_GetCurr(target->uid, rname);

        if(desired > stored) {
            ret = rname;
            break;
        }
    }
    return ret;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Harvester_Init(const struct map *map)
{
    mp_buff_init(&s_mpool);

    if(!mp_buff_reserve(&s_mpool, 4096 * 3))
        goto fail_mpool; 
    if(!(s_entity_state_table = kh_init(state)))
        goto fail_table;
    if(!si_init(&s_stringpool, &s_stridx, 512))
        goto fail_strintern;

    s_map = map;
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    return true;

fail_strintern:
    kh_destroy(state, s_entity_state_table);
fail_table:
    mp_buff_destroy(&s_mpool);
fail_mpool:
    return false;
}

void G_Harvester_Shutdown(void)
{
    s_map = NULL;
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);

    si_shutdown(&s_stringpool, s_stridx);
    kh_destroy(state, s_entity_state_table);
    mp_buff_destroy(&s_mpool);
}

bool G_Harvester_AddEntity(uint32_t uid)
{
    struct hstate hs;
    if(!hstate_init(&hs))
        return false;
    if(!hstate_set(uid, hs))
        return false;
    return true;
}

void G_Harvester_RemoveEntity(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    if(!hs)
        return;

    G_Harvester_Stop(uid);
    hstate_destroy(hs);
    hstate_remove(uid);
}

bool G_Harvester_SetGatherSpeed(uint32_t uid, const char *rname, int speed)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    return hstate_set_key(hs->gather_speeds, rname, speed);
}

int G_Harvester_GetGatherSpeed(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_GATHER_SPEED;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    hstate_get_key(hs->gather_speeds, rname, &ret);
    return ret;
}

bool G_Harvester_SetMaxCarry(uint32_t uid, const char *rname, int max)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return false;

    if(max == 0) {
        hstate_remove_prio(hs, key);
    }else{
        hstate_insert_prio(hs, key);
    }
    return hstate_set_key(hs->max_carry, rname, max);
}

int G_Harvester_GetMaxCarry(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_MAX_CARRY;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    hstate_get_key(hs->max_carry, rname, &ret);
    return ret;
}

bool G_Harvester_SetCurrCarry(uint32_t uid, const char *rname, int curr)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    return hstate_set_key(hs->curr_carry, rname, curr);
}

int G_Harvester_GetCurrCarry(uint32_t uid, const char *rname)
{
    int ret = 0;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    hstate_get_key(hs->curr_carry, rname, &ret);
    return ret;
}

void G_Harvester_SetStrategy(uint32_t uid, enum tstrategy strat)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    hs->strategy = strat;
}

int G_Harvester_GetStrategy(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    return hs->strategy;
}

bool G_Harvester_IncreaseTransportPrio(uint32_t uid, const char *rname)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return false;

    int idx = vec_name_indexof(&hs->priority, key, compare_keys);
    if(idx == -1)
        return false;
    if(idx == 0)
        return false;

    const char *tmp = vec_AT(&hs->priority, idx - 1);
    vec_AT(&hs->priority, idx - 1) = key;
    vec_AT(&hs->priority, idx) = tmp;

    return true;
}

bool G_Harvester_DecreaseTransportPrio(uint32_t uid, const char *rname)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return false;

    int idx = vec_name_indexof(&hs->priority, key, compare_keys);
    if(idx == -1)
        return false;
    if(idx == vec_size(&hs->priority) - 1)
        return false;

    const char *tmp = vec_AT(&hs->priority, idx + 1);
    vec_AT(&hs->priority, idx + 1) = key;
    vec_AT(&hs->priority, idx) = tmp;

    return true;
}

int G_Harvester_GetTransportPrio(uint32_t uid, size_t maxout, const char *out[static maxout])
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    size_t ret = MIN(maxout, vec_size(&hs->priority));

    memcpy(out, hs->priority.array, ret * sizeof(const char*));
    return ret;
}

int G_Harvester_GetCurrTotalCarry(uint32_t uid)
{
    int ret = 0;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    const char *key;
    (void)key;
    int curr;

    kh_foreach(hs->curr_carry, key, curr, {
        ret += curr;
    });

    return ret;
}

void G_Harvester_SetGatherOnLeftClick(void)
{
    s_gather_on_lclick  = true;
    s_drop_off_on_lclick = false;
    s_transport_on_lclick = false;
}

void G_Harvester_SetDropOffOnLeftClick(void)
{
    s_gather_on_lclick  = false;
    s_drop_off_on_lclick = true;
    s_transport_on_lclick = false;
}

void G_Harvester_SetTransportOnLeftClick(void)
{
    s_gather_on_lclick  = false;
    s_drop_off_on_lclick = false;
    s_transport_on_lclick = true;
}

bool G_Harvester_Gather(struct entity *harvester, struct entity *resource)
{
    struct hstate *hs = hstate_get(harvester->uid);
    assert(hs);

    if(!(resource->flags & ENTITY_FLAG_RESOURCE))
        return false;

    if(G_Harvester_GetCurrTotalCarry(harvester->uid)) {

        const char *carryname = carried_resource_name(hs);
        if(strcmp(carryname, G_Resource_GetName(resource->uid))) {
        
            struct entity *target = nearest_storage_site_dropff(harvester, carryname);
            hs->queued.cmd = CMD_GATHER;
            hs->queued.uid_arg = resource->uid;

            G_Harvester_DropOff(harvester, target);
            return true;
        }
    }

    E_Entity_Register(EVENT_MOTION_END, harvester->uid, on_arrive_at_resource, 
        (void*)((uintptr_t)harvester->uid), G_RUNNING);
    E_Entity_Register(EVENT_MOVE_ISSUED, harvester->uid, on_motion_begin_travel, 
        (void*)((uintptr_t)harvester->uid), G_RUNNING);

    G_Move_SetSurroundEntity(harvester, resource);

    hs->state = STATE_HARVESTING_SEEK_RESOURCE;
    hs->res_uid = resource->uid;
    hs->res_last_pos = G_Pos_GetXZ(resource->uid);
    hs->res_name = G_Resource_GetName(resource->uid);

    E_Entity_Notify(EVENT_HARVEST_TARGET_ACQUIRED, harvester->uid, resource, ES_ENGINE);
    return true;
}

bool G_Harvester_DropOff(struct entity *harvester, struct entity *storage)
{
    struct hstate *hs = hstate_get(harvester->uid);
    assert(hs);

    if(!(storage->flags & ENTITY_FLAG_STORAGE_SITE))
        return false;

    if(G_Harvester_GetCurrTotalCarry(harvester->uid) == 0)
        return true;

    hs->drop_off_only = true;
    entity_drop_off(harvester, storage);
    return true;
}

bool G_Harvester_Transport(struct entity *harvester, struct entity *storage)
{
    struct hstate *hs = hstate_get(harvester->uid);
    assert(hs);

    if(!(storage->flags & ENTITY_FLAG_STORAGE_SITE))
        return false;

    if(G_Harvester_GetCurrTotalCarry(harvester->uid)) {

        const char *carryname = carried_resource_name(hs);
        struct entity *nearest = nearest_storage_site_dropff(harvester, carryname);

        hs->queued.cmd = CMD_TRANSPORT;
        hs->queued.uid_arg = storage->uid;

        G_Harvester_DropOff(harvester, nearest);
        return true;
    }

    const char *rname = transport_resource(hs, storage);
    if(!rname)
        return false;

    struct entity *src = transport_source(harvester, storage, rname);
    if(!src)
        return false;

    E_Entity_Register(EVENT_MOTION_END, harvester->uid, on_arrive_at_transport_source, 
        (void*)((uintptr_t)harvester->uid), G_RUNNING);
    E_Entity_Register(EVENT_MOVE_ISSUED, harvester->uid, on_motion_begin_travel, 
        (void*)((uintptr_t)harvester->uid), G_RUNNING);

    G_Move_SetSurroundEntity(harvester, src);

    hs->state = STATE_TRANSPORT_GETTING;
    hs->transport_dest_uid = storage->uid;
    hs->transport_src_uid = src->uid;
    hs->res_name = rname;

    E_Entity_Notify(EVENT_TRANSPORT_TARGET_ACQUIRED, harvester->uid, storage, ES_ENGINE);
    return true;
}

void G_Harvester_Stop(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    hs->state = STATE_NOT_HARVESTING;

    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished);
    E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin_harvest);
    E_Entity_Unregister(EVENT_MOVE_ISSUED, uid, on_motion_begin_travel);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_resource);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_storage);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_transport_source);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_transport_dest);
}

bool G_Harvester_InTargetMode(void)
{
    return s_gather_on_lclick 
        || s_drop_off_on_lclick 
        || s_transport_on_lclick;
}

bool G_Harvester_HasRightClickAction(void)
{
    struct entity *hovered = G_Sel_GetHovered();
    if(!hovered)
        return false;

    enum selection_type sel_type;
    const vec_pentity_t *sel = G_Sel_Get(&sel_type);

    if(vec_size(sel) == 0)
        return false;

    const struct entity *first = vec_AT(sel, 0);
    if(first->flags & ENTITY_FLAG_HARVESTER && hovered->flags & ENTITY_FLAG_RESOURCE)
        return true;

    return false;
}

int G_Harvester_CurrContextualAction(void)
{
    struct entity *hovered = G_Sel_GetHovered();
    if(!hovered)
        return CTX_ACTION_NONE;

    if((hovered->flags & ENTITY_FLAG_BUILDING)
    && !G_Building_IsCompleted(hovered))
        return CTX_ACTION_NONE;

    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    if(S_UI_MouseOverWindow(mouse_x, mouse_y))
        return CTX_ACTION_NONE;

    if(G_Harvester_InTargetMode())
        return CTX_ACTION_NONE;

    enum selection_type sel_type;
    const vec_pentity_t *sel = G_Sel_Get(&sel_type);

    if(vec_size(sel) == 0 || sel_type != SELECTION_TYPE_PLAYER)
        return CTX_ACTION_NONE;

    const struct entity *first = vec_AT(sel, 0);
    if(!(first->flags & ENTITY_FLAG_HARVESTER))
        return CTX_ACTION_NONE;

    if(hovered->flags & ENTITY_FLAG_RESOURCE
    && G_Harvester_GetGatherSpeed(first->uid, G_Resource_GetName(hovered->uid)) > 0)
        return CTX_ACTION_GATHER;

    if(hovered->flags & ENTITY_FLAG_STORAGE_SITE
    && G_Harvester_GetCurrTotalCarry(first->uid) > 0)
        return CTX_ACTION_DROP_OFF;

    if(hovered->flags & ENTITY_FLAG_STORAGE_SITE)
        return CTX_ACTION_TRANSPORT;

    return CTX_ACTION_NONE;
}

bool G_Harvester_GetContextualCursor(char *out, size_t maxout)
{
    struct entity *hovered = G_Sel_GetHovered();
    if(!hovered)
        return false;

    if(!(hovered->flags & ENTITY_FLAG_RESOURCE))
        return false;

    const char *name = G_Resource_GetCursor(hovered->uid);
    pf_strlcpy(out, name, maxout);
    return true;
}
