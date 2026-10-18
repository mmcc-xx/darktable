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
#include "develop/pixelpipe_hb.h"

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
// setting is left as it was); "pivot target output" isn't ported (it needs
// agx's tone curve)
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

// -----------------------------------------------------------------------------

static const dt_api_picker_t _pickers[] = {
  { "agx", "exposure range/auto tune levels", IOP_CS_NONE, TRUE, _agx_auto_tune },
  { "agx", "white relative exposure", IOP_CS_NONE, TRUE, _agx_white },
  { "agx", "black relative exposure", IOP_CS_NONE, TRUE, _agx_black },
  { "agx", "curve/pivot relative exposure", IOP_CS_NONE, TRUE, _agx_pivot },
  { "channelmixerrgb", "picker", IOP_CS_NONE, FALSE, _channelmixerrgb, _channelmixerrgb_unavailable },
  { "exposure", "exposure", IOP_CS_NONE, FALSE, _exposure },
  { "filmicrgb", "auto tune levels", IOP_CS_NONE, TRUE, _filmic_auto_tune },
  { "filmicrgb", "middle gray luminance", IOP_CS_NONE, TRUE, _filmic_grey, _filmic_grey_unavailable },
  { "filmicrgb", "white relative exposure", IOP_CS_NONE, TRUE, _filmic_white },
  { "filmicrgb", "black relative exposure", IOP_CS_NONE, TRUE, _filmic_black },
  { "monochrome", "highlights", IOP_CS_NONE, FALSE, _monochrome },
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
