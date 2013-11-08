/********************************************************************
* Description: tp.c
*   Trajectory planner based on TC elements
*
*   Derived from a work by Fred Proctor & Will Shackleford
*
* Author:
* License: GPL Version 2
* System: Linux
*
* Copyright (c) 2004 All rights reserved.
********************************************************************/

#include "rtapi.h"		/* rtapi_print_msg */
#include "rtapi_string.h"       /* NULL */
#include "posemath.h"
#include "tc.h"
#include "tp.h"
#include "rtapi_math.h"
#include "../motion/motion.h"
#include "hal.h"
#include "../motion/mot_priv.h"
#include "motion_debug.h"
#include "motion_types.h"

#ifndef SIM
static inline double fmax(double a, double b) { return (a) > (b) ? (a) : (b); }
static inline double fmin(double a, double b) { return (a) < (b) ? (a) : (b); }
#endif

#define TP_DEBUG
#define TC_DEBUG
#include "tp_debug.h"

extern emcmot_status_t *emcmotStatus;
extern emcmot_debug_t *emcmotDebug;

int output_chan = 0;
syncdio_t syncdio; //record tpSetDout's here


STATIC double tpComputeBlendVelocity(TP_STRUCT const * const tp, TC_STRUCT const * const tc, TC_STRUCT const * const nexttc);


/**
 * Get a TC's feed rate override based on emcmotStatus.
 * This function is designed to eliminate duplicate states, since this leads to bugs.
 * TODO: move to tc?
 */
static double tpGetFeedOverride(TP_STRUCT const * const tp, TC_STRUCT const * const tc) {
    //All reasons to disable feed override go here
    if (tc->canon_motion_type == EMC_MOTION_TYPE_TRAVERSE || tc->synchronized == TC_SYNC_POSITION)  {
        return 1.0;
    } else if (tp->pausing || tp->aborting) {
        return 0.0;
    } else {
        return emcmotStatus->net_feed_scale;
    }
}


static double tpGetReqVel(TP_STRUCT const * const tp, TC_STRUCT const * const tc) {
    return tc->reqvel * tpGetFeedOverride(tp,tc);
}

static double tpGetFinalVel(TP_STRUCT const * const tp, TC_STRUCT const * const tc) {
    return tc->finalvel * tpGetFeedOverride(tp,tc);
}

static inline double tpGetScaledAccel(TP_STRUCT const * const tp, TC_STRUCT const * const tc) {
    if (tc->accel_scale < 0.0) {
        //TODO silent error ok?
        return 0.0;
    }
    return tc->maxaccel * tc->accel_scale;
}

static int tpClipVelocityLimit(TP_STRUCT const * const tp, TC_STRUCT * const tc) {
    // Nyquist-like velocity limits
    double sample_maxvel = 0.5 * tc->target / tp->cycleTime;
    if (tc->maxvel > sample_maxvel) {
        tp_debug_print("Clipped maxvel from %f to %f in tc #%d\n",tc->maxvel,sample_maxvel,tc->id);
        tc->maxvel = sample_maxvel;
    }
    return 0;
}


/**
 * Convert the 2-part spindle position and sign to a signed double.
 */
STATIC inline double tpGetSignedSpindlePosition(double spindle_pos, int spindle_dir) {
    if (spindle_dir < 0.0) {
        spindle_pos*=-1.0;
    }
    return spindle_pos;
}

/**
 * Create the trajectory planner structure with an empty queue.
 */
int tpCreate(TP_STRUCT * const tp, int _queueSize, TC_STRUCT * const tcSpace)
{
    if (0 == tp) {
        return -1;
    }

    if (_queueSize <= 0) {
        tp->queueSize = TP_DEFAULT_QUEUE_SIZE;
    } else {
        tp->queueSize = _queueSize;
    }

    /* create the queue */
    if (-1 == tcqCreate(&tp->queue, tp->queueSize, tcSpace)) {
        return -1;
    }

    /* init the rest of our data */
    return tpInit(tp);
}

/**
 * Clears any potential DIO toggles and anychanged.
 * If any DIOs need to be changed: dios[i] = 1, DIO needs to get turned on, -1
 * = off
 */
int tpClearDIOs() {
    //XXX: All IO's will be flushed on next synced aio/dio! Is it ok?
    int i;
    syncdio.anychanged = 0;
    syncdio.dio_mask = 0;
    syncdio.aio_mask = 0;
    for (i = 0; i < num_dio; i++)
        syncdio.dios[i] = 0;
    for (i = 0; i < num_aio; i++)
        syncdio.aios[i] = 0;

    return 0;
}

/**
 *    "Soft initialize" the trajectory planner tp.
 *    This is a "soft" initialization in that TP_STRUCT configuration
 *    parameters (cycleTime, vMax, and aMax) are left alone, but the queue is
 *    cleared, and the flags are set to an empty, ready queue. The currentPos
 *    is left alone, and goalPos is set to this position.  This function is
 *    intended to put the motion queue in the state it would be if all queued
 *    motions finished at the current position.
 */
int tpClear(TP_STRUCT * const tp)
{
    tcqInit(&tp->queue);
    tp->queueSize = 0;
    tp->goalPos = tp->currentPos;
    tp->nextId = 0;
    tp->execId = 0;
    tp->motionType = 0;
    tp->termCond = TC_TERM_COND_PARABOLIC;
    tp->tolerance = 0.0;
    tp->done = 1;
    tp->depth = tp->activeDepth = 0;
    tp->aborting = 0;
    tp->pausing = 0;
    tp->vScale = emcmotStatus->net_feed_scale;
    tp->synchronized = 0;
    tp->uu_per_rev = 0.0;
    emcmotStatus->spindleSync = 0;
    emcmotStatus->current_vel = 0.0;
    emcmotStatus->requested_vel = 0.0;
    emcmotStatus->distance_to_go = 0.0;
    ZERO_EMC_POSE(emcmotStatus->dtg);

    return tpClearDIOs();
}

/**
 * Fully initialize the tp structure.
 * Sets tp configuration to default values and calls tpClear to create a fresh,
 * empty queue.
 */
int tpInit(TP_STRUCT * const tp)
{
    tp->cycleTime = 0.0;
    tp->vLimit = 0.0;
    tp->vScale = 1.0;
    tp->aMax = 0.0;
    tp->vMax = 0.0;
    tp->ini_maxvel = 0.0;
    tp->wMax = 0.0;
    tp->wDotMax = 0.0;

    tp->spindle.offset = 0.0;
    tp->spindle.revs = 0.0;
    tp->spindle.waiting_for_index = MOTION_INVALID_ID;
    tp->spindle.waiting_for_atspeed = MOTION_INVALID_ID;

    ZERO_EMC_POSE(tp->currentPos);

    return tpClear(tp);
}

/**
 * Set the cycle time for the trajectory planner.
 */
int tpSetCycleTime(TP_STRUCT * const tp, double secs)
{
    if (0 == tp || secs <= 0.0) {
        return -1;
    }

    tp->cycleTime = secs;

    return 0;
}

/**
 * Set requested velocity and absolute maximum velocity (bounded by machine).
 * This is called before adding lines or circles, specifying vMax (the velocity
 * requested by the F word) and ini_maxvel, the max velocity possible before
 * meeting a machine constraint caused by an AXIS's max velocity.  (the TP is
 * allowed to go up to this high when feed override >100% is requested)  These
 * settings apply to subsequent moves until changed.
 */
int tpSetVmax(TP_STRUCT * const tp, double vMax, double ini_maxvel)
{
    if (0 == tp || vMax <= 0.0 || ini_maxvel <= 0.0) {
        return -1;
    }

    tp->vMax = vMax;
    tp->ini_maxvel = ini_maxvel;

    return 0;
}

/**
 * (?) Set the tool tip maximum velocity.
 * I think this is the [TRAJ] max velocity. This should be the max velocity of
 * const the TOOL TIP, not necessarily any particular axis. This applies to
 * subsequent moves until changed.
 */
int tpSetVlimit(TP_STRUCT * const tp, double vLimit)
{
    if (!tp) return -1;

    if (vLimit < 0.)
        tp->vLimit = 0.;
    else
        tp->vLimit = vLimit;

    return 0;
}

/** Sets the max acceleration for the trajectory planner. */
int tpSetAmax(TP_STRUCT * const tp, double aMax)
{
    if (0 == tp || aMax <= 0.0) {
        return -1;
    }

    tp->aMax = aMax;

    return 0;
}

/**
 * Sets the id that will be used for the next appended motions.
 * nextId is incremented so that the next time a motion is appended its id will
 * be one more than the previous one, modulo a signed int. If you want your own
 * ids for each motion, call this before each motion you append and stick what
 * you want in here.
 */
int tpSetId(TP_STRUCT * const tp, int id)
{

    if (!MOTION_ID_VALID(id)) {
        rtapi_print_msg(RTAPI_MSG_ERR, "tpSetId: invalid motion id %d\n", id);
        return -1;
    }

    if (0 == tp) {
        return -1;
    }

    tp->nextId = id;

    return 0;
}

/** Returns the id of the last motion that is currently
  executing.*/
int tpGetExecId(TP_STRUCT * const tp)
{
    if (0 == tp) {
        return -1;
    }

    return tp->execId;
}

/**
 * Sets the termination condition for all subsequent queued moves.
 * If cond is TC_TERM_COND_STOP, motion comes to a stop before a subsequent move
 * begins. If cond is TC_TERM_COND_PARABOLIC, the following move is begun when the
 * current move decelerates.
 */
int tpSetTermCond(TP_STRUCT * const tp, int cond, double tolerance)
{
    if (0 == tp) {
        return -1;
    }

    switch (cond) {
        //Purposeful waterfall for now
        case TC_TERM_COND_PARABOLIC:
        case TC_TERM_COND_TANGENT:
        case TC_TERM_COND_STOP:
            tp->termCond = cond;
            tp->tolerance = tolerance;
            break;
        default:
            //Invalid condition
            return  -1;
    }

    return 0;
}

/**
 * Used to tell the tp the initial position.
 * It sets the current position AND the goal position to be the same.  Used
 * only at TP initialization and when switching modes.
 */
