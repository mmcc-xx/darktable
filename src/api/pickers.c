/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

// what a module's color_picker_apply does with a picker's sample, for a
// session without darktable's window: there the module has no controls,
// and its color_picker_apply reads them and works on the darkroom
// (darktable.develop). server.c takes the sample (_pick_sample); each
// function mirrors the branch of one module's color_picker_apply that
// writes its params, the settings its controls hold taken as darktable
// keeps them

#include "api/pickers.h"
#include "api/also_changed.h"
#include "common/chromatic_adaptation.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/illuminants.h"
#include "common/iop_profile.h"
#include "common/math.h"
#include "control/conf.h"
#include "develop/openmp_maths.h"
#include "develop/pixelpipe_hb.h"

#include <float.h>
#include <math.h>

// a setting by name; NULL when this darktable's module doesn't have it
#define _P(T, m, name) ((T *)(m)->get_p((m)->params, (name)))

// ---- monochrome (monochrome.c) ----------------------------------------------

static void _monochrome(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  float *a = _P(float, m, "a"), *b = _P(float, m, "b"), *size = _P(float, m, "size");
  if(!a || !b || !size) return;
  if(fabsf(*a - pick->mean[1]) < 0.0001f && fabsf(*b - pick->mean[2]) < 0.0001f) return;
  *a = pick->mean[1];
  *b = pick->mean[2];
  const float da = pick->max[1] - pick->min[1];
  const float db = pick->max[2] - pick->min[2];
  *size = CLAMP((da + db) / 128.0, .5, 3.0);
}

// ---- exposure (exposure.c) --------------------------------------------------

// _auto_set_exposure in its area mode "correction", the one the module
// shows on every update (gui_update): exposure that brings the area's
// lightness to the target darktable keeps for the module's lightness slider
static void _exposure(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  if(pick->max[0] < pick->min[0]) return;
  const dt_iop_order_iccprofile_info_t *const input_profile = dt_ioppr_get_pipe_input_profile_info(pipe);
  if(!input_profile) return;

  dt_aligned_pixel_t XYZ, Lab;
  dot_product(pick->mean, input_profile->matrix_in, XYZ);
  dt_XYZ_to_Lab(XYZ, Lab);
  Lab[1] = Lab[2] = Lab[3] = 0.f;
  dt_Lab_to_XYZ(Lab, XYZ);

  // the slider (0-100 %, one decimal) holds the saved lightness
  const float lightness = CLAMP(dt_conf_get_float("darkroom/modules/exposure/lightness"), 0.0f, 100.0f);
  dt_aligned_pixel_t Lch_target = { roundf(10.0f * lightness) / 10.0f, 0.f, 0.f, 0.f };
  dt_aligned_pixel_t Lab_target = { 0.f }, XYZ_target = { 0.f };
  dt_LCH_2_Lab(Lch_target, Lab_target);
  dt_Lab_to_XYZ(Lab_target, XYZ_target);

  if(XYZ[1] > 1e-5f && XYZ_target[1] > 1e-5f) dt_api_exposure_set_white(m, XYZ[1] / XYZ_target[1]);
}

// ---- color calibration (channelmixerrgb.c) ----------------------------------

// a slider's value as darktable keeps it from conf (gui_update), rounded to
// the slider's digits (1)
static float _conf_slider(const char *key, const float def, const float max)
{
  const float v = dt_conf_key_exists(key) ? CLAMP(dt_conf_get_float(key), 0.0f, max) : def;
  return roundf(10.0f * v) / 10.0f;
}

// _auto_set_illuminant in its area mode "correction", the one gui_update
// always sets: the illuminant that maps the area's color to the target
// darktable keeps for the module's mapping sliders (neutral grey by
// default), optionally through the channel mixing. "measure" writes no
// settings
static void _channelmixerrgb(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  if(pick->max[0] < pick->min[0]) return;
  float *x = _P(float, m, "x"), *y = _P(float, m, "y"), *temperature = _P(float, m, "temperature");
  int *illuminant = _P(int, m, "illuminant");
  const int *adaptation = _P(int, m, "adaptation");
  const float *red = _P(float, m, "red"), *green = _P(float, m, "green"), *blue = _P(float, m, "blue");
  const int *norm_r = _P(int, m, "normalize_R"), *norm_g = _P(int, m, "normalize_G"),
            *norm_b = _P(int, m, "normalize_B");
  if(!x || !y || !temperature || !illuminant || !adaptation || !red || !green || !blue || !norm_r || !norm_g
     || !norm_b)
    return;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(pipe);
  if(!work_profile) return;
  const dt_adaptation_t kind = *adaptation;

  dt_aligned_pixel_t XYZ;
  dot_product(pick->mean, work_profile->matrix_in, XYZ);

  const gboolean use_mixing = dt_conf_key_exists("darkroom/modules/channelmixerrgb/use_mixing")
                                ? dt_conf_get_bool("darkroom/modules/channelmixerrgb/use_mixing")
                                : TRUE;
  // the channel mixing matrix (commit_params)
  dt_colormatrix_t MIX = { { 0.f } };
  const float nr = *norm_r ? MAX(NORM_MIN, red[0] + red[1] + red[2]) : 1.0f;
  const float ng = *norm_g ? MAX(NORM_MIN, green[0] + green[1] + green[2]) : 1.0f;
  const float nb = *norm_b ? MAX(NORM_MIN, blue[0] + blue[1] + blue[2]) : 1.0f;
  for_three_channels(i)
  {
    MIX[0][i] = red[i] / nr;
    MIX[1][i] = green[i] / ng;
    MIX[2][i] = blue[i] / nb;
  }

  // the target color in LMS
  const dt_aligned_pixel_t Lch_target
    = { _conf_slider("darkroom/modules/channelmixerrgb/lightness", 50.0f, 100.0f),
        _conf_slider("darkroom/modules/channelmixerrgb/chroma", 0.0f, 128.0f),
        _conf_slider("darkroom/modules/channelmixerrgb/hue", 0.0f, 360.0f) / 360.f, 0.f };
  dt_aligned_pixel_t Lab_target = { 0.f }, XYZ_target = { 0.f }, LMS_target = { 0.f };
  dt_LCH_2_Lab(Lch_target, Lab_target);
  dt_Lab_to_XYZ(Lab_target, XYZ_target);
  dt_vector_div1(XYZ_target, XYZ_target, MAX(NORM_MIN, XYZ_target[1]));
  convert_any_XYZ_to_LMS(XYZ_target, LMS_target, kind);

  // undo the channel mixing on the target: the color wanted after the CAT
  if(use_mixing)
  {
    float MIX_3x3[9], MIX_INV_3x3[9];
    pack_3xSSE_to_3x3(MIX, MIX_3x3);
    matrice_pseudoinverse((float(*)[3])MIX_3x3, (float(*)[3])MIX_INV_3x3, 3);
    dt_colormatrix_t MIX_INV;
    transpose_3x3_to_3xSSE(MIX_INV_3x3, MIX_INV);
    dt_aligned_pixel_t temp = { 0.0f };
    dot_product(LMS_target, MIX_INV, temp);
    convert_any_LMS_to_XYZ(temp, XYZ_target, kind);
    dt_vector_div1(XYZ_target, XYZ_target, MAX(NORM_MIN, XYZ_target[1]));
    convert_any_XYZ_to_LMS(XYZ_target, LMS_target, kind);
  }

  // the input color in LMS
  dt_aligned_pixel_t LMS = { 0.f };
  dt_vector_div1(XYZ, XYZ, MAX(NORM_MIN, XYZ[1]));
  convert_any_XYZ_to_LMS(XYZ, LMS, kind);

  // target / input = D50 / illuminant, solved for the illuminant
  dt_aligned_pixel_t D50, illuminant_LMS = { 0.f }, illuminant_XYZ = { 0.f };
  convert_D50_to_LMS(kind, D50);
  for_three_channels(c) illuminant_LMS[c] = D50[c] * LMS[c] / LMS_target[c];
  convert_any_LMS_to_XYZ(illuminant_LMS, illuminant_XYZ, kind);
  const float sum = fmaxf(illuminant_XYZ[0] + illuminant_XYZ[1] + illuminant_XYZ[2], NORM_MIN);

  *x = illuminant_XYZ[0] / sum;
  *y = illuminant_XYZ[1] / sum;
  // custom: the daylight and black body models aren't accurate enough to
  // match colors
  *illuminant = DT_ILLUMINANT_CUSTOM;
  *temperature = dt_api_cct(*x, *y);
}

