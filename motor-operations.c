/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*-
 * (c) 2013, 2014 Henner Zeller <h.zeller@acm.org>
 *
 * This file is part of BeagleG. http://github.com/hzeller/beagleg
 *
 * BeagleG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BeagleG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with BeagleG.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "motor-operations.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "motor-interface-constants.h"
#include "motion-queue.h"

// We need two loops per motor step (edge up, edge down),
// So we need to multiply step-counts by 2
// This could be more, if we wanted to implement sub-step resolution with
// more than one bit output per step (probably only with hand-built drivers).
#define LOOPS_PER_STEP (1 << 1)

// If we do more than these number of steps, the fixed point fraction
// accumulate too much error.
#define MAX_STEPS_PER_SEGMENT (65535 / LOOPS_PER_STEP)

// TODO: don't store this singleton like, but keep in user_data of the MotorOperations
static float hardware_frequency_limit_;

static inline float sq(float x) { return x * x; }  // square a number
static inline double sqd(double x) { return x * x; }  // square a number
static inline int round2int(float x) { return (int) roundf(x); }

// Clip speed to maximum we can reach with hardware.
static float clip_hardware_frequency_limit(float v) {
  return v < hardware_frequency_limit_ ? v : hardware_frequency_limit_;
}

static float calcAccelerationCurveValueAt(int index, float acceleration) {
  // counter_freq * sqrt(2 / accleration)
  const float accel_factor = TIMER_FREQUENCY
    * (sqrtf(LOOPS_PER_STEP * 2.0f / acceleration)) / LOOPS_PER_STEP;
  // The approximation is pretty far off in the first step; adjust.
  const float c0 = (index == 0) ? accel_factor * 0.67605f : accel_factor;
  return c0 * (sqrtf(index + 1) - sqrtf(index));
}

#if 0
// Is acceleration in acceptable range ?
static char test_acceleration_ok(float acceleration) {
  if (acceleration <= 0)
    return 1;  // <= 0: always full speed.

  // Check that the fixed point acceleration parameter (that we shift
  // DELAY_CYCLE_SHIFT) fits into 32 bit.
  // Also 2 additional bits headroom because we need to shift it by 2 in the
  // division.
  const float start_accel_cycle_value = (1 << (DELAY_CYCLE_SHIFT + 2))
    * calcAccelerationCurveValueAt(0, acceleration);
  if (start_accel_cycle_value > 0xFFFFFFFF) {
    fprintf(stderr, "Too slow acceleration to deal with. If really needed, "
	    "reduce value of #define DELAY_CYCLE_SHIFT\n");
    return 0;
  }
  return 1;
}
#endif

static void beagleg_enqueue_internal(struct MotionQueue *backend,
                                     const struct MotorMovement *param,
				    int defining_axis_steps) {
  struct MotionSegment new_element;
  new_element.direction_bits = 0;

  // The defining_axis_steps is the number of steps of the axis that requires
  // the most number of steps. All the others are a fraction of the steps.
  //
  // The top bits have LOOPS_PER_STEP states (2 is the minium, as we need two
  // cycles for a 0 1 transition. So in that case we have 31 bit fraction
  // and 1 bit that overflows and toggles for the steps we want to generate.
  const uint64_t max_fraction = 0xFFFFFFFF / LOOPS_PER_STEP;
  for (int i = 0; i < MOTION_MOTOR_COUNT; ++i) {
    if (param->steps[i] < 0) {
      new_element.direction_bits |= (1 << i);
    }
    const uint64_t delta = abs(param->steps[i]);
    new_element.fractions[i] = delta * max_fraction / defining_axis_steps;
  }

  // TODO: clamp acceleration to be a minimum value.
  const int total_loops = LOOPS_PER_STEP * defining_axis_steps;
  // There are three cases: either we accelerate, travel or decelerate.
  if (param->v0 == param->v1) {
    // Travel
    new_element.loops_accel = new_element.loops_decel = 0;
    new_element.loops_travel = total_loops;
    const float travel_speed = clip_hardware_frequency_limit(param->v0);
    new_element.travel_delay_cycles = round2int(TIMER_FREQUENCY / (LOOPS_PER_STEP * travel_speed));
  } else if (param->v0 < param->v1) {
    // acclereate
    new_element.loops_travel = new_element.loops_decel = new_element.travel_delay_cycles = 0;
    new_element.loops_accel = total_loops;

    // v1 = v0 + a*t -> t = (v1 - v0)/a
    // s = a/2 * t^2 + v0 * t; subsitution t from above.
    // a = (v1^2-v0^2)/(2*s)
    float acceleration = (sq(param->v1) - sq(param->v0)) / (2.0f * defining_axis_steps);
    //fprintf(stderr, "M-OP HZ: defining=%d ; accel=%.2f\n", defining_axis_steps, acceleration);
    // If we accelerated from zero to our first speed, this is how many steps
    // we needed. We need to go this index into our taylor series.
    const int accel_loops_from_zero =
      round2int(LOOPS_PER_STEP * (sq(param->v0 - 0) / (2.0f * acceleration)));

    new_element.accel_series_index = accel_loops_from_zero;
    new_element.hires_accel_cycles =
      round2int((1 << DELAY_CYCLE_SHIFT) * calcAccelerationCurveValueAt(new_element.accel_series_index, acceleration));
  } else {  // v0 > v1
    // decelerate
    new_element.loops_travel = new_element.loops_accel = new_element.travel_delay_cycles = 0;
    new_element.loops_decel = total_loops;

    float acceleration = (sq(param->v0) - sq(param->v1)) / (2.0f * defining_axis_steps);
    //fprintf(stderr, "M-OP HZ: defining=%d ; decel=%.2f\n", defining_axis_steps, acceleration);
    // We are into the taylor sequence this value up and reduce from there.
    const int accel_loops_from_zero =
      round2int(LOOPS_PER_STEP * (sq(param->v0 - 0) / (2.0f * acceleration)));

    new_element.accel_series_index = accel_loops_from_zero;
    new_element.hires_accel_cycles =
      round2int((1 << DELAY_CYCLE_SHIFT) * calcAccelerationCurveValueAt(new_element.accel_series_index, acceleration));
  }

  new_element.aux = param->aux_bits;
  new_element.state = STATE_FILLED;
  backend->enqueue(&new_element);
}