int tpSetPos(TP_STRUCT * const tp, EmcPose pos)
{
    if (0 == tp) {
        return -1;
    }

    tp->currentPos = pos;
    tp->goalPos = pos;

    return 0;
}

/**
 * Check for valid tp before queueing additional moves.
 */
int tpErrorCheck(TP_STRUCT const * const tp) {

    if (!tp) {
        rtapi_print_msg(RTAPI_MSG_ERR, "TP is null\n");
        return -1;
    }
    if (tp->aborting) {
        rtapi_print_msg(RTAPI_MSG_ERR, "TP is aborting\n");
        return -1;
    }
    return 0;
}


/** 
 * Break out a 9D EmcPose structure into 3 PmCartesian pieces for processing.
 * This functiona assumes that we're not using the rotation component for
 * anything, so it just treats ABC and UVW as additional orthogonal axes. If
 * NULL is passed for any of the pointers, then that component is unassigned.
 */
STATIC inline void tpConvertEmcPosetoPmCartesian(EmcPose const * const pose, PmCartesian * const xyz, PmCartesian * const
        abc, PmCartesian * const uvw) {

    //Direct copy of translation struct for xyz
    if (xyz) {
        *xyz = pose->tran;
    }

    //Convert ABCUVW axes into 2 pairs of 3D lines
    if (abc) {
        abc->x = pose->a;
        abc->y = pose->b;
        abc->z = pose->c;
    }

    if (uvw) {
        uvw->x = pose->u;
        uvw->y = pose->v;
        uvw->z = pose->w;
    }

}


/** 
 * Collect PmCartesian elements into 9D EmcPose structure.
 * TODO: move this to posemath
 */
STATIC inline void tpConvertPmCartesiantoEmcPose(PmCartesian const * const xyz, PmCartesian const * const abc, PmCartesian const * const uvw, EmcPose * const pose) {

    pose->tran = *xyz;

    pose->a = abc->x;
    pose->b = abc->y;
    pose->c = abc->z;
    pose->u = uvw->x;
    pose->v = uvw->y;
    pose->w = uvw->z;

}


/**
 * Initialize a new queue segment with common parameters.
 * This function is mostly to save space in the tpAddXXX functions, since they
 * get pretty long. If you need a custom setting, overwrite your particular
 * field after calling this function.
 */
STATIC inline void tpInitializeNewSegment(TP_STRUCT const * const tp,
        TC_STRUCT * const tc, double vel, double ini_maxvel, double acc,
        unsigned char enables) {

    tc->sync_accel = 0;
    tc->cycle_time = tp->cycleTime;
    tc->id = -1; //ID to be set when added to queue

    tc->progress = 0.0;
    tc->maxaccel = acc;
    tc->maxvel = ini_maxvel;
    //Note: capping reqvel here since maxvel never changes for a given segment
    tc->reqvel = fmin(vel,ini_maxvel);
#ifdef TP_CHECK_MORE
    if (tc->reqvel <= 0) {
        tp_debug_print(" Requested velocity %f of TC id %u is <= 0.0!\n",tc->reqvel,tc->id);
    }

#endif

    tc->active = 0;

    tc->currentvel = 0.0;
    tc->blending = 0;
    tc->blend_vel = 0.0;
    tc->vel_at_blend_start = 0.0;
    tc->finalvel = 0.0;

    tc->enables=enables;

    tc->atpeak=0;
    tc->accel_scale=1.0;
}

/**
 * Find the maximum angle allowed between "tangent" segments.
 * Since we are discretized by a timestep, the maximum allowable
 * "kink" in a trajectory is bounded by normal acceleration. A small
 * kink will effectively be one step along the tightest radius arc
 * possible at a given speed.
 */
STATIC inline double tpMaxTangentAngle(double v, double acc, double period) {
    double dx = v / period;
    // Hand-wavy constant to be more conservative.
    // TODO calculate / experimentally determine if we need this
    const double safety_factor = 10.0;

    if (dx > 0.0) {
        return ((acc / dx) / safety_factor);
    } else {
        tp_debug_print(" Velocity or period is negative!\n");
        //Should not happen...
        return TP_ANGLE_EPSILON;
    }
}


/**
 * Somewhat redundant function to calculate the segment intersection angle.
 * This is PI - the acute angle between the unit vectors.
 */
STATIC inline int tpFindIntersectionAngle(PmCartesian const * const u1, PmCartesian const * const u2, double * const theta) {
    double dot;
    pmCartCartDot(u1, u2, &dot);

    tp_debug_print("u1 = %f %f %f u2 = %f %f %f\n", u1->x, u1->y, u1->z, u2->x, u2->y, u2->z);

    if (dot > 1.0 || dot < -1.0) {
        return -1;
    }

    *theta = acos(-dot)/2.0;
    return 0;
}


/**
 * Calculate the angle between two unit cartesian vectors.
 * TODO Make this a posemath function?
 */
STATIC inline int tpCalculateUnitCartAngle(PmCartesian const * const u1, PmCartesian const * const u2, double * const theta) {
    double dot;
    pmCartCartDot(u1, u2, &dot);

    if (dot > 1.0 || dot < -1.0) return -1;

    *theta = acos(dot);
    return 0;
}

/**
 * Apply calculated blend arc parameters to a TC.
 * @param start is the starting point of the arc as calculated by the blend routine.
 * @param end is the end point of the arc on the next move.
 * @param 
 *
 * see pmSphericalArcInit for further details on how arcs are specified. Note that
 * degenerate arcs/circles are not allowed. We are guaranteed to have a move in
 * xyz so the target is always the length.
 */
STATIC int tpApplyBlendArcParameters(TP_STRUCT const * const tp, TC_STRUCT * const blend_tc, double vel)
{
    double length;
    if (tpErrorCheck(tp)<0) return -1;
    // find "helix" length
    length = blend_tc->coords.circle.xyz.angle * blend_tc->coords.circle.xyz.radius;
    blend_tc->target = length;
    //TODO acceleration limits tweaked by optimization
    blend_tc->motion_type = TC_CIRCULAR;
    //Blend arc specific settings:
    blend_tc->term_cond = TC_TERM_COND_TANGENT;
    blend_tc->tolerance = 0.0;
    blend_tc->reqvel = vel;
    return 0;
}

/**
 * Initialize a spherical blend arc from its parent lines.
 */
STATIC int tpInitBlendArc(TP_STRUCT const * const tp, TC_STRUCT const * const prev_line_tc,
        TC_STRUCT* const tc) {

    if (tpErrorCheck(tp)<0) return -1;

    // Treating arc as extension of prev_line_tc
    tc->enables = prev_line_tc->enables;
    tc->atspeed = prev_line_tc->atspeed;

    //KLUDGE this init function is a bit overkill now...
    tpInitializeNewSegment(tp, tc, 0, prev_line_tc->maxvel, 0, 0);

    tc->motion_type = TC_CIRCULAR;
    tc->canon_motion_type = EMC_MOTION_TYPE_ARC;

    tc->synchronized = prev_line_tc->synchronized;
    tc->uu_per_rev = prev_line_tc->uu_per_rev;
    tc->indexrotary = -1;
    tc->enables = prev_line_tc->enables;

    //FIXME do we need this in a blend arc?
    if (syncdio.anychanged != 0) {
        tc->syncdio = syncdio; //enqueue the list of DIOs that need toggling
        tpClearDIOs(); // clear out the list, in order to prepare for the next time we need to use it
    } else {
        tc->syncdio.anychanged = 0;
    }

    return 0;
}


/**
 * Compute arc segment to blend between two lines.
 */