// ---- agx (agx.c) ------------------------------------------------------------

// an exposure range end from the area's darkest or brightest channel
// (_apply_auto_black_exposure, _apply_auto_white_exposure)
static float _agx_ev(const dt_iop_module_t *m, const float norm, const float lo, const float hi)
{
  const float *scaling = _P(float, m, "dynamic_range_scaling");
  return CLAMPF(log2f(fmaxf(1e-6f, norm) / 0.18f) * (1.f + (scaling ? *scaling : 0.f)), lo, hi);
}

// color_picker_apply: then _update_pivot_x keeps the pivot's place in EV.
// with auto gamma on, darktable shows the new gamma on its slider only (the
// setting is left as it was)
static void _agx_pick(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick, const gboolean black,
                      const gboolean white, const gboolean pivot)
{
  float *b = _P(float, m, "range_black_relative_ev"), *w = _P(float, m, "range_white_relative_ev");
  float *pivot_x = _P(float, m, "curve_pivot_x");
  if(!b || !w || !pivot_x) return;
  const float old_b = *b, old_w = *w;
  if(black) *b = _agx_ev(m, MIN(pick->min[0], MIN(pick->min[1], pick->min[2])), -20.f, -0.1f);
  if(white) *w = _agx_ev(m, MAX(pick->max[0], MAX(pick->max[1], pick->max[2])), 0.1f, 20.f);
  if(pivot)
  {
    // _apply_auto_pivot_x: the pivot to the area's luminance
    const dt_iop_order_iccprofile_info_t *const profile = dt_ioppr_get_pipe_work_profile_info(pipe);
    if(!profile) return;
    const float lum = dt_ioppr_get_rgb_matrix_luminance(pick->mean, profile->matrix_in, profile->lut_in,
                                                       profile->unbounded_coeffs_in, profile->lutsize,
                                                       profile->nonlinearlut);
    const float ev = CLAMPF(log2f(fmaxf(1e-6f, lum) / 0.18f), *b, *w);
    *pivot_x = (ev - *b) / (*w - *b);
  }
  // _update_pivot_x
  const float pivot_ev = old_b + *pivot_x * (old_w - old_b);
  *pivot_x = (CLAMPF(pivot_ev, *b, *w) - *b) / (*w - *b);
}

// agx's tone curve at x (0-1 of the exposure range), from its settings:
// _calculate_tone_mapping_params and _apply_curve, with their helpers
// (_adjust_pivot, _calculate_pivot_y_at_gamma, _scale, _sigmoid, the
// fallback toe and shoulder)
typedef struct _agx_curve_t
{
  float pivot_x, pivot_y, slope, intercept, target_black, target_white;
  float toe_power, toe_tx, toe_ty, toe_scale, toe_fb_coeff, toe_fb_power;
  float sh_power, sh_tx, sh_ty, sh_scale, sh_fb_coeff, sh_fb_power;
  gboolean convex_toe, concave_shoulder;
} _agx_curve_t;

static float _agx_f(const dt_iop_module_t *m, const char *name, const float def)
{
  const float *v = _P(float, m, name);
  return v ? *v : def;
}

static float _agx_pivot_y_at(const dt_iop_module_t *m, const float gamma)
{
  return powf(CLAMPF(_agx_f(m, "curve_pivot_y_linear_output", 0.18f),
                     _agx_f(m, "curve_target_display_black_ratio", 0.f),
                     _agx_f(m, "curve_target_display_white_ratio", 1.f)),
              1.f / gamma);
}

static float _agx_scale(const float limit_x, const float limit_y, const float tx, const float ty,
                        const float slope, const float power)
{
  const float eps = 1e-6f;
  const float projected_rise = slope * fmaxf(eps, limit_x - tx);
  const float actual_rise = fmaxf(eps, limit_y - ty);
  const float base = fmaxf(eps, powf(actual_rise, -power) - powf(projected_rise, -power));
  return fminf(1e9f, powf(base, -1.f / power));
}

static void _agx_curve(const dt_iop_module_t *m, _agx_curve_t *c)
{
  const float eps = 1e-6f, default_gamma = 2.2f;
  const gboolean *auto_gamma = _P(gboolean, m, "auto_gamma");
  const float black_ev = _agx_f(m, "range_black_relative_ev", -10.f);
  const float white_ev = _agx_f(m, "range_white_relative_ev", 6.5f);
  const float pivot_y_linear = _agx_f(m, "curve_pivot_y_linear_output", 0.18f);
  // _adjust_pivot
  c->pivot_x = CLAMPF(_agx_f(m, "curve_pivot_x", 0.5f), eps, 1.f - eps);
  float gamma = _agx_f(m, "curve_gamma", default_gamma);
  if(auto_gamma && *auto_gamma)
    gamma = CLAMPF(c->pivot_x > 0.f && pivot_y_linear > 0.f ? log2f(pivot_y_linear) / log2f(c->pivot_x) : gamma,
                   0.01f, 100.f);
  c->pivot_y = _agx_pivot_y_at(m, gamma);

  const float range_adjusted_slope = _agx_f(m, "curve_contrast_around_pivot", 2.8f) * ((white_ev - black_ev) / 16.5f);
  // _calculate_slope_gamma_compensation
  const float d_current = gamma * powf(fmaxf(eps, c->pivot_y), gamma - 1.0f);
  const float d_default
    = default_gamma * powf(fmaxf(eps, _agx_pivot_y_at(m, default_gamma)), default_gamma - 1.0f);
  c->slope = range_adjusted_slope / (d_current / d_default);

  // toe
  c->target_black = powf(_agx_f(m, "curve_target_display_black_ratio", 0.f), 1.f / gamma);
  c->toe_power = fmaxf(0.01f, _agx_f(m, "curve_toe_power", 1.5f));
  const float toe_length_y = (c->pivot_y - c->target_black) * _agx_f(m, "curve_linear_ratio_below_pivot", 0.f);
  float dx_below = toe_length_y / c->slope;
  c->toe_tx = fmaxf(eps, c->pivot_x - dx_below);
  dx_below = c->pivot_x - c->toe_tx;
  c->toe_ty = c->pivot_y - c->slope * dx_below;
  c->toe_scale = -_agx_scale(1.f, 1.f - c->target_black, 1.f - c->toe_tx, 1.f - c->toe_ty, c->slope, c->toe_power);
  const float toe_dy = fmaxf(eps, c->toe_ty - c->target_black);
  c->convex_toe = toe_dy / c->toe_tx > c->slope;
  c->toe_fb_power = c->slope * c->toe_tx / toe_dy;
  c->toe_fb_coeff = toe_dy / powf(c->toe_tx, c->toe_fb_power);
  c->intercept = c->toe_ty - c->slope * c->toe_tx;

  // shoulder
  c->target_white = powf(_agx_f(m, "curve_target_display_white_ratio", 1.f), 1.f / gamma);
  const float sh_length_y = (c->target_white - c->pivot_y) * _agx_f(m, "curve_linear_ratio_above_pivot", 0.f);
  float dx_above = sh_length_y / c->slope;
  c->sh_tx = fminf(1.f - eps, c->pivot_x + dx_above);
  dx_above = c->sh_tx - c->pivot_x;
  c->sh_ty = c->pivot_y + c->slope * dx_above;
  c->sh_power = fmaxf(0.01f, _agx_f(m, "curve_shoulder_power", 1.5f));
  c->sh_scale = _agx_scale(1.f, c->target_white, c->sh_tx, c->sh_ty, c->slope, c->sh_power);
  const float sh_length_x = 1.f - c->sh_tx;
  const float sh_dy = fmaxf(eps, c->target_white - c->sh_ty);
  c->concave_shoulder = sh_dy / sh_length_x > c->slope;
  c->sh_fb_power = c->slope * sh_length_x / sh_dy;
  c->sh_fb_coeff = sh_dy / powf(sh_length_x, c->sh_fb_power);
}