static int get_defining_axis_steps(const struct MotorMovement *param) {
  int defining_axis_steps = abs(param->steps[0]);
  for (int i = 1; i < BEAGLEG_NUM_MOTORS; ++i) {
    if (abs(param->steps[i]) > defining_axis_steps) {
      defining_axis_steps = abs(param->steps[i]);
    }
  }
  return defining_axis_steps;
}

static int beagleg_enqueue(void *ctx, const struct MotorMovement *param,
                           FILE *err_stream) {
  struct MotionQueue *backend = (struct MotionQueue*)ctx;
  const int defining_axis_steps = get_defining_axis_steps(param);
  if (defining_axis_steps == 0) {
    fprintf(err_stream ? err_stream : stderr, "zero steps. Ignoring command.\n");
    return 1;
  }

  if (defining_axis_steps > MAX_STEPS_PER_SEGMENT) {
    // We have more steps that we can enqueue in one chunk, so let's cut
    // it in pieces.
    const double a = (sqd(param->v1) - sqd(param->v0))/(2.0*defining_axis_steps);
    const int divisions = (defining_axis_steps / MAX_STEPS_PER_SEGMENT) + 1;
    int64_t hires_steps_per_div[BEAGLEG_NUM_MOTORS];
    for (int i = 0; i < BEAGLEG_NUM_MOTORS; ++i) {
      // (+1 to fix rounding trouble in the LSB)
      hires_steps_per_div[i] = ((int64_t)param->steps[i] << 32)/divisions + 1;
    }

    struct MotorMovement previous = {0}, accumulator = {0}, output;
    int64_t hires_step_accumulator[BEAGLEG_NUM_MOTORS] = {0};
    double previous_speed = param->v0;   // speed calculation in double

    for (int d = 0; d < divisions; ++d) {
      for (int i = 0; i < BEAGLEG_NUM_MOTORS; ++i) {
        hires_step_accumulator[i] += hires_steps_per_div[i];
        accumulator.steps[i] = hires_step_accumulator[i] >> 32;
        output.steps[i] = accumulator.steps[i] - previous.steps[i];
      }
      const int division_steps = get_defining_axis_steps(&output);
      // These squared values can get huge, lets not loose precision
      // here and do calculations in double (otherwise our results can
      // be a little bit off and fail to reach zero properly).
      const double v0squared = sqd(previous_speed);
      // v1 = v0 + a*t; t = (sqrt(v0^2 + 2 * a * steps) - v0) / a
      // -> v1 = sqrt(v0^ + 2 * a * steps)
      const double v1squared = v0squared + 2.0 * a * division_steps;
      // Rounding errors can make v1squared slightly negative...
      const double v1 = v1squared > 0.0 ? sqrt(v1squared) : 0;
      output.v0 = previous_speed;
      output.v1 = v1;
      beagleg_enqueue_internal(backend, &output, division_steps);
      previous = accumulator;
      previous_speed = v1;
    }
  } else {
    beagleg_enqueue_internal(backend, param, defining_axis_steps);
  }
  return 0;
}

static void beagleg_motor_enable(void *ctx, char on) {
  struct MotionQueue *backend = (struct MotionQueue*)ctx;
  backend->wait_queue_empty();
  backend->motor_enable(on);
}

static void beagleg_wait_queue_empty(void *ctx) {
  struct MotionQueue *backend = (struct MotionQueue*)ctx;
  backend->wait_queue_empty();
}

int beagleg_init_motor_ops(struct MotionQueue *backend,
                           struct MotorOperations *ops) {
  hardware_frequency_limit_ = 1e6;    // Don't go over 1 Mhz

  // Set up operations.
  ops->user_data = backend;
  ops->motor_enable = beagleg_motor_enable;
  ops->enqueue = beagleg_enqueue;
  ops->wait_queue_empty = beagleg_wait_queue_empty;

  return 0;
}