STATIC int tpCreateBlendArc(TP_STRUCT const * const tp, TC_STRUCT * const prev_tc,
        TC_STRUCT  * const tc, TC_STRUCT * const blend_tc) {

    // Assume at this point that we've checked for dumb reasons not to
    // calculate the blend arc, like intersection angle
    // Calculate radius based on tolerances
    double theta=0.0;
    int res = tpFindIntersectionAngle(&prev_tc->coords.line.xyz.uVec, &tc->coords.line.xyz.uVec, &theta);
    if (res) {
        //Can't get an intersection angle, bail
        tp_debug_print("Failed to find intersection angle!\n");
        return -1;
    }
    tp_debug_print("theta=%f\n",theta);

    //TODO make this a state of TC?
    const double acc_ratio=1;
    //Find common velocity and acceleration
    double v_req=fmax(prev_tc->reqvel, tc->reqvel);
    tp_debug_print("vr1 = %f, vr2 = %f\n",prev_tc->reqvel,tc->reqvel);
    tp_debug_print("v_req=%f\n",v_req);
    
    //Want the raw accelerations here
    double a_max=fmin(prev_tc->maxaccel, tc->maxaccel);
    tp_debug_print("a_max=%f\n",a_max);
    // Hack to give us a little room to play with d_upper later
    // FIXME formally prove the min. safety factor needed
    double acc_safety_factor=.98;
    double a_n_max=a_max/pmSqrt(1.0+1.0/pmSq(acc_ratio)) * acc_safety_factor;
    blend_tc->accel_scale=1/pmSqrt(1.0+pmSq(acc_ratio));

    blend_tc->maxaccel=a_max;

    //Get 3D start, middle, end position
    PmCartesian start, middle, end;
    start = prev_tc->coords.line.xyz.start;
    middle = prev_tc->coords.line.xyz.end;
    end = tc->coords.line.xyz.end;

    //Find the minimum tolerance (in case it dropped between moves)
    double T1=prev_tc->tolerance;
    double T2=tc->tolerance;
    if ( 0.0 == T1) T1=10000000;
    if ( 0.0 == T2) T2=10000000;

    double tolerance=fmin(T1,T2);
    tp_debug_print(" Blend Tolerance = %f\n",tolerance);

    //Store trig functions for later use
    double Ctheta=cos(theta);
    double Stheta=sin(theta);
    double Ttheta=tan(theta);
    
    double tmp = 1.0 - Stheta;
    double h_tol;
    if (tmp>TP_ANGLE_EPSILON) {
        h_tol=tolerance/tmp;
    } else {
        tp_debug_print("h_tol too large! theta = %f\n",theta);
        return -1;
    }

    double d_tol=Ctheta*h_tol;

    // Limit amount of line segment to blend so that we don't delete the line
    const double blend_ratio = 0.5;

    //HACK Assume that we are not working on segments already traversed for now
    double L1 = prev_tc->target;
    double L2 = tc->target;

    double d_prev = L1 * 1.0; //Blend over the whole previous segment
    double d_next = L2 * blend_ratio; //Blend over a portion of the next

    /*double v_sample = (L1+L2) / (3.0 * tp->cycleTime);*/

    double d_geom=fmin(fmin(d_prev, d_next), d_tol);
    tp_debug_print("d_geom = %f, d_prev = %f, d_next = %f\n",d_geom,d_prev,d_next);

    double R_geom=Ttheta * d_geom;
    tp_debug_print("R_geom = %f\n",R_geom);

    //Calculate limiting velocity due to radius and normal acceleration
    double v_normal=pmSqrt(a_n_max*R_geom);

    //The new upper bound is the lower of the two speeds
    double v_upper=fmin(v_req, v_normal);
    tp_debug_print("v_normal = %f\n",v_normal); 
    tp_debug_print("v_upper = %f\n",v_upper); 

    //At this new limiting velocity, find the radius by the reverse formula

    double R_normal;
    if (a_n_max>TP_ACCEL_EPSILON) {
        R_normal=pmSq(v_upper)/a_n_max;
    } else {
        R_normal=0.0;
        tp_debug_print("a_n_max = %f, too low!\n",a_n_max);
        return -1;
    }

    // Final radius calculations
    double R_upper=fmin(R_normal, R_geom);
    tp_debug_print("R_upper = %f\n",R_upper); 

    //Check for segment length limits
    //TODO div by zero
    double d_upper = R_upper / Ttheta;
    double phi = (PM_PI- theta * 2.0);
    double s_arc = phi * R_upper;
    double L_prev = L1 - d_upper;
    double L_next = L2 - d_upper;

    if (L_prev < -TP_MAG_EPSILON) {
        //Something is very wrong here...
        tp_debug_print("Cannot have negative segment length!\n");
        return -1;
    } else if (L_prev < TP_MAG_EPSILON) {
        //Force the prev. segment to be consumed since we're otherwise going to
        //cause degeneracy
        d_upper += L_prev;
    } else {
        // Test if our ideal lower bound on d (from arc equation) is lower than
        // our ideal upper bound on d based on the sample time. Usually there
        // will be overlap, and we can proceed as normal. If there isn't, then
        // we have short segments, and need to compromise on segment length to
        // avoid degeneracy.
        
        double v_sample = phi * d_upper * Ttheta / tp->cycleTime;

        // The blend velocity we can actually get is limited by the sample rate
        v_upper = fmin(v_upper,v_sample);

        // d required to meet v_upper
        double d_sample = v_upper * tp->cycleTime / (phi * Ttheta);

        double v1_sample = (L1-d_sample) / tp->cycleTime;

        //If we take too big a bite out of the previous line, we won't be able
        //to move fast enough through the segment to reach v_upper anyway.
        //Compromise if this is an issue:
        if (v1_sample < v_upper) {
            d_upper = L1/(1+phi*Ttheta);
            //FIXME variable reuse
            v_upper = v1_sample;
        } 
        tp_debug_print("Adjusted v_upper = %f, d_upper = %f\n",v_upper,d_upper);
    }

    R_upper = d_upper*Ttheta;
    tp_debug_print("adjusted R_upper = %f\n",R_upper); 
    tp_debug_print("effective a_n = %f\n", pmSq(v_upper)/R_upper); 

    tp_debug_print("arc length = %f, L_prev = %f, L_next = %f\n", s_arc, L_prev, L_next);

    /* Additional quality / performance checks.
     * If for whatever reason we can't get parabolic-equivalent performance (by
     * checking against the parabolic velocity), then abort arc creation and
     * fall back to parabolic blends */
    
    //FIXME hack to get proper accelerations for this calculation
    double v_parabolic = tpComputeBlendVelocity(tp, prev_tc, tc);

    tp_debug_print("Speed Comparison: v_arc %f, v_para %f\n",v_upper,v_parabolic);
    if (v_upper < v_parabolic) {
        tp_debug_print("v_arc lower, abort arc creation\n");
        return -1;
    }

    //If for some reason we get too small a radius, the blend will fail. This
    //shouldn't happen if everything upstream is working.
    if (R_upper < TP_MAG_EPSILON) {
        tp_debug_print("Blend radius too small, aborting...\n");
        return -1;
    }

    //TEMPORARY - always fail here to check degradation
    return -1;

    tpInitBlendArc(tp, prev_tc, blend_tc);

    //TODO Recycle calculations?
    //TODO errors within this function
    pmCircleFromPoints(&blend_tc->coords.circle.xyz, &start, &middle, &end, R_upper);

    tp_debug_print("angle = %f\n",blend_tc->coords.circle.xyz.angle); 

    tpApplyBlendArcParameters(tp, blend_tc, v_upper);

    //TODO setup arc params in blend_tc
    return 0;
}


/**
 * Add a newly created motion segment to the tp queue.
 * Returns an error code if the queue operation fails, otherwise adds a new
 * segment to the queue and updates the end point of the trajectory planner.
 */
STATIC inline int tpAddSegmentToQueue(TP_STRUCT * const tp, TC_STRUCT * const tc, EmcPose const * const end) {

    tc->id=tp->nextId;
    if (tcqPut(&tp->queue, tc) == -1) {
        rtapi_print_msg(RTAPI_MSG_ERR, "tcqPut failed.\n");
        return -1;
    }

    // Store end of current move as new final goal of TP
    tp->goalPos = *end;
    tp->done = 0;
    tp->depth = tcqLen(&tp->queue);
    //Fixing issue with duplicate id's?
    tp_debug_print("Adding TC id %d of type %d\n",tc->id,tc->motion_type);
    tp->nextId++;

    return 0;
}


/**
 * Adds a rigid tap cycle to the motion queue.
 */
int tpAddRigidTap(TP_STRUCT * const tp, EmcPose end, double vel, double ini_maxvel,
        double acc, unsigned char enables) {
    TC_STRUCT tc;
    PmCartLine line_xyz;
    PmCartesian start_xyz, end_xyz;
    PmCartesian abc, uvw;

    if (tpErrorCheck(tp)) return -1;

    //Slightly more allocation this way, but much easier to read
    tpConvertEmcPosetoPmCartesian(&(tp->goalPos), &start_xyz, &abc, &uvw);
    tpConvertEmcPosetoPmCartesian(&end, &end_xyz, NULL, NULL);

    pmCartLineInit(&line_xyz, &start_xyz, &end_xyz);

    tpInitializeNewSegment(tp,&tc,vel,ini_maxvel,acc,enables);

    tc.coords.rigidtap.reversal_target = line_xyz.tmag;

    // allow 10 turns of the spindle to stop - we don't want to just go on forever
    tc.target = line_xyz.tmag + 10. * tp->uu_per_rev;

    tc.atspeed = 1;

    tc.coords.rigidtap.xyz = line_xyz;
    tc.coords.rigidtap.abc = abc;
    tc.coords.rigidtap.uvw = uvw;
    tc.coords.rigidtap.state = TAPPING;
    tc.motion_type = TC_RIGIDTAP;
    tc.canon_motion_type = 0;
    tc.term_cond = 0;
    tc.tolerance = tp->tolerance;

    if(!tp->synchronized) {
        rtapi_print_msg(RTAPI_MSG_ERR, "Cannot add unsynchronized rigid tap move.\n");
        return -1;
    }
    tc.synchronized = tp->synchronized;

    tc.uu_per_rev = tp->uu_per_rev;
    tc.indexrotary = -1;

    if (syncdio.anychanged != 0) {
        tc.syncdio = syncdio; //enqueue the list of DIOs that need toggling
        tpClearDIOs(); // clear out the list, in order to prepare for the next time we need to use it
    } else {
        tc.syncdio.anychanged = 0;
    }

    //Assume non-zero error code is failure
    return tpAddSegmentToQueue(tp, &tc, &end);
}

STATIC int tpCheckNeedBlendArc(TP_STRUCT const * const tp, TC_STRUCT const * const prev_tc, 
        TC_STRUCT const * const tc, double period) {
    double omega=0.0;

    if (prev_tc == NULL || tc == NULL) {
        tp_debug_print("prev_tc or tc doesn't exist\n");
        return -1;
    }

    //Abort blend arc if the intersection angle calculation fails (not the same as tangent case)
    if (tpCalculateUnitCartAngle(&(prev_tc->coords.line.xyz.uVec), &(tc->coords.line.xyz.uVec), &omega)) {
        return -1;
    }

    //TODO check if this would benefit from being clipped by max delta V
    double v_req=fmax(prev_tc->reqvel, tc->reqvel);
    //TODO can we make this max? does it even matter?
    double a_max=fmin(tpGetScaledAccel(tp, prev_tc),
            tpGetScaledAccel(tp, tc));

    // Calculate the maximum angle between unit vectors that can still be
    // considered "tangent" (i.e. small enough that the
    // acceleration/deceleration spike is within limits).
    double crit_angle = TP_ANGLE_EPSILON;

    tp_debug_print("max tan angle is %f\n",crit_angle);
    tp_debug_print("angle between segs = %f\n",omega);

    //If the segments are nearly tangent, just treat it as tangent since the
    //acceleration is within bounds.
    if (omega < crit_angle) {
        return 1;
    }

    //If the corner is too tight, a circular arc would have zero radius. Fall
    //back to default blend.
    if ((PM_PI - omega) < crit_angle ) {
        tp_debug_print("Corner too tight, omega = %f\n",omega);
        return -1;
    }

    //If not linear blends, we can't easily compute an arc
    if (!(prev_tc->motion_type == TC_LINEAR) || !(tc->motion_type == TC_LINEAR)) {
        tp_debug_print("Wrong motion type tc =%u, tc2=%u\n",
                prev_tc->motion_type,tc->motion_type);
        return -1;
    }

    //If exact stop, we don't compute the arc
    if (prev_tc->term_cond != TC_TERM_COND_PARABOLIC) {
        tp_debug_print("Wrong term cond =%u\n", prev_tc->term_cond);
        return -1;
    }

    //If we have any rotary axis motion, then don't create a blend arc
    if (prev_tc->coords.line.abc.tmag > TP_MAG_EPSILON ||
            tc->coords.line.abc.tmag > TP_MAG_EPSILON) {
        tp_debug_print("ABC motion, can't do 3D arc blend\n");
        return -1;
    }

    if (prev_tc->coords.line.uvw.tmag > TP_MAG_EPSILON ||
            tc->coords.line.uvw.tmag > TP_MAG_EPSILON) {
        tp_debug_print("UVW motion, can't do 3D arc blend\n");
        return -1;
    }
    return 0;
}