static float _agx_sigmoid(const float x, const float scale, const float slope, const float power, const float tx,
                          const float ty)
{
  const float v = slope * (x - tx) / scale;
  return scale * (v / powf(1.f + powf(v, power), 1.f / power)) + ty;
}

static float _agx_apply_curve(const float x, const _agx_curve_t *c)
{
  float y;
  if(x < c->toe_tx)
    y = c->convex_toe
          ? (x < 0.f ? c->target_black : c->target_black + fmaxf(0.f, c->toe_fb_coeff * powf(x, c->toe_fb_power)))
          : _agx_sigmoid(x, c->toe_scale, c->slope, c->toe_power, c->toe_tx, c->toe_ty);
  else if(x <= c->sh_tx)
    y = c->slope * x + c->intercept;
  else
    y = c->concave_shoulder
          ? (x >= 1.f ? c->target_white
                      : c->target_white - fmaxf(0.f, c->sh_fb_coeff * powf(1.f - x, c->sh_fb_power)))
          : _agx_sigmoid(x, c->sh_scale, c->slope, c->sh_power, c->sh_tx, c->sh_ty);
  return CLAMPF(y, c->target_black, c->target_white);
}

// _apply_auto_pivot_xy: the pivot to the area's luminance, its output to
// where the curve maps that now (the pivot's brightness kept)
static void _agx_pivot_target(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  float *b = _P(float, m, "range_black_relative_ev"), *w = _P(float, m, "range_white_relative_ev");
  float *pivot_x = _P(float, m, "curve_pivot_x"), *pivot_y = _P(float, m, "curve_pivot_y_linear_output");
  const float *gamma = _P(float, m, "curve_gamma");
  const dt_iop_order_iccprofile_info_t *const profile = dt_ioppr_get_pipe_work_profile_info(pipe);
  if(!b || !w || !pivot_x || !pivot_y || !gamma || !profile) return;
  const float lum = dt_ioppr_get_rgb_matrix_luminance(pick->mean, profile->matrix_in, profile->lut_in,
                                                     profile->unbounded_coeffs_in, profile->lutsize,
                                                     profile->nonlinearlut);
  const float ev = CLAMPF(log2f(fmaxf(1e-6f, lum) / 0.18f), *b, *w);
  const float x = (ev - *b) / (*w - *b);
  _agx_curve_t c;
  _agx_curve(m, &c);
  *pivot_y = powf(_agx_apply_curve(x, &c), *gamma);
  *pivot_x = x;
}

static void _agx_black(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _agx_pick(m, pipe, pick, TRUE, FALSE, FALSE);
}

static void _agx_white(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _agx_pick(m, pipe, pick, FALSE, TRUE, FALSE);
}

static void _agx_auto_tune(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _agx_pick(m, pipe, pick, TRUE, TRUE, FALSE);
}

static void _agx_pivot(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _agx_pick(m, pipe, pick, FALSE, FALSE, TRUE);
}

// ---- filmic rgb (filmicrgb.c) -----------------------------------------------

// get_pixel_norm, by the preserve chrominance values
static float _filmic_norm(const float *rgb, const int method, const dt_iop_order_iccprofile_info_t *profile)
{
  switch(method)
  {
    case 1: // max RGB
      return MAX(rgb[0], MAX(rgb[1], rgb[2]));
    case 3: // RGB power norm
    {
      float num = 0.f, den = 0.f;
      for(int c = 0; c < 3; c++)
      {
        const float v = fabsf(rgb[c]);
        num += v * v * v;
        den += v * v;
      }
      return num / fmaxf(den, 1e-12f);
    }
    case 4: // RGB euclidean norm (legacy)
      return sqrtf(sqf(rgb[0]) + sqf(rgb[1]) + sqf(rgb[2]));
    case 5: // RGB euclidean norm
      return sqrtf(sqf(rgb[0]) + sqf(rgb[1]) + sqf(rgb[2])) * 0.5773502691896258f;
    default: // luminance Y, no
      return profile ? dt_ioppr_get_rgb_matrix_luminance(rgb, profile->matrix_in, profile->lut_in,
                                                         profile->unbounded_coeffs_in, profile->lutsize,
                                                         profile->nonlinearlut)
                     : dt_camera_rgb_luminance(rgb);
  }
}

// apply_auto_grey, apply_auto_black, apply_auto_white_point_source and
// apply_autotune; then _compute_output_power
static void _filmic_pick(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick, const gboolean grey,
                         const gboolean black, const gboolean white)
{
  float *gs = _P(float, m, "grey_point_source"), *bs = _P(float, m, "black_point_source"),
        *ws = _P(float, m, "white_point_source"), *power = _P(float, m, "output_power");
  const float *security = _P(float, m, "security_factor"), *gt = _P(float, m, "grey_point_target");
  const int *preserve = _P(int, m, "preserve_color");
  const gboolean *custom_grey = _P(gboolean, m, "custom_grey");
  if(!gs || !bs || !ws || !power || !security || !gt || !preserve) return;
  const dt_iop_order_iccprofile_info_t *const profile = dt_ioppr_get_pipe_work_profile_info(pipe);
  const gboolean tune = grey && black && white;
  if(grey && (!tune || (custom_grey && *custom_grey)))
  {
    const float prev = *gs;
    *gs = CLAMP(100.f * _filmic_norm(pick->mean, *preserve, profile) / 2.0f, 0.001f, 100.0f);
    // the grey picker alone shifts the range with it
    if(!tune)
    {
      const float var = log2f(prev / *gs);
      *bs -= var;
      *ws += var;
    }
  }
  if(white)
  {
    const float w = _filmic_norm(pick->max, 1, profile);
    *ws = CLAMP(log2f(w / (*gs / 100.0f)), 1.0f, 16.0f) * (1.0f + *security / 100.0f);
  }
  if(black)
  {
    const float b = _filmic_norm(pick->min, 1, profile);
    *bs = fmaxf(CLAMP(log2f(b / (*gs / 100.0f)), -16.0f, -1.0f) * (1.0f + *security / 100.0f), -16.0f);
  }
  const dt_introspection_field_t *f = m->so->get_f ? m->so->get_f("output_power") : NULL;
  const float lo = f ? f->Float.Min : 1.f, hi = f ? f->Float.Max : 10.f;
  *power = CLAMPF(logf(*gt / 100.0f) / logf(-*bs / (*ws - *bs)), lo, hi);
}

static void _filmic_grey(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _filmic_pick(m, pipe, pick, TRUE, FALSE, FALSE);
}

static void _filmic_black(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _filmic_pick(m, pipe, pick, FALSE, TRUE, FALSE);
}