/**
 * Connect a blend arc to the two line segments it blends.
 *
 */
STATIC int tcConnectBlendArc(TC_STRUCT * const prev_tc, TC_STRUCT * const tc,
        TC_STRUCT const * const blend_tc) {

    //Scratch variables for arc end and start points
    PmCartesian start_xyz, end_xyz;

    //Get start and end points of blend arc to update lines
    pmCirclePoint(&blend_tc->coords.circle.xyz, 0.0, &start_xyz);
    pmCirclePoint(&blend_tc->coords.circle.xyz,
            blend_tc->coords.circle.xyz.angle, &end_xyz);

    /* Only shift XYZ for now*/
    pmCartLineInit(&prev_tc->coords.line.xyz,
            &prev_tc->coords.line.xyz.start,&start_xyz);
    pmCartLineInit(&tc->coords.line.xyz, &end_xyz, &tc->coords.line.xyz.end);

    tp_debug_print("Old target = %f\n",prev_tc->target);
    prev_tc->target=prev_tc->coords.line.xyz.tmag;
    tp_debug_print("Target = %f\n",prev_tc->target);

    //FIXME use defined epsilon
    tc->target=tc->coords.line.xyz.tmag;
    prev_tc->term_cond = TC_TERM_COND_TANGENT;

    if (prev_tc->target < 0.000001 ) {
        tp_debug_print("Flagged prev_tc for removal\n");
        return 1;
    }

    return 0;
}


/**
 * Do "rising tide" optimization to find allowable final velocities for each queued segment.
 * Walk along the queue from the back to the front. Based on the "current"
 * segment's final velocity, calculate the previous segment's maximum allowable
 * final velocity. The depth we walk along the queue is controlled by the
 * TP_LOOKAHEAD_DEPTH constant for now. The process safetly aborts early due to
 * a short queue or other conflicts.
 */
STATIC int tpRunOptimization(TP_STRUCT * const tp) {
    //Just loop over everything 
    int ind, x;
    //Assume that length won't change during a run
    TC_STRUCT *tc=NULL;
    TC_STRUCT *prev_tc=NULL;
    double vs=0.0;

    int len = tcqLen(&tp->queue);
    int walk = TP_LOOKAHEAD_DEPTH;

    if (len < 2) {
        return 0;
    }

    tp_debug_print("  queue _len = %d\n", len);
    for (x = 1; x < walk; ++x) {
        //Start at most recently added

        ind=len-x;
        tp_debug_print(" x=%u, ind = %u\n", x,ind);

        tc=tcqItem(&tp->queue, ind);
        prev_tc=tcqItem(&tp->queue, ind-1);

        if ( !prev_tc || !tc) {
            break;
        }

        tp_debug_print("  prev term = %u, tc term = %u\n",
                prev_tc->term_cond,tc->term_cond);

        // stop optimizing if we hit a non-tangent segment (final velocity
        // stays zero)
        if (prev_tc->term_cond != TC_TERM_COND_TANGENT) {
            break;
        }

        //Abort if a segment is already in progress
        //TODO: do we need to do this? The calculation should compensate
        if (prev_tc->progress>0) {
            tp_debug_print("segment %d already started, progress is %f!\n",
                    ind-1,prev_tc->progress);
            break;
        }

        //Calculate the maximum starting velocity vs of segment tc, given the
        //trajectory parameters
        double acc = tpGetScaledAccel(tp,tc);
        vs = pmSqrt(pmSq(tc->finalvel) + 2 * acc * tc->target);

        tp_debug_print(" vs = %f, reqvel = %f\n",vs,tc->reqvel);
        if (vs > tc->maxvel) {
            //Found a peak
            vs = tc->maxvel;
            prev_tc->finalvel = vs;
            prev_tc->atpeak=1;
            tp_debug_print("found peak\n");
        } else {
            prev_tc->finalvel = vs;
            prev_tc->atpeak=0;
        }
        if (tc->atpeak) {
            break;
        }

        tp_debug_print(" prev_tc-> fv = %f, tc->fv = %f\n",
                prev_tc->finalvel, tc->finalvel);

    }

    return 0;
}


/**
 * Handle creating a blend arc when a new line segment is about to enter the queue.
 * This function handles the checks, setup, and calculations for creating a new
 * blend arc. Essentially all of the blend arc functions are called through
 * here to isolate this process from tpAddLine.
 * TODO: remove "end" as a parameter since it gets thrown out by the next line
 * added after this.
 */
static int tpHandleBlendArc(TP_STRUCT * const tp, TC_STRUCT * const tc, EmcPose const * const end) {

    tp_debug_print("----------------------\nStarting blend stuff\n");

    TC_STRUCT *prev_tc = tcqLast(&tp->queue);

    //If the previous segment has already started, then don't create a blend
    //arc for the next pair.
    // TODO May be able to lift this restriction if we can ensure that we leave
    // 1 timestep's worth of distance in prev_tc
    if ( !prev_tc || prev_tc->progress > 0.0) {
        return -1;
    }

    int need_arc = tpCheckNeedBlendArc(tp,prev_tc, tc, tp->cycleTime);

    TC_STRUCT blend_tc;

    switch (need_arc) {
        case 0:
            tp_debug_print("Need a blend arc\n");
            //make blend arc
            int arc_fail = tpCreateBlendArc(tp, prev_tc, tc, &blend_tc);
            if (arc_fail) {
                tp_debug_print("error creating arc\n");
                return -1;
            }

            int arc_connect_stat = tcConnectBlendArc(prev_tc, tc, &blend_tc);
            
            if ( 1 == arc_connect_stat) {
                //Remove previous segment that is now zero length
                int trim_fail = tcqPopBack(&tp->queue);
                if (trim_fail) {
                    //Really should not happen...
                    tp_debug_print("Failed to pop last segment!\n");
                    return -1;
                }
                //TODO check for failure, bail if we can't blend
            }

            tpClipVelocityLimit(tp, prev_tc);
            tpClipVelocityLimit(tp, &blend_tc);
            //TC is clipped later

            tpAddSegmentToQueue(tp, &blend_tc, end);

            tpRunOptimization(tp);
            break;
        case 1: //Intentional waterfall
            //Skip, already tangent
            tp_debug_print("Marking segment as tangent (exact stop)\n");
            //
            prev_tc->term_cond = TC_TERM_COND_TANGENT;
            break;
        default:
            tp_debug_print("Failed? need_arc = %d\n", need_arc);
            //Numerical issue? any error means we can't blend, so leave final velocity zero
            return -1;
    }
    return 0;
}

/**
 * Add a straight line to the tc queue.
 * This is a coordinated move in any or all of the six axes. It goes from the
 * end of the previous move to the new end specified here at the
 * currently-active accel and vel settings from the tp struct.
 */
int tpAddLine(TP_STRUCT * const tp, EmcPose end, int type, double vel, double ini_maxvel, double acc, unsigned char enables, char atspeed, int indexrotary)
{
    TC_STRUCT tc;
    PmCartLine line_xyz, line_uvw, line_abc;
    PmCartesian start_xyz, end_xyz;
    PmCartesian start_uvw, end_uvw;
    PmCartesian start_abc, end_abc;

    if (tpErrorCheck(tp)<0) {
        return -1;
    }

    tpConvertEmcPosetoPmCartesian(&(tp->goalPos), &start_xyz, &start_abc, &start_uvw);
    tpConvertEmcPosetoPmCartesian(&end, &end_xyz, &end_abc, &end_uvw);

    pmCartLineInit(&line_xyz, &start_xyz, &end_xyz);
    pmCartLineInit(&line_uvw, &start_uvw, &end_uvw);
    pmCartLineInit(&line_abc, &start_abc, &end_abc);

    tpInitializeNewSegment(tp, &tc, vel, ini_maxvel, acc, enables);

    if (!line_xyz.tmag_zero) {
        tc.target = line_xyz.tmag;
    } else if (!line_uvw.tmag_zero) {
        tc.target = line_uvw.tmag;
    } else {
        tc.target = line_abc.tmag;
    }

    tc.atspeed = atspeed;

    tc.coords.line.xyz = line_xyz;
    tc.coords.line.uvw = line_uvw;
    tc.coords.line.abc = line_abc;
    tc.motion_type = TC_LINEAR;
    tc.canon_motion_type = type;

    tc.term_cond = tp->termCond;
    tc.tolerance = tp->tolerance;

    tc.synchronized = tp->synchronized;
    tc.uu_per_rev = tp->uu_per_rev;
    tc.indexrotary = indexrotary;

    if (syncdio.anychanged != 0) {
        tc.syncdio = syncdio; //enqueue the list of DIOs that need toggling
        tpClearDIOs(); // clear out the list, in order to prepare for the next time we need to use it
    } else {
        tc.syncdio.anychanged = 0;
    }

    tpHandleBlendArc(tp, &tc, &end);

    tpClipVelocityLimit(tp, &tc);

    int retval =  tpAddSegmentToQueue(tp, &tc, &end);
    return retval;
}


/**
 * Adds a circular (circle, arc, helix) move from the end of the
 * last move to this new position.
 *
 * @param end is the xyz/abc point of the destination.
 *
 * see pmCircleInit for further details on how arcs are specified. Note that
 * degenerate arcs/circles are not allowed. We are guaranteed to have a move in
 * xyz so the target is always the circle/arc/helical length.
 */
int tpAddCircle(TP_STRUCT * const tp, EmcPose end,
        PmCartesian center, PmCartesian normal, int turn, int type,
        double vel, double ini_maxvel, double acc, unsigned char enables, char atspeed)
{
    TC_STRUCT tc;
    PmCircle circle;
    PmCartLine line_uvw, line_abc;
    PmCartesian start_xyz, end_xyz;
    PmCartesian start_uvw, end_uvw;
    PmCartesian start_abc, end_abc;
    double helix_z_component;   // z of the helix's cylindrical coord system
    double helix_length;

    if (tpErrorCheck(tp)<0) {
        return -1;
    }

    tpConvertEmcPosetoPmCartesian(&(tp->goalPos), &start_xyz, &start_abc, &start_uvw);
    tpConvertEmcPosetoPmCartesian(&end, &end_xyz, &end_abc, &end_uvw);

    pmCircleInit(&circle, &start_xyz, &end_xyz, &center, &normal, turn);
    pmCartLineInit(&line_uvw, &start_uvw, &end_uvw);
    pmCartLineInit(&line_abc, &start_abc, &end_abc);

    // find helix length
    pmCartMag(&circle.rHelix, &helix_z_component);
    helix_length = pmSqrt(pmSq(circle.angle * circle.radius) +
            pmSq(helix_z_component));
    tpInitializeNewSegment(tp, &tc, vel, ini_maxvel, acc, enables);

    tc.target = helix_length;
    //Assume acceleration ratio of 1
    tc.atspeed = atspeed;
    //TODO acceleration bounded by optimizer

    tc.coords.circle.xyz = circle;
    tc.coords.circle.uvw = line_uvw;
    tc.coords.circle.abc = line_abc;
    tc.motion_type = TC_CIRCULAR;
    tc.canon_motion_type = type;
    tc.term_cond = tp->termCond;
    tc.tolerance = tp->tolerance;

    tc.synchronized = tp->synchronized;
    tc.uu_per_rev = tp->uu_per_rev;
    tc.indexrotary = -1;

    // Nyquist-like velocity limits
    // TODO: deal with shortening of segment in blend arc
    double sample_maxvel = 0.5* tc.target / tp->cycleTime;
    tc.maxvel = fmin(sample_maxvel,tc.maxvel);

    if (syncdio.anychanged != 0) {
        tc.syncdio = syncdio; //enqueue the list of DIOs that need toggling
        tpClearDIOs(); // clear out the list, in order to prepare for the next time we need to use it
    } else {
        tc.syncdio.anychanged = 0;
    }

    tpClipVelocityLimit(tp, &tc);
    //Assume non-zero error code is failure
    return tpAddSegmentToQueue(tp, &tc, &end);
}

/**
 * Handle potential overshoot for tangent segments.
 * Ending a TC with final velocity often leads to small "overshoots". Since we
 * are parameterized by arc length, and the next move is tangent, just apply
 * overshoot to the next TC.
 */
STATIC void tpCheckOvershoot(TC_STRUCT * const tc, TC_STRUCT * const nexttc, EmcPose * const secondary_before) {
    double overshoot=0.0;

    tcGetPos(nexttc, secondary_before);
    //Apply any overshoot to the next TC, and make sure its initial
    //velocity is the same. This gives C1 continuity at tangent blends.
    if (tc->progress > tc->target) {
        //Store previous position
        overshoot = tc->progress - tc->target;
        tp_debug_print("Overshot by %f at end of move %d\n", overshoot, tc->id);
        if (nexttc) {
            nexttc->progress = overshoot;
            if (overshoot > nexttc->target) {
                //trouble...
                tp_debug_print("Overshot beyond nexttc, OS = %f, targ = %f\n",overshoot,nexttc->target);
            }
            nexttc->currentvel = tc->currentvel;
            tc->progress=tc->target;
            tp_debug_print("setting init vel to %f\n", nexttc->currentvel);
        }
        else {
            
            tp_debug_print("No more moves! This should not happen\n");
            tc->progress=tc->target;
        }
    }
    //NOTE: we're assuming that tangent blends mean there's enough length in
    //the next segment to deal with this. TODO: either add a check here or
    //make damn sure that the optimizer does its job.
}

/**
 * Adjusts blend velocity and acceleration to safe limits.
 * If we are blending between tc and nexttc, then we need to figure out what a
 * safe blend velocity is based on the known trajectory parameters. This
 * function updates the TC_STRUCT data with a safe blend velocity.
 */
STATIC double tpComputeBlendVelocity(TP_STRUCT const * const tp, TC_STRUCT const * const tc, TC_STRUCT const * const nexttc) {

    double blend_vel=0.0;
    double v_peak_this;
    double v_peak_next;

    const double acc_this = tpGetScaledAccel(tp,tc);
    const double acc_next = tpGetScaledAccel(tp,nexttc);

    if(nexttc && acc_next) {
        v_peak_this = pmSqrt(tc->target * acc_this);
        v_peak_next = pmSqrt(nexttc->target * acc_next);
        blend_vel=fmin(v_peak_this,v_peak_next);

        if(blend_vel > tpGetReqVel(tp,nexttc)) {
            // segment has a cruise phase so let's blend over the
            // whole accel period if possible
            blend_vel = tpGetReqVel(tp,nexttc);
        }
        if(acc_this < acc_next)
            blend_vel *= acc_this / acc_next;

        if(tc->tolerance) {
            /* see diagram blend.fig.  T (blend tolerance) is given, theta
             * is calculated from dot(s1, s2)
             *
             * blend criteria: we are decelerating at the end of segment s1
             * and we pass distance d from the end.
             * find the corresponding velocity v when passing d.
             *
             * in the drawing note d = 2T/cos(theta)
             *
             * when v1 is decelerating at a to stop, v = at, t = v/a
             * so required d = .5 a (v/a)^2
             *
             * equate the two expressions for d and solve for v
             */
            double tblend_vel;
            double dot;
            double theta;
            PmCartesian v1, v2;

            tcGetEndingUnitVector(tc, &v1);
            tcGetStartingUnitVector(nexttc, &v2);
            pmCartCartDot(&v1, &v2, &dot);

            theta = acos(-dot)/2.0;
            if(cos(theta) > 0.001) {
                tblend_vel = 2.0 * pmSqrt(acc_this * tc->tolerance / cos(theta));
                if(tblend_vel < blend_vel)
                    blend_vel = tblend_vel;
            }
        }
    }
    return blend_vel;
}

/**
 * Clip (saturate) a value x to be within +/- max.
 */
STATIC double saturate(double x, double max) {
    if ( x > max ) return max;
    else if ( x < (-max) ) return -max;
    else return x;
}

/**
 * Compute the updated position and velocity over one timestep.
 * @param tc trajectory component being processed
 * @param v optional velocity output as reference
 * @param on_final_decel optional output flag indicating we are slowing towards the goal speed.
 *
 * This function creates the trapezoidal velocity profile based on tc's
 * velocity and acceleration limits. The formula has been tweaked slightly to
 * allow a non-zero velocity at the instant the target is reached.
 */
void tcRunCycle(TP_STRUCT const * const tp, TC_STRUCT * const tc, double * v, int * on_final_decel) {
    double discr, maxnewvel, newvel, newaccel=0, delta_pos;
    double discr_term1, discr_term2, discr_term3;
   
    // Find maximum allowed velocity from feed and machine limits
    double req_vel = tpGetReqVel(tp,tc);
    // Store a copy of final velocity
    double final_vel = tpGetFinalVel(tp,tc);

    //Clamp the requested velocity by the maximum velocity allowed.
    //TODO remove this since we check limits during initial setup
    if (req_vel > tc->maxvel) {
        req_vel = tc->maxvel;
    }

    //Clamp final velocity to the max velocity we can achieve
    if (final_vel > req_vel) {
        final_vel = req_vel;
    }

    // Need this to plan down to zero V
    if (tp->pausing) {
        final_vel = 0.0;
    }

    if (!tc->blending) {
        tc->vel_at_blend_start = tc->currentvel;
    }

    delta_pos = tc->target - tc->progress;
    const double maxaccel = tpGetScaledAccel(tp, tc);

    discr_term1 = pmSq(final_vel);
    discr_term2 = maxaccel * (2.0 * delta_pos - tc->currentvel * tc->cycle_time);
    discr_term3 = pmSq(maxaccel * tc->cycle_time / 2.0);

    discr = discr_term1 + discr_term2 + discr_term3;

    // Descriminant is a little more complicated with final velocity term If
    // descriminant < 0, we've overshot (or are about to). Do the best we can
    // in this situation
    if (discr < 0.0) {
        newvel = maxnewvel = 0.0;
    }
    else {
        newvel = maxnewvel = -0.5 * maxaccel * tc->cycle_time + pmSqrt(discr);
    }

    if (newvel > req_vel) newvel = req_vel;

    if (newvel < 0.0 ) {
        //If we're not hitting a tangent move, then we need to throw out any overshoot to force an exact stop.
        //FIXME this means a momentary spike in acceleration, test to see if it's a problem
        newvel = newaccel = 0.0;

        if ( !(tc->term_cond == TC_TERM_COND_TANGENT) || (tc->progress < tc->target ) ) {
            rtapi_print_msg(RTAPI_MSG_DBG, "calculated newvel = %f, with T = %f, P = %f", newvel, tc->target, tc->progress);
            tc->progress = tc->target;
        }
    } else {

        bool is_pure_rotary = (tc->motion_type == TC_LINEAR) &&
            (tc->coords.line.xyz.tmag_zero) && (tc->coords.line.uvw.tmag_zero);

        // if the motion is not purely rotary axes (and therefore in angular
        // units), clamp motion's velocity at TRAJ MAX_VELOCITY (tooltip
        // maxvel) except when it's synced to spindle position.
        if (!is_pure_rotary && tc->synchronized != TC_SYNC_POSITION && newvel > tp->vLimit) {
            newvel = tp->vLimit;
        }

        // get acceleration to reach newvel, bounded by machine maximum
        newaccel = (newvel - tc->currentvel) / tc->cycle_time;
        newaccel = saturate(newaccel, maxaccel);

        newvel = tc->currentvel + newaccel * tc->cycle_time;
        // update position in this tc using trapezoidal integration
        // Note that progress can be greater than the target after this step.
        // TODO: handle this in tp
        tc->progress += (newvel + tc->currentvel) * 0.5 * tc->cycle_time;
        tc->currentvel = newvel;
    }
    rtapi_print_msg(RTAPI_MSG_DBG, "TC result: v = %f, dtg = %f, vf = %f, T = %f, P = %f",
            newvel, tc->target-tc->progress,
            final_vel, tc->target, tc->progress);

    if (v) *v = newvel;
    if (on_final_decel) *on_final_decel = fabs(maxnewvel - newvel) < 0.001;
}