static void _filmic_white(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _filmic_pick(m, pipe, pick, FALSE, FALSE, TRUE);
}

static void _filmic_auto_tune(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _filmic_pick(m, pipe, pick, TRUE, TRUE, TRUE);
}

// ---- rgb levels (rgblevels.c) -----------------------------------------------

// color_picker_apply: a point of the channel the module's tab shows from the
// sample's first channel (red, whatever the tab), kept between the others.
// headless there is no tab: the first, the one darktable shows after
// gui_update and in "RGB, linked channels"
static void _rgblevels_pick(dt_iop_module_t *m, const dt_api_pick_t *pick, const int point)
{
  float *levels = _P(float, m, "levels");
  if(!levels) return;
  float *l = levels; // levels[0][0..2]
  const float v = pick->mean[0];
  if(point == 0)
    l[0] = v > l[1] ? l[1] - FLT_EPSILON : v;
  else if(point == 1)
    l[1] = v < l[0] || v > l[2] ? l[1] : v;
  else
    l[2] = v < l[1] ? l[1] + FLT_EPSILON : v;
}

static void _rgblevels_black(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _rgblevels_pick(m, pick, 0);
}

static void _rgblevels_gray(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _rgblevels_pick(m, pick, 1);
}

static void _rgblevels_white(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _rgblevels_pick(m, pick, 2);
}

// ---- color balance rgb (colorbalancergb.c) -----------------------------------

// pipe_RGB_to_Ych: a sample in the module's profile to Ych (D65)
static gboolean _cbrgb_Ych(const dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const float *rgb, dt_aligned_pixel_t Ych)
{
  const dt_iop_order_iccprofile_info_t *const profile = dt_ioppr_get_pipe_current_profile_info((dt_iop_module_t *)m, pipe);
  if(!profile) return FALSE;
  dt_aligned_pixel_t XYZ_D50 = { 0.f }, XYZ_D65 = { 0.f };
  dt_ioppr_rgb_matrix_to_xyz(rgb, XYZ_D50, profile->matrix_in_transposed, profile->lut_in,
                             profile->unbounded_coeffs_in, profile->lutsize, profile->nonlinearlut);
  XYZ_D50_to_D65(XYZ_D50, XYZ_D65);
  XYZ_to_Ych(XYZ_D65, Ych);
  return TRUE;
}

// color_picker_apply's hue pickers: the opposite hue of the area (darktable
// takes the area's own with ctrl held, which a press without the keyboard
// never has) and its chroma
static void _cbrgb_hue(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick, const char *hue,
                       const char *chroma)
{
  float *h = _P(float, m, hue), *c = _P(float, m, chroma);
  dt_aligned_pixel_t Ych = { 0.f };
  if(!h || !c || !_cbrgb_Ych(m, pipe, pick->mean, Ych)) return;
  // YRG_RAD_TO_CONVENTIONAL_DEG, + 180
  *h = rad2degf(get_hue_angle_from_Ych(Ych)) + 30.f + 180.f;
  *c = Ych[1] * Ych[0];
}

static void _cbrgb_offset(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _cbrgb_hue(m, pipe, pick, "global_H", "global_C");
}

static void _cbrgb_lift(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _cbrgb_hue(m, pipe, pick, "shadows_H", "shadows_C");
}

static void _cbrgb_power(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _cbrgb_hue(m, pipe, pick, "midtones_H", "midtones_C");
}

static void _cbrgb_gain(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _cbrgb_hue(m, pipe, pick, "highlights_H", "highlights_C");
}

static void _cbrgb_white_fulcrum(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  float *f = _P(float, m, "white_fulcrum");
  dt_aligned_pixel_t Ych = { 0.f };
  if(f && _cbrgb_Ych(m, pipe, pick->max, Ych)) *f = log2f(Ych[0]);
}

static void _cbrgb_grey_fulcrum(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  float *f = _P(float, m, "grey_fulcrum");
  dt_aligned_pixel_t Ych = { 0.f };
  if(f && _cbrgb_Ych(m, pipe, pick->mean, Ych)) *f = Ych[0];
}

// ---- levels (levels.c) -------------------------------------------------------

// color_picker_apply: a point from the sample's lightness (Lab L / 100),
// kept between the others
static void _levels_pick(dt_iop_module_t *m, const dt_api_pick_t *pick, const int point)
{
  float *l = _P(float, m, "levels");
  if(!l) return;
  const float v = pick->mean[0] / 100.0f;
  if(point == 0)
    l[0] = v > l[1] ? l[1] - FLT_EPSILON : v;
  else if(point == 1)
    l[1] = v < l[0] || v > l[2] ? l[1] : v;
  else
    l[2] = v < l[1] ? l[1] + FLT_EPSILON : v;
}

static void _levels_black(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _levels_pick(m, pick, 0);
}

static void _levels_gray(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _levels_pick(m, pick, 1);
}

static void _levels_white(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _levels_pick(m, pick, 2);
}

// darktable shows the middle gray slider, and its picker, only with custom
// middle gray values (gui_changed)
static const char *_filmic_grey_unavailable(const dt_iop_module_t *m)
{
  const gboolean *custom_grey = _P(gboolean, m, "custom_grey");
  return custom_grey && !*custom_grey ? "the picker is hidden while \"use custom middle-gray values\" is off" : NULL;
}

// without a chromatic adaptation (monochrome photos, a second instance)
// darktable hides the picker (_update_illuminants)
static const char *_channelmixerrgb_unavailable(const dt_iop_module_t *m)
{
  const int *adaptation = _P(int, m, "adaptation");
  return adaptation && (*adaptation == DT_ADAPTATION_RGB || *adaptation == DT_ADAPTATION_LAST)
           ? "the picker is hidden while adaptation is \"none (bypass)\""
           : NULL;
}

// ---- basic adjustments (basicadj.c) -----------------------------------------

// color_picker_apply: the middle gray is the area's luminance in percent
// (the slider's clamp to 0.05-100 acts on the slider only, the setting is
// kept as computed). without a work profile darktable takes the camera
// luminance, not scaled to percent, and so does this
static void _basicadj(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  float *middle_grey = _P(float, m, "middle_grey");
  if(!middle_grey) return;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(m, pipe);
  *middle_grey = work_profile ? dt_ioppr_get_rgb_matrix_luminance(pick->mean, work_profile->matrix_in,
                                                                  work_profile->lut_in,
                                                                  work_profile->unbounded_coeffs_in,
                                                                  work_profile->lutsize, work_profile->nonlinearlut)
                                  * 100.f
                              : dt_camera_rgb_luminance(pick->mean);
}

// ---- borders (borders.c) ----------------------------------------------------

// color_picker_apply: the point's color, as the pipe has it, becomes the
// border's or the frame line's color. a point equal to either color is
// ignored (darktable's guard against loops)
static void _borders_pick(dt_iop_module_t *m, const dt_api_pick_t *pick, const gboolean frame)
{
  float *color = _P(float, m, "color"), *frame_color = _P(float, m, "frame_color");
  if(!color || !frame_color) return;
  gboolean same_color = TRUE, same_frame = TRUE;
  for(int c = 0; c < 3; c++)
  {
    same_color &= fabsf(color[c] - pick->mean[c]) < 0.0001f;
    same_frame &= fabsf(frame_color[c] - pick->mean[c]) < 0.0001f;
  }
  if(same_color || same_frame) return;
  float *target = frame ? frame_color : color;
  for(int c = 0; c < 3; c++) target[c] = pick->mean[c];
}

static void _borders_border(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _borders_pick(m, pick, FALSE);
}

static void _borders_frame(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _borders_pick(m, pick, TRUE);
}