void tpToggleDIOs(TC_STRUCT * const tc) {

    int i=0;
    if (tc->syncdio.anychanged != 0) { // we have DIO's to turn on or off
        for (i=0; i < num_dio; i++) {
            if (!(tc->syncdio.dio_mask & (1 << i))) continue;
            if (tc->syncdio.dios[i] > 0) emcmotDioWrite(i, 1); // turn DIO[i] on
            if (tc->syncdio.dios[i] < 0) emcmotDioWrite(i, 0); // turn DIO[i] off
        }
        for (i=0; i < num_aio; i++) {
            if (!(tc->syncdio.aio_mask & (1 << i))) continue;
            emcmotAioWrite(i, tc->syncdio.aios[i]); // set AIO[i]
        }
        tc->syncdio.anychanged = 0; //we have turned them all on/off, nothing else to do for this TC the next time
    }
}


/**
 * Handle special cases for rigid tapping.
 * This function deals with updating the goal position and spindle position
 * during a rigid tap cycle. In particular, the target and spindle goal need to
 * be carefully handled since we're reversing direction.
 */
STATIC void tpHandleRigidTap(TP_STRUCT const * const tp,
        TC_STRUCT * const tc) {

    static double old_spindlepos;
    double new_spindlepos = emcmotStatus->spindleRevs;
    if (emcmotStatus->spindle.direction < 0) new_spindlepos = -new_spindlepos;

    switch (tc->coords.rigidtap.state) {
        case TAPPING:
            rtapi_print_msg(RTAPI_MSG_DBG, "TAPPING");
            if (tc->progress >= tc->coords.rigidtap.reversal_target) {
                // command reversal
                emcmotStatus->spindle.speed *= -1;
                tc->coords.rigidtap.state = REVERSING;
            }
            break;
        case REVERSING:
            rtapi_print_msg(RTAPI_MSG_DBG, "REVERSING");
            if (new_spindlepos < old_spindlepos) {
                PmCartesian start, end;
                PmCartLine *aux = &tc->coords.rigidtap.aux_xyz;
                // we've stopped, so set a new target at the original position
                tc->coords.rigidtap.spindlerevs_at_reversal = new_spindlepos + tp->spindle.offset;

                pmCartLinePoint(&tc->coords.rigidtap.xyz, tc->progress, &start);
                end = tc->coords.rigidtap.xyz.start;
                pmCartLineInit(aux, &start, &end);
                rtapi_print_msg(RTAPI_MSG_DBG, "old target = %f", tc->target);
                tc->coords.rigidtap.reversal_target = aux->tmag;
                tc->target = aux->tmag + 10. * tc->uu_per_rev;
                tc->progress = 0.0;
                rtapi_print_msg(RTAPI_MSG_DBG, "new target = %f", tc->target);

                tc->coords.rigidtap.state = RETRACTION;
            }
            old_spindlepos = new_spindlepos;
            rtapi_print_msg(RTAPI_MSG_DBG, "Spindlepos = %f", new_spindlepos);
            break;
        case RETRACTION:
            rtapi_print_msg(RTAPI_MSG_DBG, "RETRACTION");
            if (tc->progress >= tc->coords.rigidtap.reversal_target) {
                emcmotStatus->spindle.speed *= -1;
                tc->coords.rigidtap.state = FINAL_REVERSAL;
            }
            break;
        case FINAL_REVERSAL:
            rtapi_print_msg(RTAPI_MSG_DBG, "FINAL_REVERSAL");
            if (new_spindlepos > old_spindlepos) {
                PmCartesian start, end;
                PmCartLine *aux = &tc->coords.rigidtap.aux_xyz;
                pmCartLinePoint(aux, tc->progress, &start);
                end = tc->coords.rigidtap.xyz.start;
                pmCartLineInit(aux, &start, &end);
                tc->target = aux->tmag;
                tc->progress = 0.0;
                //No longer need spindle sync at this point
                tc->synchronized = 0;
                tc->reqvel = tc->maxvel;

                tc->coords.rigidtap.state = FINAL_PLACEMENT;
            }
            old_spindlepos = new_spindlepos;
            break;
        case FINAL_PLACEMENT:
            rtapi_print_msg(RTAPI_MSG_DBG, "FINAL_PLACEMENT\n");
            // this is a regular move now, it'll stop at target above.
            break;
    }
}


/**
 * Update emcMotStatus with information about trajectory motion.
 * Based on the specified trajectory segment tc, read its progress and status
 * flags. Then, update the emcmotStatus structure with this information.
 */
STATIC void tpUpdateMovementStatus(TP_STRUCT * const tp,
        emcmot_status_t * const emcmotStatus, TC_STRUCT const * const tc ) {
    EmcPose target;
    tcGetEndpoint(tc, &target);

    tp->motionType = tc->canon_motion_type;
    emcmotStatus->distance_to_go = tc->target - tc->progress;
    emcmotStatus->enables_queued = tc->enables;
    // report our line number to the guis
    tp->execId = tc->id;
    emcmotStatus->requested_vel = tc->reqvel;
    emcmotStatus->current_vel = tc->currentvel;

    emcmotStatus->dtg.tran.x = target.tran.x - tp->currentPos.tran.x;
    emcmotStatus->dtg.tran.y = target.tran.y - tp->currentPos.tran.y;
    emcmotStatus->dtg.tran.z = target.tran.z - tp->currentPos.tran.z;
    emcmotStatus->dtg.a = target.a - tp->currentPos.a;
    emcmotStatus->dtg.b = target.b - tp->currentPos.b;
    emcmotStatus->dtg.c = target.c - tp->currentPos.c;
    emcmotStatus->dtg.u = target.u - tp->currentPos.u;
    emcmotStatus->dtg.v = target.v - tp->currentPos.v;
    emcmotStatus->dtg.w = target.w - tp->currentPos.w;

}


/**
 * Do a parabolic blend by updating the nexttc.
 * Perform the actual blending process by updating the nexttc.
 */
STATIC void tpDoParabolicBlend(TP_STRUCT * const tp, TC_STRUCT * const tc,
        TC_STRUCT * nexttc, double primary_vel) {

    /*EmcPose secondary_before = tcGetPos(nexttc);*/
    //Store the actual requested velocity
    double save_vel = nexttc->reqvel;

    if (tpGetFeedOverride(tp,nexttc) > 0.0) {
        nexttc->reqvel =  ((tc->vel_at_blend_start - primary_vel) / tpGetFeedOverride(tp, nexttc));
    } else {
        nexttc->reqvel = 0.0;
    }

    tcRunCycle(tp, nexttc, NULL, NULL);
    //Restore the blend velocity
    nexttc->reqvel = save_vel;

}


/**
 * Calculate the displacement between a previous pose and the current tc position.
 * This function encapsulates the simple but verbose displacement calculation
 * based on an initial position. Because of the blending method, we need to use
 * displacement instead of absolute position when blending between moves.
 */
STATIC void tpFindDisplacement(TC_STRUCT const * const tc, EmcPose const * const before,
        EmcPose * const displacement) {

    EmcPose after;
    tcGetPos(tc, &after);

    pmCartCartSub(&after.tran, &before->tran,
            &(displacement->tran));
    displacement->a = after.a - before->a;
    displacement->b = after.b - before->b;
    displacement->c = after.c - before->c;

    displacement->u = after.u - before->u;
    displacement->v = after.v - before->v;
    displacement->w = after.w - before->w;

}


/**
 * Update the planner's position, given a displacement.
 * This function stores the result of the internal calculations in tpRunCycle,
 * updating the global position of tp.
 */
STATIC void tpUpdatePosition(TP_STRUCT * const tp, EmcPose const * const displacement) {

    pmCartCartAdd(&tp->currentPos.tran, &displacement->tran,
            &(tp->currentPos.tran));
    tp->currentPos.a += displacement->a;
    tp->currentPos.b += displacement->b;
    tp->currentPos.c += displacement->c;

    tp->currentPos.u += displacement->u;
    tp->currentPos.v += displacement->v;
    tp->currentPos.w += displacement->w;
}


/**
 * Cleanup if tc is not valid (empty queue).
 * If the program ends, or we hit QUEUE STARVATION, do a soft reset on the trajectory planner.
 * TODO: Is this necessary?
 * TODO: Can this be handled by tpClear?
 */
STATIC void tpHandleEmptyQueue(TP_STRUCT * const tp,
        emcmot_status_t * const emcmotStatus) {

    tcqInit(&tp->queue);
    tp->goalPos = tp->currentPos;
    tp->done = 1;
    tp->depth = tp->activeDepth = 0;
    tp->aborting = 0;
    tp->execId = 0;
    tp->motionType = 0;
    tpResume(tp);
    // when not executing a move, use the current enable flags
    emcmotStatus->enables_queued = emcmotStatus->enables_new;
}

/** Wrapper function to unlock rotary axes */
STATIC void tpSetRotaryUnlock(int axis, int unlock) {
    emcmotSetRotaryUnlock(axis, unlock);
}

/** Wrapper function to check rotary axis lock */
STATIC int tpGetRotaryIsUnlocked(int axis) {
    return emcmotGetRotaryIsUnlocked(axis);
}


/**
 * Cleanup after a trajectory segment is complete.
 * If the current move is complete and we're not waiting on the spindle for
 * const this move, then pop if off the queue and perform cleanup operations.
 * Finally, get the next move in the queue.
 */
STATIC TC_STRUCT * tpCompleteSegment(TP_STRUCT * const tp, TC_STRUCT *
        const tc) {
    // if we're synced, and this move is ending, save the
    // spindle position so the next synced move can be in
    // the right place.

    rtapi_print_msg(RTAPI_MSG_DBG, "Finished tc id %d", tc->id);
    if(tc->synchronized != TC_SYNC_NONE)
        tp->spindle.offset += tc->target/tc->uu_per_rev;
    else
        tp->spindle.offset = 0.0;

    if(tc->indexrotary != -1) {
        // this was an indexing move, so before we remove it we must
        // relock the axis
        tpSetRotaryUnlock(tc->indexrotary, 0);
        // if it is now locked, fall through and remove the finished move.
        // otherwise, just come back later and check again
        if(tpGetRotaryIsUnlocked(tc->indexrotary))
            return NULL;
    }

    // done with this move
    tcqRemove(&tp->queue, 1);

    // so get next move
    TC_STRUCT * tc_next = tcqItem(&tp->queue, 0);

    if(!tc_next) return NULL;

    rtapi_print_msg(RTAPI_MSG_DBG, "Found next tc id %d\n", tc_next->id);
    return tc_next;
}


/**
 * Handle an abort command.
 * Based on the current motion state, handle the consequences of an abort command.
 */
STATIC int tpHandleAbort(TP_STRUCT * const tp, TC_STRUCT * const tc,
        TC_STRUCT * const nexttc) {

    //If the motion has stopped, then it's safe to reset the TP struct.
    if( MOTION_ID_VALID(tp->spindle.waiting_for_index) ||
            MOTION_ID_VALID(tp->spindle.waiting_for_atspeed) ||
            (tc->currentvel == 0.0 && !nexttc) ||
            (tc->currentvel == 0.0 && nexttc && nexttc->currentvel == 0.0) ) {
        tcqInit(&tp->queue);
        tp->goalPos = tp->currentPos;
        tp->done = 1;
        tp->depth = tp->activeDepth = 0;
        tp->aborting = 0;
        tp->execId = 0;
        tp->motionType = 0;
        tp->synchronized = 0;
        tp->spindle.waiting_for_index = MOTION_INVALID_ID;
        tp->spindle.waiting_for_atspeed = MOTION_INVALID_ID;
        emcmotStatus->spindleSync = 0;
        tpResume(tp);
        return 0;
    }  //FIXME consistent error codes
    return 1;

}


/**
 * Check if the segment waiting for an index has changed.
 * If the current segment waiting for an index is not the current segment, then
 * something has gone wrong. The fix for now is to just update status so we're
 * waiting in the current segment instead. (Rob's understanding)
 */
STATIC int tpCheckWaiting(TP_STRUCT * const tp, TC_STRUCT const * const tc) {

    // this is no longer the segment we were waiting_for_index for
    if (MOTION_ID_VALID(tp->spindle.waiting_for_index) && tp->spindle.waiting_for_index != tc->id)
    {
        rtapi_print_msg(RTAPI_MSG_ERR,
                "Was waiting for index on motion id %d, but reached id %d\n",
                tp->spindle.waiting_for_index, tc->id);
        tp->spindle.waiting_for_index = MOTION_INVALID_ID;
    }

    if (MOTION_ID_VALID(tp->spindle.waiting_for_atspeed) && tp->spindle.waiting_for_atspeed != tc->id)
    {

        rtapi_print_msg(RTAPI_MSG_ERR,
                "Was waiting for atspeed on motion id %d, but reached id %d\n",
                tp->spindle.waiting_for_atspeed, tc->id);
        tp->spindle.waiting_for_atspeed = MOTION_INVALID_ID;
    }

    if (MOTION_ID_VALID(tp->spindle.waiting_for_atspeed)) {
        if(!emcmotStatus->spindle_is_atspeed) {
            // spindle is still not at the right speed, so wait another cycle
            return 1;
        } else {
            tp->spindle.waiting_for_atspeed = MOTION_INVALID_ID;
        }
    }

    return 0;
}


/**
 * Get a pointer to nexttc if we can, based on conditions.
 * Once an active TC is created in the planner, we want to know the nexttc if
 * we can get it. it's not an error if nexttc is missing (like in the MDI, or
 * at the end of a path).
 */
STATIC TC_STRUCT * const tpGetNextTC(TP_STRUCT * const tp,
        TC_STRUCT * const tc, int stepping) {

    TC_STRUCT * nexttc = NULL;

    if(!stepping && tc->term_cond)
        nexttc = tcqItem(&tp->queue, 1);
    else
        nexttc = NULL;

    if( tc->synchronized != TC_SYNC_POSITION && nexttc && nexttc->synchronized == TC_SYNC_POSITION) {
        // we'll have to wait for spindle sync; might as well
        // stop at the right place (don't blend)
        tc->term_cond = TC_TERM_COND_STOP;
        nexttc = NULL;
    }

    if(nexttc && nexttc->atspeed) {
        // we'll have to wait for the spindle to be at-speed; might as well
        // stop at the right place (don't blend), like above
        // FIXME change the values so that 0 is exact stop mode
        tc->term_cond = TC_TERM_COND_STOP;
        nexttc = NULL;
    }

    return nexttc;
}


/**
 * "Activate" a segment being read for the first time.
 * This function handles initial setup of a new segment read off of the queue
 * for the first time.
 */
STATIC int tpActivateSegment(TP_STRUCT * const tp, TC_STRUCT * const tc,
        emcmot_status_t * const emcmotStatus) {

    // wait for atspeed, if motion requested it.  also, force
    // atspeed check for the start of all spindle synchronized
    // moves.
    bool needs_atspeed = tc->atspeed ||
        (tc->synchronized == TC_SYNC_POSITION && !(emcmotStatus->spindleSync));
    if( needs_atspeed && !(emcmotStatus->spindle_is_atspeed)) {
        tp->spindle.waiting_for_atspeed = tc->id;
        return 0;
    }

    if (tc->indexrotary != -1) {
        // request that the axis unlock
        tpSetRotaryUnlock(tc->indexrotary, 1);
        // if it is unlocked, fall through and start the move.
        // otherwise, just come back later and check again
        if (!tpGetRotaryIsUnlocked(tc->indexrotary))
            return 0;
    }

    // Temporary debug message
    rtapi_print_msg(RTAPI_MSG_DBG, "Activate tc id %d\n", tc->id);

    tc->active = 1;
    tc->currentvel = 0;
    //FIXME activedepth might change meaning with lookahead?
    tp->depth = tp->activeDepth = 1;
    tp->motionType = tc->canon_motion_type;
    tc->blending = 0;

    // honor accel constraint in case we happen to make an acute angle
    // with the next segment.
    // TODO better acceleration constraints?
    if(tc->term_cond == TC_TERM_COND_PARABOLIC) {
        tc->accel_scale = 0.5;
        tp_debug_print("Parabolic blend, reduce maxaccel by %f\n",tc->accel_scale);
    }

    if(TC_SYNC_POSITION == tc->synchronized && !(emcmotStatus->spindleSync)) {
        // if we aren't already synced, wait
        tp->spindle.waiting_for_index = tc->id;
        // ask for an index reset
        emcmotStatus->spindle_index_enable = 1;
        tp->spindle.offset = 0.0;
        rtapi_print_msg(RTAPI_MSG_DBG, "Waiting on sync...\n");
        // don't move: wait
        return 0;
    }

    //Keep going:
    return 1;
}


/**
 * Run velocity mode synchronization.
 * Update requested velocity to follow the spindle's velocity (scaled by feed rate).
 */
STATIC void tpSyncVelocityMode(TP_STRUCT * const tp, TC_STRUCT * const tc, TC_STRUCT const * nexttc) {
    //NOTE: check for aborting outside of here
    double speed = emcmotStatus->spindleSpeedIn;
    double pos_error = fabs(speed) * tc->uu_per_rev;
    //Take into account blending?
    if(nexttc) pos_error -= nexttc->progress; /* ?? */
    tc->reqvel = pos_error;
}


/**
 * Run position mode synchronization.
 * Updates requested velocity for a trajectory segment to track the spindle's position.
 */
STATIC void tpSyncPositionMode(TP_STRUCT * const tp, TC_STRUCT * const tc, TC_STRUCT const * nexttc ) {

    double spindle_pos = tpGetSignedSpindlePosition(emcmotStatus->spindleRevs,emcmotStatus->spindle.direction);
    double spindle_vel, target_vel;
    double oldrevs = tp->spindle.revs;

    /*double new_spindlepos = emcmotStatus->spindleRevs;*/

    if(tc->motion_type == TC_RIGIDTAP &&
            (tc->coords.rigidtap.state == RETRACTION ||
             tc->coords.rigidtap.state == FINAL_REVERSAL))
        tp->spindle.revs = tc->coords.rigidtap.spindlerevs_at_reversal -
            spindle_pos;
    else
        tp->spindle.revs = spindle_pos;

    double pos_error = (tp->spindle.revs - tp->spindle.offset) * tc->uu_per_rev - tc->progress;

    if(nexttc) pos_error -= nexttc->progress;

    if(tc->sync_accel) {
        // detect when velocities match, and move the target accordingly.
        // acceleration will abruptly stop and we will be on our new target.
        spindle_vel = tp->spindle.revs/(tc->cycle_time * tc->sync_accel++);
        target_vel = spindle_vel * tc->uu_per_rev;
        if(tc->currentvel >= target_vel) {
            // move target so as to drive pos_error to 0 next cycle
            tp->spindle.offset = tp->spindle.revs - tc->progress/tc->uu_per_rev;
            tc->sync_accel = 0;
            tc->reqvel = target_vel;
        } else {
            // beginning of move and we are behind: accel as fast as we can
            tc->reqvel = tc->maxvel;
        }
    } else {
        // we have synced the beginning of the move as best we can -
        // track position (minimize pos_error).
        double errorvel;
        spindle_vel = (tp->spindle.revs - oldrevs) / tc->cycle_time;
        target_vel = spindle_vel * tc->uu_per_rev;
        errorvel = pmSqrt(fabs(pos_error) * tpGetScaledAccel(tp,tc));
        if(pos_error<0) errorvel = -errorvel;
        tc->reqvel = target_vel + errorvel;
    }
    //Finally, clip requested velocity at zero
    if (tc->reqvel < 0.0) {
        tc->reqvel = 0.0;
    }
}