// ---- colorize (colorize.c) --------------------------------------------------

// color_picker_apply: the point's Lab color as sRGB, its hue and saturation
// (lightness dropped). unchanged settings (within 0.0001) are left alone
static void _colorize(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  float *hue = _P(float, m, "hue"), *saturation = _P(float, m, "saturation");
  if(!hue || !saturation) return;
  float H = .0f, S = .0f, L = .0f;
  dt_aligned_pixel_t XYZ, rgb;
  dt_Lab_to_XYZ(pick->mean, XYZ);
  dt_XYZ_to_sRGB(XYZ, rgb);
  rgb2hsl(rgb, &H, &S, &L);
  if(fabsf(*hue - H) < 0.0001f && fabsf(*saturation - S) < 0.0001f) return;
  *hue = H;
  *saturation = S;
}

// ---- graduated density (graduatednd.c) --------------------------------------

// color_picker_apply: the point's color (pipe RGB, not converted) as hue and
// saturation, lightness dropped
static void _graduatednd(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  float *hue = _P(float, m, "hue"), *saturation = _P(float, m, "saturation");
  if(!hue || !saturation) return;
  float H = .0f, S = .0f, L = .0f;
  rgb2hsl(pick->mean, &H, &S, &L);
  if(fabsf(*hue - H) < 0.0001f && fabsf(*saturation - S) < 0.0001f) return;
  *hue = H;
  *saturation = S;
}

// ---- split-toning (splittoning.c) -------------------------------------------

// color_picker_apply: the point's color as hue and saturation of the
// highlights or of the shadows (any other picker)
static void _splittoning_pick(dt_iop_module_t *m, const dt_api_pick_t *pick, const gboolean highlights)
{
  float *hue = _P(float, m, highlights ? "highlight_hue" : "shadow_hue");
  float *saturation = _P(float, m, highlights ? "highlight_saturation" : "shadow_saturation");
  if(!hue || !saturation) return;
  float H = .0f, S = .0f, L = .0f;
  rgb2hsl(pick->mean, &H, &S, &L);
  if(fabsf(*hue - H) < 0.0001f && fabsf(*saturation - S) < 0.0001f) return;
  *hue = H;
  *saturation = S;
}

static void _splittoning_shadows(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _splittoning_pick(m, pick, FALSE);
}

static void _splittoning_highlights(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _splittoning_pick(m, pick, TRUE);
}

// ---- watermark (watermark.c) ------------------------------------------------

// color_picker_apply: the point's color, as the pipe has it, becomes the
// text color. a color equal to the current one (within 0.0001) is ignored.
// darktable makes the picker sensitive for text and svg watermarks only
// (it stays visible), so nothing is checked here
static void _watermark(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  float *color = _P(float, m, "color");
  if(!color) return;
  if(fabsf(color[0] - pick->mean[0]) < 0.0001f && fabsf(color[1] - pick->mean[1]) < 0.0001f
     && fabsf(color[2] - pick->mean[2]) < 0.0001f)
    return;
  for(int c = 0; c < 3; c++) color[c] = pick->mean[c];
}

// ---- color harmonizer (colorharmonizer.c) -----------------------------------

// _picked_color_to_hue: the point's color (negatives clipped) in the work
// profile as a darktable UCS hue, 0-1. FALSE without a work profile
static gboolean _colorharmonizer_hue(const dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick, float *hue)
{
  const dt_iop_order_iccprofile_info_t *const work_profile = pipe->work_profile_info;
  if(!work_profile) return FALSE;

  dt_aligned_pixel_t rgb, XYZ, XYZ_D65, xyY, JCH;
  for(int c = 0; c < 3; c++) rgb[c] = fmaxf(pick->mean[c], 0.0f);
  rgb[3] = 0.0f;
  dot_product(rgb, work_profile->matrix_in, XYZ);
  XYZ[3] = 0.0f;

  const float L_white = Y_to_dt_UCS_L_star(1.0f);
  XYZ_D50_to_D65(XYZ, XYZ_D65);
  dt_D65_XYZ_to_xyY(XYZ_D65, xyY);
  xyY_to_dt_UCS_JCH(xyY, L_white, JCH);
  *hue = (JCH[2] + M_PI_F) / DT_2PI_F;
  return TRUE;
}

// color_picker_apply, anchor hue: darktable's gui_changed afterwards only
// redraws and syncs the vectorscope, it writes no setting
static void _colorharmonizer_anchor(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  float *anchor_hue = _P(float, m, "anchor_hue");
  float hue = 0.0f;
  if(!anchor_hue || !_colorharmonizer_hue(pipe, pick, &hue)) return;
  *anchor_hue = hue;
}

// darktable hides the anchor hue slider, and its picker, with the custom
// harmony rule (gui_changed, _update_visibility); 9 is the rule's custom
static const char *_colorharmonizer_unavailable(const dt_iop_module_t *m)
{
  const int *rule = _P(int, m, "rule");
  return rule && *rule == 9 ? "the picker is hidden while the harmony rule is \"custom\"" : NULL;
}

// ---- color equalizer (colorequal.c) -----------------------------------------

// color_picker_apply, white level: the luminance Y (D65) of the area's
// brightest channels, in EV. the slider is set inside a gui update, so the
// setting is written as computed (not clamped to -2..16). darktable's
// gui_changed only redraws, it writes no setting. without a profile darktable
// takes log2 of 0; nothing is written here
static void _colorequal_white_level(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  float *white_level = _P(float, m, "white_level");
  if(!white_level) return;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(m, pipe);
  if(!work_profile) return;

  dt_aligned_pixel_t XYZ_D50 = { 0.0f, 0.0f, 0.0f, 0.0f }, XYZ_D65 = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t Ych = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_ioppr_rgb_matrix_to_xyz(pick->max, XYZ_D50, work_profile->matrix_in_transposed, work_profile->lut_in,
                             work_profile->unbounded_coeffs_in, work_profile->lutsize, work_profile->nonlinearlut);
  XYZ_D50_to_D65(XYZ_D50, XYZ_D65);
  XYZ_to_Ych(XYZ_D65, Ych);
  *white_level = log2f(Ych[0]);
}

// ---- filmic (filmic.c, the old filmic, op "filmic") -------------------------

// sanitize_latitude: the linear part can't be longer than the dynamic range
// (darktable's slider set inside it is a gui update: the setting only)
static void _filmic_v1_latitude(dt_iop_module_t *m)
{
  float *latitude = _P(float, m, "latitude_stops");
  const float *ws = _P(float, m, "white_point_source"), *bs = _P(float, m, "black_point_source");
  if(!latitude || !ws || !bs) return;
  if(*latitude > (*ws - *bs) * 0.99f) *latitude = (*ws - *bs) * 0.99f;
}

// apply_auto_grey, apply_auto_black, apply_auto_white_point_source and
// apply_autotune: the sample is Lab (the module's default color space), its
// Y is the luminance. the grey picker alone shifts the range with it and
// the others sanitize the latitude (the auto tune after all three)
static void _filmic_v1_pick(dt_iop_module_t *m, const dt_api_pick_t *pick, const gboolean grey,
                            const gboolean black, const gboolean white)
{
  float *gs = _P(float, m, "grey_point_source"), *bs = _P(float, m, "black_point_source"),
        *ws = _P(float, m, "white_point_source");
  const float *security = _P(float, m, "security_factor");
  if(!gs || !bs || !ws || !security) return;
  const float noise = powf(2.0f, -16.0f);
  dt_aligned_pixel_t XYZ = { 0.0f };
  const gboolean tune = grey && black && white;

  if(grey)
  {
    dt_Lab_to_XYZ(pick->mean, XYZ);
    const float prev = *gs;
    *gs = 100.f * XYZ[1];
    // apply_auto_grey only (not apply_autotune)
    if(!tune)
    {
      const float var = Log2(prev / *gs);
      *bs -= var;
      *ws += var;
    }
  }
  float EVmin = *bs, EVmax = *ws;
  if(black)
  {
    dt_Lab_to_XYZ(pick->min, XYZ);
    EVmin = Log2Thres(XYZ[1] / (*gs / 100.0f), noise) * (1.0f + *security / 100.0f);
  }
  if(white)
  {
    dt_Lab_to_XYZ(pick->max, XYZ);
    EVmax = Log2Thres(XYZ[1] / (*gs / 100.0f), noise) * (1.0f + *security / 100.0f);
  }
  if(black) *bs = EVmin;
  if(white) *ws = EVmax;
  if(black || white) _filmic_v1_latitude(m);
}

static void _filmic_v1_grey(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _filmic_v1_pick(m, pick, TRUE, FALSE, FALSE);
}

static void _filmic_v1_black(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _filmic_v1_pick(m, pick, FALSE, TRUE, FALSE);
}

static void _filmic_v1_white(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _filmic_v1_pick(m, pick, FALSE, FALSE, TRUE);
}

static void _filmic_v1_auto_tune(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _filmic_v1_pick(m, pick, TRUE, TRUE, TRUE);
}

// ---- profile gamma (profile_gamma.c) ----------------------------------------

// apply_auto_grey, apply_auto_black, apply_auto_dynamic_range and
// apply_autotune: the sample is linear RGB, its brightest channel the value
static void _profile_gamma_pick(dt_iop_module_t *m, const dt_api_pick_t *pick, const gboolean grey,
                                const gboolean black, const gboolean white)
{
  float *gp = _P(float, m, "grey_point"), *shadows = _P(float, m, "shadows_range"),
        *range = _P(float, m, "dynamic_range");
  const float *security = _P(float, m, "security_factor");
  if(!gp || !shadows || !range || !security) return;
  const float noise = powf(2.0f, -16.0f);
  const float k = 1.0f + *security / 100.0f;

  if(grey) *gp = 100.f * max3f(pick->mean);
  // the black picker alone sets shadows_range, the white one alone the
  // range above the shadows already set, the auto tune both
  float EVmin = *shadows;
  if(black) EVmin = Log2Thres(max3f(pick->min) / (*gp / 100.0f), noise) * k;
  if(white)
  {
    const float EVmax = Log2Thres(max3f(pick->max) / (*gp / 100.0f), noise) * k;
    *range = EVmax - EVmin;
  }
  if(black) *shadows = EVmin;
}

static void _profile_gamma_grey(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _profile_gamma_pick(m, pick, TRUE, FALSE, FALSE);
}

static void _profile_gamma_black(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _profile_gamma_pick(m, pick, FALSE, TRUE, FALSE);
}

static void _profile_gamma_range(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _profile_gamma_pick(m, pick, FALSE, FALSE, TRUE);
}

static void _profile_gamma_auto_tune(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _profile_gamma_pick(m, pick, TRUE, TRUE, TRUE);
}

// ---- negadoctor (negadoctor.c) ----------------------------------------------

// -32 EV
#define NEGADOCTOR_THRESHOLD 2.3283064365386963e-10f

// the settings the pickers chain on (each reads the ones set before it);
// NULL in the first when the module lacks one
typedef struct _nega_t
{
  float *Dmin, *wb_high, *wb_low, *D_max, *offset, *black, *exposure;
} _nega_t;

static gboolean _nega(dt_iop_module_t *m, _nega_t *n)
{
  n->Dmin = _P(float, m, "Dmin");
  n->wb_high = _P(float, m, "wb_high");
  n->wb_low = _P(float, m, "wb_low");
  n->D_max = _P(float, m, "D_max");
  n->offset = _P(float, m, "offset");
  n->black = _P(float, m, "black");
  n->exposure = _P(float, m, "exposure");
  return n->Dmin && n->wb_high && n->wb_low && n->D_max && n->offset && n->black && n->exposure;
}

// apply_auto_Dmin: the film base color, all four channels of the sample
static void _negadoctor_dmin(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _nega_t n;
  if(!_nega(m, &n)) return;
  for(int k = 0; k < 4; k++) n.Dmin[k] = pick->mean[k];
}

// apply_auto_Dmax
static void _negadoctor_dmax(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _nega_t n;
  if(!_nega(m, &n)) return;
  dt_aligned_pixel_t RGB;
  for(int c = 0; c < 3; c++) RGB[c] = log10f(n.Dmin[c] / fmaxf(pick->min[c], NEGADOCTOR_THRESHOLD));
  *n.D_max = v_maxf(RGB);
}

// apply_auto_offset
static void _negadoctor_offset(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _nega_t n;
  if(!_nega(m, &n)) return;
  dt_aligned_pixel_t RGB;
  for(int c = 0; c < 3; c++)
    RGB[c] = log10f(n.Dmin[c] / fmaxf(pick->max[c], NEGADOCTOR_THRESHOLD)) / *n.D_max;
  *n.offset = v_minf(RGB);
}

// apply_auto_WB_low
static void _negadoctor_wb_low(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _nega_t n;
  if(!_nega(m, &n)) return;
  dt_aligned_pixel_t RGB_min;
  for(int c = 0; c < 3; c++)
    RGB_min[c] = log10f(n.Dmin[c] / fmaxf(pick->mean[c], NEGADOCTOR_THRESHOLD)) / *n.D_max;
  const float RGB_v_min = v_minf(RGB_min); // can be negative
  for(int c = 0; c < 3; c++) n.wb_low[c] = RGB_v_min / RGB_min[c];
  n.wb_low[3] = 1.0f;
}

// apply_auto_WB_high
static void _negadoctor_wb_high(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _nega_t n;
  if(!_nega(m, &n)) return;
  dt_aligned_pixel_t RGB_min;
  for(int c = 0; c < 3; c++)
    RGB_min[c] = fabsf(-1.0f / (*n.offset * n.wb_low[c]
                                - log10f(n.Dmin[c] / fmaxf(pick->mean[c], NEGADOCTOR_THRESHOLD)) / *n.D_max));
  const float RGB_v_min = v_minf(RGB_min); // must be positive
  for(int c = 0; c < 3; c++) n.wb_high[c] = RGB_min[c] / RGB_v_min;
  n.wb_high[3] = 1.0f;
}

// apply_auto_black
static void _negadoctor_black(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _nega_t n;
  if(!_nega(m, &n)) return;
  dt_aligned_pixel_t RGB;
  for(int c = 0; c < 3; c++)
  {
    RGB[c] = -log10f(n.Dmin[c] / fmaxf(pick->max[c], NEGADOCTOR_THRESHOLD));
    RGB[c] *= n.wb_high[c] / *n.D_max;
    RGB[c] += n.wb_low[c] * *n.offset * n.wb_high[c];
    RGB[c] = 0.1f - (1.0f - fast_exp10f(RGB[c]));
  }
  *n.black = v_maxf(RGB);
}

// apply_auto_exposure: the setting is the linear factor (darktable's
// slider shows its log2)
static void _negadoctor_exposure(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _nega_t n;
  if(!_nega(m, &n)) return;
  dt_aligned_pixel_t RGB;
  for(int c = 0; c < 3; c++)
  {
    RGB[c] = -log10f(n.Dmin[c] / fmaxf(pick->min[c], NEGADOCTOR_THRESHOLD));
    RGB[c] *= n.wb_high[c] / *n.D_max;
    RGB[c] += n.wb_low[c] * *n.offset;
    RGB[c] = 0.96f / (1.0f - fast_exp10f(RGB[c]) + *n.black);
  }
  *n.exposure = v_minf(RGB);
}

// ---- color zones (colorzones.c) ---------------------------------------------

// a curve node of colorzones and rgbcurve (dt_iop_colorzones_node_t,
// dt_iop_rgbcurve_node_t); both modules keep 20 nodes per curve
typedef struct _curve_node_t
{
  float x, y;
} _curve_node_t;

#define _CURVE_MAXNODES 20
#define _CURVE_MIN_X_DISTANCE 0.0025f

// colorzones' _add_node: sorted insert, refused (nothing added) when the
// node is closer than the minimum distance to a neighbour
static void _colorzones_add_node(_curve_node_t *curve, int *nodes, const float x, const float y)
{
  int selected = -1;
  if(curve[0].x > x)
    selected = 0;
  else
  {
    for(int k = 1; k < *nodes; k++)
    {
      if(curve[k].x > x)
      {
        selected = k;
        break;
      }
    }
  }
  if(selected == -1) selected = *nodes;

  if((selected > 0 && x - curve[selected - 1].x <= _CURVE_MIN_X_DISTANCE)
     || (selected < *nodes && curve[selected].x - x <= _CURVE_MIN_X_DISTANCE))
    return;

  for(int i = *nodes; i > selected; i--)
  {
    curve[i].x = curve[i - 1].x;
    curve[i].y = curve[i - 1].y;
  }
  curve[selected].x = x;
  curve[selected].y = y;
  (*nodes)++;
}

// the "select by" channel's coordinate of a sample, 0..1
static float _colorzones_x(const int channel, const dt_aligned_pixel_t v)
{
  if(channel == 0) return v[0] / 100.f;
  if(channel == 1) return v[1] / (128.f * M_SQRT2_F);
  if(channel == 2) return v[2];
  return 0.f;
}

// color_picker_apply of "create curve": resets the curve of the channel tab
// that is open (g->channel) and puts 5 nodes on it from the area: feather,
// min, mean, max, feather, at the "select by" channel's values. The tab is
// the one gui_init takes from conf plugins/darkroom/colorzones/gui_channel
// (lightness when the key isn't there; darktable only writes it on closing
// the module, so a tab picked in an open window is not seen). No modifier
// key is held headless: a flat curve. "show color" writes nothing
static void _colorzones_create_curve(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  int *channel = _P(int, m, "channel");
  _curve_node_t *all = _P(_curve_node_t, m, "curve");
  int *num_nodes = _P(int, m, "curve_num_nodes");
  int *type = _P(int, m, "curve_type");
  const _curve_node_t *all_d = m->get_p(m->default_params, "curve");
  const int *num_nodes_d = m->get_p(m->default_params, "curve_num_nodes");
  const int *type_d = m->get_p(m->default_params, "curve_type");
  if(!channel || !all || !num_nodes || !type || !all_d || !num_nodes_d || !type_d) return;

  const int ch_val = *channel;
  const int ch_curve = dt_conf_key_exists("plugins/darkroom/colorzones/gui_channel")
                         ? CLAMP(dt_conf_get_int("plugins/darkroom/colorzones/gui_channel"), 0, 2)
                         : 0;
  _curve_node_t *curve = all + ch_curve * _CURVE_MAXNODES;

  // reset current curve
  num_nodes[ch_curve] = num_nodes_d[ch_curve];
  type[ch_curve] = type_d[ch_curve];
  for(int k = 0; k < _CURVE_MAXNODES; k++)
    curve[k] = all_d[ch_curve * _CURVE_MAXNODES + k];

  // flat: no ctrl (positive) or shift (negative)
  const int picker_set_upper_lower = 0;

  // now add 5 nodes: feather, min, center, max, feather
  const float feather = 0.02f;
  const float increment = 0.1f * picker_set_upper_lower;
  float x = _colorzones_x(ch_val, pick->min) - feather;
  if(x > 0.f && x < 1.f) _colorzones_add_node(curve, &num_nodes[ch_curve], x, .5f);

  x = _colorzones_x(ch_val, pick->min);
  if(x > 0.f && x < 1.f) _colorzones_add_node(curve, &num_nodes[ch_curve], x, .5f + increment);

  x = _colorzones_x(ch_val, pick->mean);
  if(x > 0.f && x < 1.f) _colorzones_add_node(curve, &num_nodes[ch_curve], x, .5f + 2.f * increment);

  x = _colorzones_x(ch_val, pick->max);
  if(x > 0.f && x < 1.f) _colorzones_add_node(curve, &num_nodes[ch_curve], x, .5f + increment);

  x = _colorzones_x(ch_val, pick->max) + feather;
  if(x > 0.f && x < 1.f) _colorzones_add_node(curve, &num_nodes[ch_curve], x, .5f);
}

// ---- rgb curve (rgbcurve.c) -------------------------------------------------

// rgbcurve's _add_node: sorted insert, no distance check
static int _rgbcurve_add_node(_curve_node_t *curve, int *nodes, const float x, const float y)
{
  int selected = -1;
  if(curve[0].x > x)
    selected = 0;
  else
  {
    for(int k = 1; k < *nodes; k++)
    {
      if(curve[k].x > x)
      {
        selected = k;
        break;
      }
    }
  }
  if(selected == -1) selected = *nodes;
  for(int i = *nodes; i > selected; i--)
  {
    curve[i].x = curve[i - 1].x;
    curve[i].y = curve[i - 1].y;
  }
  curve[selected].x = x;
  curve[selected].y = y;
  (*nodes)++;
  return selected;
}

// _add_node_from_picker
static void _rgbcurve_add_node_from_picker(dt_iop_module_t *m, _curve_node_t *curve, int *nodes,
                                           const dt_aligned_pixel_t in, const float increment, const int ch,
                                           const dt_iop_order_iccprofile_info_t *const work_profile)
{
  const int *autoscale = _P(int, m, "curve_autoscale");
  const int *compensate = _P(int, m, "compensate_middle_grey");
  float val = 0.f;

  // DT_S_SCALE_AUTOMATIC_RGB is 0
  if(*autoscale == 0)
    val = (work_profile)
            ? dt_ioppr_get_rgb_matrix_luminance(in, work_profile->matrix_in, work_profile->lut_in,
                                                work_profile->unbounded_coeffs_in, work_profile->lutsize,
                                                work_profile->nonlinearlut)
            : dt_camera_rgb_luminance(in);
  else
    val = in[ch];

  float x, y;
  if(*compensate && work_profile)
    y = x = dt_ioppr_compensate_middle_grey(val, work_profile);
  else
    y = x = val;

  x = CLIP(x - increment);
  y = CLIP(y + increment);

  _rgbcurve_add_node(curve, nodes, x, y);
}

// color_picker_apply of "create curve": resets the curve of the channel tab
// that is open (g->channel) and puts nodes on it from the area: min, mean,
// max (the mean's pair shifted by the modifier key), and a middle node when
// that makes 5. The tab is R: gui_init, gui_reset and a mode change set it,
// and nothing headless picks another. No modifier key is held headless: a
// flat curve. "show color" writes nothing
static void _rgbcurve_create_curve(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  _curve_node_t *all = _P(_curve_node_t, m, "curve_nodes");
  int *num_nodes = _P(int, m, "curve_num_nodes");
  int *type = _P(int, m, "curve_type");
  const _curve_node_t *all_d = m->get_p(m->default_params, "curve_nodes");
  const int *num_nodes_d = m->get_p(m->default_params, "curve_num_nodes");
  const int *type_d = m->get_p(m->default_params, "curve_type");
  if(!all || !num_nodes || !type || !all_d || !num_nodes_d || !type_d || !_P(int, m, "curve_autoscale")
     || !_P(int, m, "compensate_middle_grey"))
    return;

  const int ch = 0; // DT_IOP_RGBCURVE_R
  _curve_node_t *curve = all + ch * _CURVE_MAXNODES;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(pipe);

  // reset current curve
  num_nodes[ch] = num_nodes_d[ch];
  type[ch] = type_d[ch];
  for(int k = 0; k < _CURVE_MAXNODES; k++)
    curve[k] = all_d[ch * _CURVE_MAXNODES + k];

  // flat: no ctrl (positive) or shift (negative)
  const int picker_set_upper_lower = 0;

  // now add 4 nodes: min, avg, center, max
  const float increment = 0.05f * picker_set_upper_lower;

  _rgbcurve_add_node_from_picker(m, curve, &num_nodes[ch], pick->min, 0.f, ch, work_profile);
  _rgbcurve_add_node_from_picker(m, curve, &num_nodes[ch], pick->mean, increment, ch, work_profile);
  _rgbcurve_add_node_from_picker(m, curve, &num_nodes[ch], pick->max, 0.f, ch, work_profile);

  if(num_nodes[ch] == 5)
    _rgbcurve_add_node(curve, &num_nodes[ch], curve[1].x - increment + (curve[3].x - curve[1].x) / 2.f,
                       curve[1].y + increment + (curve[3].y - curve[1].y) / 2.f);
}

// -----------------------------------------------------------------------------

static const dt_api_picker_t _pickers[] = {
  { "agx", "exposure range/auto tune levels", IOP_CS_NONE, TRUE, _agx_auto_tune },
  { "agx", "white relative exposure", IOP_CS_NONE, TRUE, _agx_white },
  { "agx", "black relative exposure", IOP_CS_NONE, TRUE, _agx_black },
  { "agx", "curve/pivot relative exposure", IOP_CS_NONE, TRUE, _agx_pivot },
  { "agx", "curve/pivot target output", IOP_CS_NONE, TRUE, _agx_pivot_target },
  { "basicadj", "middle gray", IOP_CS_NONE, FALSE, _basicadj },
  { "borders", "pickers/border color", IOP_CS_NONE, FALSE, _borders_border },
  { "borders", "pickers/frame line color", IOP_CS_NONE, FALSE, _borders_frame },
  { "channelmixerrgb", "picker", IOP_CS_NONE, FALSE, _channelmixerrgb, _channelmixerrgb_unavailable },
  { "colorbalancergb", "offset/hue", IOP_CS_NONE, FALSE, _cbrgb_offset },
  { "colorbalancergb", "lift/hue", IOP_CS_NONE, FALSE, _cbrgb_lift },
  { "colorbalancergb", "power/hue", IOP_CS_NONE, FALSE, _cbrgb_power },
  { "colorbalancergb", "gain/hue", IOP_CS_NONE, FALSE, _cbrgb_gain },
  { "colorbalancergb", "white fulcrum", IOP_CS_NONE, FALSE, _cbrgb_white_fulcrum },
  { "colorbalancergb", "contrast gray fulcrum", IOP_CS_NONE, FALSE, _cbrgb_grey_fulcrum },
  { "colorequal", "white level", IOP_CS_NONE, FALSE, _colorequal_white_level },
  { "colorharmonizer", "anchor hue", IOP_CS_NONE, FALSE, _colorharmonizer_anchor, _colorharmonizer_unavailable },
  { "colorize", "hue", IOP_CS_NONE, FALSE, _colorize },
  { "colorzones", "pickers/create curve", IOP_CS_LCH, FALSE, _colorzones_create_curve },
  { "exposure", "exposure", IOP_CS_NONE, FALSE, _exposure },
  { "filmic", "auto tune levels", IOP_CS_NONE, TRUE, _filmic_v1_auto_tune },
  { "filmic", "middle gray luminance", IOP_CS_NONE, TRUE, _filmic_v1_grey },
  { "filmic", "white relative exposure", IOP_CS_NONE, TRUE, _filmic_v1_white },
  { "filmic", "black relative exposure", IOP_CS_NONE, TRUE, _filmic_v1_black },
  { "filmicrgb", "auto tune levels", IOP_CS_NONE, TRUE, _filmic_auto_tune },
  { "filmicrgb", "middle gray luminance", IOP_CS_NONE, TRUE, _filmic_grey, _filmic_grey_unavailable },
  { "filmicrgb", "white relative exposure", IOP_CS_NONE, TRUE, _filmic_white },
  { "filmicrgb", "black relative exposure", IOP_CS_NONE, TRUE, _filmic_black },
  { "graduatednd", "hue", IOP_CS_NONE, FALSE, _graduatednd },
  { "levels", "pickers/black", IOP_CS_NONE, FALSE, _levels_black },
  { "levels", "pickers/gray", IOP_CS_NONE, FALSE, _levels_gray },
  { "levels", "pickers/white", IOP_CS_NONE, FALSE, _levels_white },
  { "monochrome", "highlights", IOP_CS_NONE, FALSE, _monochrome },
  { "negadoctor", "pickers/film material", IOP_CS_NONE, FALSE, _negadoctor_dmin },
  { "negadoctor", "D max", IOP_CS_NONE, FALSE, _negadoctor_dmax },
  { "negadoctor", "scan exposure bias", IOP_CS_NONE, FALSE, _negadoctor_offset },
  { "negadoctor", "pickers/shadows", IOP_CS_NONE, FALSE, _negadoctor_wb_low },
  { "negadoctor", "pickers/illuminant", IOP_CS_NONE, FALSE, _negadoctor_wb_high },
  { "negadoctor", "paper black (density correction)", IOP_CS_NONE, FALSE, _negadoctor_black },
  { "negadoctor", "print exposure adjustment", IOP_CS_NONE, FALSE, _negadoctor_exposure },
  { "profile_gamma", "auto tune levels", IOP_CS_NONE, FALSE, _profile_gamma_auto_tune },
  { "profile_gamma", "middle gray luma", IOP_CS_NONE, FALSE, _profile_gamma_grey },
  { "profile_gamma", "dynamic range", IOP_CS_NONE, FALSE, _profile_gamma_range },
  { "profile_gamma", "black relative exposure", IOP_CS_NONE, FALSE, _profile_gamma_black },
  { "rgbcurve", "pickers/create curve", IOP_CS_NONE, FALSE, _rgbcurve_create_curve },
  { "rgblevels", "pickers/black", IOP_CS_NONE, FALSE, _rgblevels_black },
  { "rgblevels", "pickers/gray", IOP_CS_NONE, FALSE, _rgblevels_gray },
  { "rgblevels", "pickers/white", IOP_CS_NONE, FALSE, _rgblevels_white },
  { "splittoning", "shadows/hue", IOP_CS_NONE, FALSE, _splittoning_shadows },
  { "splittoning", "highlights/hue", IOP_CS_NONE, FALSE, _splittoning_highlights },
  { "watermark", "pick color", IOP_CS_NONE, FALSE, _watermark },
};

const dt_api_picker_t *dt_api_picker_nth(const char *op, const int n)
{
  int k = 0;
  for(size_t i = 0; i < G_N_ELEMENTS(_pickers); i++)
    if(!g_strcmp0(op, _pickers[i].op) && k++ == n) return &_pickers[i];
  return NULL;
}

const dt_api_picker_t *dt_api_picker_find(const char *op, const char *control)
{
  for(size_t i = 0; i < G_N_ELEMENTS(_pickers); i++)
    if(!g_strcmp0(op, _pickers[i].op) && !g_strcmp0(control, _pickers[i].control)) return &_pickers[i];
  return NULL;
}