/**
 * Calculate an updated goal position for the next timestep.
 * This is the brains of the operation. It's called every TRAJ period and is
 * expected to set tp->currentPos to the new machine position. Lots of other
 * const tp fields (depth, done, etc) have to be twiddled to communicate the status;
 * I think those are spelled out here correctly and I can't clean it up
 * without breaking the API that the TP presents to motion. It's not THAT bad
 Found next the interest of not touching stuff outside this directory, I'm going
 * const to leave it for now.
 */
int tpRunCycle(TP_STRUCT * const tp, long period)
{
    // vel = (new position - old position) / cycle time
    // (two position points required)
    //
    // acc = (new vel - old vel) / cycle time
    // (three position points required)

    //Pointer to next trajectory component
    TC_STRUCT *tc, *nexttc;

    double primary_vel;
    int on_final_decel;

    //Pose data used for intermediate calculations
    EmcPose primary_before, primary_displacement;
    EmcPose secondary_before, secondary_displacement;

    //Update motion status
    emcmotStatus->tcqlen = tcqLen(&tp->queue);
    //FIXME why is this zero?
    emcmotStatus->requested_vel = 0.0;
    //Define TC as the "first" element in the queue
    tc = tcqItem(&tp->queue, 0);

    //If we have a NULL pointer, then the queue must be empty, so we're done.
    if(!tc) {
        tpHandleEmptyQueue(tp, emcmotStatus);
        return 0;
    }

    //If we can't get a valid tc (end of move, waiting on spindle), we're done for now.
    if (tc->target == tc->progress && tp->spindle.waiting_for_atspeed != tc->id) {
        tc = tpCompleteSegment(tp, tc);
        if (!tc)  return 0;
    }

    nexttc = tpGetNextTC(tp, tc, emcmotDebug->stepping);

    if(tp->aborting) {
        int slowing = tpHandleAbort(tp, tc, nexttc);
        if (!slowing) {
            rtapi_print_msg(RTAPI_MSG_DBG, "  Early stop at tpHandleAbort?\n");
            return 0;
        }
    }

    int tc_wait = tpCheckWaiting(tp, tc);
    if (tc_wait) return 0;

    //TODO refactor
    if(tc->active == 0) {
        bool ready = tpActivateSegment(tp, tc, emcmotStatus);
        // Need to wait to continue motion, end planning here
        if (!ready) {
            return 0;
        }
    }

    if (MOTION_ID_VALID(tp->spindle.waiting_for_index)) {
        if(emcmotStatus->spindle_index_enable) {
            /* haven't passed index yet */
            return 0;
        } else {
            /* passed index, start the move */
            emcmotStatus->spindleSync = 1;
            tp->spindle.waiting_for_index = MOTION_INVALID_ID;
            tc->sync_accel=1;
            tp->spindle.revs=0;
        }
    }

    if (tc->motion_type == TC_RIGIDTAP) {
        tpHandleRigidTap(tp, tc);
    }

    if(!tc->synchronized) {
        emcmotStatus->spindleSync = 0;
    }

    if(nexttc && nexttc->active == 0) {
        // this means this tc is being read for the first time.
        rtapi_print_msg(RTAPI_MSG_DBG, "Activate nexttc id %d\n", nexttc->id);
        nexttc->currentvel = 0;
        tp->depth = tp->activeDepth = 1;
        nexttc->active = 1;
        nexttc->blending = 0;

        // honor accel constraint if we happen to make an acute angle with the
        // above segment or the following one
        // TODO: replace this with better acceleration constraint
        if(tc->term_cond == TC_TERM_COND_PARABOLIC || nexttc->term_cond == TC_TERM_COND_PARABOLIC) {
            nexttc->accel_scale = 0.5;
            tp_debug_print("Parabolic blend, nexttc accel scale %f\n",tc->accel_scale);
        }
    }

    /** If synchronized with spindle, calculate requested velocity to track spindle motion.*/
    switch (tc->synchronized) {
        case TC_SYNC_NONE:
            break;
        case TC_SYNC_VELOCITY:
            tpSyncVelocityMode(tp, tc, nexttc);
            break;
        case TC_SYNC_POSITION:
            tpSyncPositionMode(tp, tc, nexttc);
        default:
            tp_debug_print("unrecognized spindle sync state!\n");
            break;
    }

    if(nexttc) {
        if (nexttc->synchronized) {
            //If the next move is synchronized too, then match it's
            //requested velocity to the current move
            nexttc->reqvel = tc->reqvel;
        }   
    }

    if (tc->term_cond == TC_TERM_COND_PARABOLIC) {
        tc->blend_vel = tpComputeBlendVelocity(tp, tc, nexttc);
    }

    tcGetPos(tc, &primary_before);

    // Update the current tc
    tcRunCycle(tp, tc, &primary_vel, &on_final_decel);
    //If we've overshot the target, then fold this into the next move
    if (tc->term_cond == TC_TERM_COND_TANGENT) tpCheckOvershoot(tc, nexttc, &secondary_before);

    //Update
    tpFindDisplacement(tc, &primary_before, &primary_displacement);
    rtapi_print_msg(RTAPI_MSG_DBG, "Primary disp, X = %f, Y=%f, Z=%f", 
            primary_displacement.tran.x, primary_displacement.tran.y, primary_displacement.tran.z);

    // Update the trajectory planner position based on the results

    /* BLENDING STUFF */
    // make sure we continue to blend this segment even when its
    // accel reaches 0 (at the very end)
    bool is_blend_start = (tc->term_cond == TC_TERM_COND_PARABOLIC ) && nexttc &&
        on_final_decel && (primary_vel < tc->blend_vel);

    bool is_tangent_blend_start = (tc->term_cond == TC_TERM_COND_TANGENT ) &&
        nexttc && (tc->target == tc->progress);

    if (is_blend_start) tc->blending = 1;

    tpUpdatePosition(tp, &primary_displacement);
    if(tc->blending && nexttc ) {
        // hack to show blends in axis
        tp->motionType = 0;

        tcGetPos(nexttc, &secondary_before);

        tpDoParabolicBlend(tp, tc, nexttc, primary_vel);
        tpFindDisplacement(nexttc, &secondary_before, &secondary_displacement);

        //Add in contributions from both segments
        tpUpdatePosition(tp, &secondary_displacement);


        if(tc->currentvel > nexttc->currentvel) tpUpdateMovementStatus(tp, emcmotStatus, tc);
        else {
            tpToggleDIOs(nexttc);
            tpUpdateMovementStatus(tp, emcmotStatus, nexttc);
        }

        //Update velocity status based on both tc and nexttc
        emcmotStatus->current_vel = tc->currentvel + nexttc->currentvel;

    }
    else {

        if (is_tangent_blend_start) {
            rtapi_print_msg(RTAPI_MSG_DBG, "Found Tangency at %d, T-P of tc is %f at_endpt = %d\n",
                    tc->id, tc->target - tc->progress, tc->target == tc->progress);

            tpFindDisplacement(nexttc, &secondary_before, &secondary_displacement);
            /*rtapi_print_msg(RTAPI_MSG_DBG, "Secondary disp, X = %f, Y=%f, Z=%f", */
            /*secondary_displacement.tran.x, secondary_displacement.tran.y, secondary_displacement.tran.z);*/
            tpUpdatePosition(tp, &secondary_displacement);

        }
        tpToggleDIOs(tc); //check and do DIO changes
        tpUpdateMovementStatus(tp, emcmotStatus, tc);
    }

    return 0;
}

int tpSetSpindleSync(TP_STRUCT * const tp, double sync, int mode) {
    if(sync) {
        tp->synchronized = 1;
        tp->uu_per_rev = sync;
        tp->velocity_mode = mode;
    } else
        tp->synchronized = 0;

    return 0;
}

int tpPause(TP_STRUCT * const tp)
{
    if (0 == tp) {
        return -1;
    }
    tp->pausing = 1;
    return 0;
}

int tpResume(TP_STRUCT * const tp)
{
    if (0 == tp) {
        return -1;
    }
    tp->pausing = 0;
    return 0;
}

int tpAbort(TP_STRUCT * const tp)
{
    if (0 == tp) {
        return -1;
    }

    if (!tp->aborting) {
        /* const to abort, signal a pause and set our abort flag */
        tpPause(tp);
        tp->aborting = 1;
    }
    return tpClearDIOs(); //clears out any already cached DIOs
}

int tpGetMotionType(TP_STRUCT * const tp)
{
    return tp->motionType;
}

int tpGetPos(TP_STRUCT const * const tp, EmcPose * const pos)
{

    if (0 == tp) {
        ZERO_EMC_POSE((*pos));
        return -1;
    } else {
        *pos = tp->currentPos;
    }

    return 0;
}

int tpIsDone(TP_STRUCT * const tp)
{
    if (0 == tp) {
        return 0;
    }

    return tp->done;
}

int tpQueueDepth(TP_STRUCT * const tp)
{
    if (0 == tp) {
        return 0;
    }

    return tp->depth;
}

int tpActiveDepth(TP_STRUCT * const tp)
{
    if (0 == tp) {
        return 0;
    }

    return tp->activeDepth;
}

int tpSetAout(TP_STRUCT * const tp, unsigned char index, double start, double end) {
    if (0 == tp) {
        return -1;
    }
    syncdio.anychanged = 1; //something has changed
    syncdio.aio_mask |= (1 << index);
    syncdio.aios[index] = start;
    return 0;
}

int tpSetDout(TP_STRUCT * const tp, int index, unsigned char start, unsigned char end) {
    if (0 == tp) {
        return -1;
    }
    syncdio.anychanged = 1; //something has changed
    syncdio.dio_mask |= (1 << index);
    if (start > 0)
        syncdio.dios[index] = 1; // the end value can't be set from canon currently, and has the same value as start
    else
        syncdio.dios[index] = -1;
    return 0;
}

// vim:sw=4:sts=4:et:
