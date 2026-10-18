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

// the settings a module's gui_changed adjusts alongside a change, for a
// session without darktable's window: there the module has no controls and
// its gui_changed can't run. each function mirrors the branches of one
// module's gui_changed that write its params (darktable-api/
// gui_changed_inventory.json lists them), keyed by the setting's name
// instead of its control. darktable-api/also_changed_check.py compares them
// with what darktable's window does

#include "api/also_changed.h"
#include "api/compat.h"
#include "common/chromatic_adaptation.h"
#include "common/color_harmony.h"
#include "common/color_ryb.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/illuminants.h"
#include "common/image.h"
#include "develop/develop.h"
#include "develop/imageop.h"

#include <math.h>
#include <string.h>

// a setting by name; NULL when this darktable's module doesn't have it
#define _P(T, m, name) ((T *)(m)->get_p((m)->params, (name)))
// the same setting before the change
#define _OLD(T, m, old, name) (*(const T *)(m)->get_p((old), (name)))

static gboolean _is(const char *field, const char *name)
{
  return !g_strcmp0(field, name);
}

// the value a bauhaus slider keeps when set outside a gui update
// (_slider_normalized_to_value): within its hard range, rounded to its
// display precision
static float _slider_value(const float v, const float hard_min, const float hard_max, const int digits,
                           const float factor)
{
  const float base = powf(10.0f, digits) * factor;
  return roundf(base * CLAMP(v, hard_min, hard_max)) / base;
}

// ---- exposure (exposure.c) --------------------------------------------------

enum { _EXPOSURE_MODE_MANUAL = 0, _EXPOSURE_MODE_DEFLICKER = 1 };

// _exposure_compensation_ev: the EXIF biases the module adds to the slider
static float _exposure_compensation(const dt_iop_module_t *m)
{
  const dt_image_t *img = &m->dev->image_storage;
  const gboolean *bias_on = _P(gboolean, m, "compensate_exposure_bias");
  const gboolean *hl_on = _P(gboolean, m, "compensate_hilite_pres");
  float comp = 0.0f;
  if(bias_on && *bias_on && img->exif_exposure_bias != DT_EXIF_TAG_UNINITIALIZED)
    comp -= CLAMP(img->exif_exposure_bias, -5.0f, 5.0f);
#ifdef HAVE_EXIF_HIGHLIGHT_PRESERVATION
  if(hl_on && *hl_on && img->exif_highlight_preservation > 0.0f
     && img->exif_highlight_preservation != DT_EXIF_TAG_UNINITIALIZED)
    comp += CLAMP(img->exif_highlight_preservation, -1.0f, 4.0f);
#else
  (void)hl_on;
#endif
  return comp;
}

static void _exposure_set_black(dt_iop_module_t *m, float black);

// _exposure_set_white: the slider value that maps white to 1
static void _exposure_set_white(dt_iop_module_t *m, const float white)
{
  float *exposure = _P(float, m, "exposure");
  float *black = _P(float, m, "black");
  const float e = -log2f(fmaxf(1e-20f, white)) - _exposure_compensation(m);
  if(*exposure == e) return;
  *exposure = e;
  if(*black >= white) _exposure_set_black(m, white - 0.01f);
}

static void _exposure_set_black(dt_iop_module_t *m, const float black)
{
  float *b = _P(float, m, "black");
  if(*b == black) return;
  *b = black;
  const float white = exp2f(-(*_P(float, m, "exposure") + _exposure_compensation(m)));
  if(*b >= white) _exposure_set_white(m, *b + 0.01f);
}

static void _exposure(dt_iop_module_t *m, const char *field, const void *old)
{
  int *mode = _P(int, m, "mode");
  float *exposure = _P(float, m, "exposure");
  float *black = _P(float, m, "black");
  if(!mode || !exposure || !black) return;
  const dt_image_t *img = &m->dev->image_storage;
  // deflicker needs raw sensor data: other images are forced to manual. the
  // buffer description is known once the image was loaded
  if(_is(field, "mode") && *mode == _EXPOSURE_MODE_DEFLICKER && img->buf_dsc.channels
     && (!dt_image_is_raw(img) || img->buf_dsc.channels != 1 || img->buf_dsc.datatype != TYPE_UINT16))
    *mode = _EXPOSURE_MODE_MANUAL;
  // black and the white point must not cross, in manual mode only
  if(*mode != _EXPOSURE_MODE_MANUAL) return;
  const float white = exp2f(-(*exposure + _exposure_compensation(m)));
  if(_is(field, "black"))
  {
    if(*black >= white) _exposure_set_white(m, *black + 0.01f);
  }
  else if(_is(field, "mode") || _is(field, "exposure") || _is(field, "compensate_exposure_bias")
          || _is(field, "compensate_hilite_pres"))
  {
    if(*black >= white) _exposure_set_black(m, white - 0.01f);
  }
}

// ---- agx (agx.c) ------------------------------------------------------------

// _update_pivot_x: the pivot keeps its place in EV when the range changes
static void _agx_update_pivot_x(dt_iop_module_t *m, const float old_black, const float old_white)
{
  const float black = *_P(float, m, "range_black_relative_ev");
  const float white = *_P(float, m, "range_white_relative_ev");
  float *pivot_x = _P(float, m, "curve_pivot_x");
  const float pivot_ev = old_black + *pivot_x * (old_white - old_black);
  *pivot_x = (CLAMPF(pivot_ev, black, white) - black) / (white - black);
}

static void _agx(dt_iop_module_t *m, const char *field, const void *old)
{
  float *black = _P(float, m, "range_black_relative_ev");
  float *white = _P(float, m, "range_white_relative_ev");
  if(!black || !white || !_P(float, m, "curve_pivot_x")) return;
  if(_is(field, "range_black_relative_ev"))
    _agx_update_pivot_x(m, _OLD(float, m, old, "range_black_relative_ev"), *white);
  else if(_is(field, "range_white_relative_ev"))
    _agx_update_pivot_x(m, *black, _OLD(float, m, old, "range_white_relative_ev"));
  else if(_is(field, "dynamic_range_scaling"))
  {
    const float prev = _OLD(float, m, old, "dynamic_range_scaling");
    const float ratio = (*_P(float, m, "dynamic_range_scaling") - prev) / (prev + 1.f);
    const float old_black = *black, old_white = *white;
    *black = old_black * (1.f + ratio);
    *white = old_white * (1.f + ratio);
    _agx_update_pivot_x(m, old_black, old_white);
  }
  // _adjust_pivot / _calculate_auto_gamma, for any change: the gamma that
  // puts the pivot at its output level; the slider holds it within 0.01-100
  const gboolean *auto_gamma = _P(gboolean, m, "auto_gamma");
  float *gamma = _P(float, m, "curve_gamma");
  const float *pivot_y = _P(float, m, "curve_pivot_y_linear_output");
  if(auto_gamma && *auto_gamma && gamma && pivot_y)
  {
    const float eps = 1e-6f;
    const float pivot_x = CLAMPF(*_P(float, m, "curve_pivot_x"), eps, 1.f - eps);
    const float g = pivot_x > 0.f && *pivot_y > 0.f ? log2f(*pivot_y) / log2f(pivot_x) : *gamma;
    // set through its slider, which keeps two decimals
    *gamma = roundf(CLAMPF(g, 0.01f, 100.f) * 100.f) / 100.f;
  }
}

// ---- color calibration (channelmixerrgb.c) ----------------------------------

// _get_white_balance_coeff: the camera white balance against D65 the
// module's "as shot in camera" reads
static void _cm_white_balance(const dt_iop_module_t *m, dt_aligned_pixel_t wb)
{
  const dt_dev_chroma_t *chr = &m->dev->chroma;
  for_four_channels(k) wb[k] = 1.0f;
  if(!dt_image_is_matrix_correction_supported(&m->dev->image_storage)) return;
  const gboolean d65 = chr->late_correction ? dt_dev_equal_chroma(chr->wb_coeffs, chr->as_shot)
                                            : dt_dev_equal_chroma(chr->wb_coeffs, chr->D65coeffs);
  if(d65) return;
  const gboolean valid = chr->D65coeffs[0] > 0.0 && chr->D65coeffs[1] > 0.0 && chr->D65coeffs[2] > 0.0;
  const gboolean changed = chr->wb_coeffs[0] > 1.0f || chr->wb_coeffs[1] > 1.0f || chr->wb_coeffs[2] > 1.0f;
  if(valid && changed)
    for_four_channels(k) wb[k] = (float)chr->D65coeffs[k] / chr->wb_coeffs[k];
}

// _check_if_close_to_daylight, the parts gui_changed uses: the correlated
// color temperature of x, y, and CAT16 for the adaptation
static float _cm_cct(const float x, const float y)
{
  float t = xy_to_CCT(x, y);
  // xy_to_CCT is valid only in 3000 - 25000 K
  if(t < 3000.f && t > 1667.f) t = CCT_reverse_lookup(x, y);
  return t;
}

static void _cm_from_camera(dt_iop_module_t *m, float *x, float *y, float *temperature, int *adaptation)
{
  dt_aligned_pixel_t wb;
  _cm_white_balance(m, wb);
  find_temperature_from_raw_coeffs(&m->dev->image_storage, wb, x, y);
  *temperature = _cm_cct(*x, *y);
  if(adaptation) *adaptation = DT_ADAPTATION_CAT16;
}

static void _channelmixerrgb(dt_iop_module_t *m, const char *field, const void *old)
{
  int *illuminant = _P(int, m, "illuminant");
  float *x = _P(float, m, "x"), *y = _P(float, m, "y"), *temperature = _P(float, m, "temperature");
  const int *ifluo = _P(int, m, "illum_fluo"), *iled = _P(int, m, "illum_led");
  int *adaptation = _P(int, m, "adaptation");
  if(!illuminant || !x || !y || !temperature || !ifluo || !iled) return;
  if(_is(field, "illuminant"))
  {
    // leaving "as shot in camera": start from the camera's white balance
    if(_OLD(int, m, old, "illuminant") == DT_ILLUMINANT_CAMERA)
      _cm_from_camera(m, x, y, temperature, adaptation);
    // to daylight or black body the window restores the temperature last
    // set in that mode, which only its controls remember: here the setting
    // keeps the value it had
    if(*illuminant == DT_ILLUMINANT_D || *illuminant == DT_ILLUMINANT_BB)
      *temperature = _OLD(float, m, old, "temperature");
    if(*illuminant == DT_ILLUMINANT_CAMERA)
      _cm_from_camera(m, x, y, temperature, adaptation);
  }
  if(_is(field, "illuminant") || _is(field, "illum_fluo") || _is(field, "illum_led")
     || _is(field, "temperature"))
  {
    // the chromaticity follows the illuminant it is defined by
    if(*illuminant != DT_ILLUMINANT_CUSTOM && *illuminant != DT_ILLUMINANT_CAMERA)
      illuminant_to_xy(*illuminant, NULL, NULL, x, y, *temperature, *ifluo, *iled);
    // modes without a temperature show the closest one
    if(*illuminant != DT_ILLUMINANT_D && *illuminant != DT_ILLUMINANT_BB
       && *illuminant != DT_ILLUMINANT_CAMERA)
      *temperature = _cm_cct(*x, *y);
  }
}

// ---- profile gamma (profile_gamma.c) ----------------------------------------

static void _profile_gamma(dt_iop_module_t *m, const char *field, const void *old)
{
  float *dynamic_range = _P(float, m, "dynamic_range");
  float *shadows_range = _P(float, m, "shadows_range");
  const float *security = _P(float, m, "security_factor");
  if(!dynamic_range || !shadows_range || !security) return;
  // the safety factor widens or narrows the range around the black point.
  // the sliders are set inside a gui update, so the values are kept as they are
  if(_is(field, "security_factor"))
  {
    const float prev = _OLD(float, m, old, "security_factor");
    const float ratio = (*security - prev) / (prev + 100.0f);
    float ev_min = *shadows_range;
    ev_min = ev_min + ratio * ev_min;
    float ev_max = *dynamic_range + *shadows_range;
    ev_max = ev_max + ratio * ev_max;
    *dynamic_range = ev_max - ev_min;
    *shadows_range = ev_min;
  }
}

// ---- white balance (temperature.c) -----------------------------------------

enum { _TEMP_AS_SHOT = 0, _TEMP_USER = 2, _TEMP_D65 = 3, _TEMP_D65_LATE = 4 };

static void _temperature(dt_iop_module_t *m, const char *field, const void *old)
{
  int *preset = _P(int, m, "preset");
  const float *coeffs = _P(float, m, "red");
  if(!preset || !coeffs) return;
  // _update_preset: moving a coefficient makes the preset "user defined",
  // and the late correction only belongs to the as shot to reference preset.
  // the presets combobox is set to user first, whose callback copies the
  // cached coefficients, which are the ones just set
  if(_is(field, "red") || _is(field, "green") || _is(field, "blue") || _is(field, "various"))
  {
    *preset = _TEMP_USER;
    m->dev->chroma.late_correction = FALSE;
  }
  // the window then refreshes the module (gui_update), which names the
  // coefficients that are the camera's as shot (the late correction preset
  // keeps its name) or its D65 ones. the first three settings are compared
  const dt_dev_chroma_t *chr = &m->dev->chroma;
  if(dt_dev_equal_chroma(coeffs, chr->as_shot))
  {
    if(*preset != _TEMP_D65_LATE) *preset = _TEMP_AS_SHOT;
  }
  else if(dt_dev_equal_chroma(coeffs, chr->D65coeffs))
    *preset = _TEMP_D65;
}

// ---- spektrafilm (spektrafilm.c) --------------------------------------------

static void _spektrafilm(dt_iop_module_t *m, const char *field, const void *old)
{
  // _stamp_lut_hash, first in gui_changed, records the hashes of the loaded
  // data pack, which only the window loads: here they stay as they were
  const gboolean *auto_exposure = _P(gboolean, m, "print_auto_exposure");
  float *exposure = _P(float, m, "print_exposure_ev");
  if(!auto_exposure || !exposure) return;
  // switching the automatic print exposure on starts its fine tuning from zero
  if(_is(field, "print_auto_exposure") && !_OLD(gboolean, m, old, "print_auto_exposure") && *auto_exposure)
    *exposure = 0.0f;
}

// ---- local contrast (bilat.c) -----------------------------------------------

enum { _BILAT_MODE_BILATERAL = 0, _BILAT_MODE_LOCAL_LAPLACIAN = 1 };

static void _bilat(dt_iop_module_t *m, const char *field, const void *old)
{
  int *mode = _P(int, m, "mode");
  float *sigma_r = _P(float, m, "sigma_r");
  float *sigma_s = _P(float, m, "sigma_s");
  if(!mode || !sigma_r || !sigma_s) return;
  // the sliders of the mode in use set it again to itself: only the midtone
  // slider can change the mode (to local laplacian), and a mode change copies
  // the sigmas from the sliders of the new mode. gui_update has set these:
  // the sliders of the mode in use from the settings, those of the other
  // mode to fixed values (highlights and shadows 0.5, contrast 20 and
  // coarseness 50), and the settings follow the sliders
  float highlights = 0.5f, shadows = 0.5f;
  if(_is(field, "midtone") && *mode != _BILAT_MODE_LOCAL_LAPLACIAN)
    *mode = _BILAT_MODE_LOCAL_LAPLACIAN;
  else if(_is(field, "mode"))
  {
    highlights = *sigma_r;
    shadows = *sigma_s;
  }
  else
    return;
  // a slider keeps its value within its range, rounded: highlights and
  // shadows 0 - 2 shown in percent (steps of 0.01), contrast 1 - 100 and
  // coarseness 3 - 100 in whole numbers
  if(*mode == _BILAT_MODE_LOCAL_LAPLACIAN)
  {
    *sigma_r = _slider_value(highlights, 0.0f, 2.0f, 0, 100.0f);
    *sigma_s = _slider_value(shadows, 0.0f, 2.0f, 0, 100.0f);
  }
  else
  {
    *sigma_r = _slider_value(*sigma_r, 1.0f, 100.0f, 0, 1.0f);
    *sigma_s = _slider_value(*sigma_s, 3.0f, 100.0f, 0, 1.0f);
  }
}

// ---- haze removal (hazeremoval.c) ---------------------------------------------

static void _hazeremoval(dt_iop_module_t *m, const char *field, const void *old)
{
  gboolean *compatibility = _P(gboolean, m, "compatibility_mode");
  gboolean *adaptive = _P(gboolean, m, "adaptive");
  if(!compatibility || !adaptive) return;
  // touching a slider leaves the compatibility mode of old edits
  if(_is(field, "strength") || _is(field, "distance"))
  {
    *compatibility = FALSE;
    *adaptive = TRUE;
  }
}

// ---- color mapping (colormapping.c) -------------------------------------------

enum { _COLORMAPPING_HISTN = 1 << 11, _COLORMAPPING_MAXN = 5, _COLORMAPPING_NEUTRAL = 0 };

static void _colormapping(dt_iop_module_t *m, const char *field, const void *old)
{
  float *source_ihist = _P(float, m, "source_ihist");
  float *source_mean = _P(float, m, "source_mean");
  float *source_var = _P(float, m, "source_var");
  float *source_weight = _P(float, m, "source_weight");
  int *target_hist = _P(int, m, "target_hist");
  float *target_mean = _P(float, m, "target_mean");
  float *target_var = _P(float, m, "target_var");
  float *target_weight = _P(float, m, "target_weight");
  int *flag = _P(int, m, "flag");
  if(!source_ihist || !source_mean || !source_var || !source_weight || !target_hist || !target_mean
     || !target_var || !target_weight || !flag)
    return;
  // only changing the number of clusters resets the source and target
  if(_is(field, "n"))
  {
    memset(source_ihist, 0, sizeof(float) * _COLORMAPPING_HISTN);
    memset(source_mean, 0, sizeof(float) * _COLORMAPPING_MAXN * 2);
    memset(source_var, 0, sizeof(float) * _COLORMAPPING_MAXN * 2);
    memset(source_weight, 0, sizeof(float) * _COLORMAPPING_MAXN);
    memset(target_hist, 0, sizeof(int) * _COLORMAPPING_HISTN);
    memset(target_mean, 0, sizeof(float) * _COLORMAPPING_MAXN * 2);
    memset(target_var, 0, sizeof(float) * _COLORMAPPING_MAXN * 2);
    memset(target_weight, 0, sizeof(float) * _COLORMAPPING_MAXN);
    *flag = _COLORMAPPING_NEUTRAL;
  }
}

// ---- color zones (colorzones.c) -----------------------------------------------

enum { _COLORZONES_CHANNELS = 3, _COLORZONES_MAXNODES = 20, _COLORZONES_h = 2, _COLORZONES_MODE_SMOOTH = 0 };
// curve_tools.h
enum { _CATMULL_ROM = 1 };

typedef struct { float x, y; } _colorzones_node_t;

// _reset_nodes
static void _colorzones_reset_nodes(_colorzones_node_t *curve, const int num_nodes, const gboolean touch_edges)
{
  for(int k = 0; k < num_nodes; k++)
  {
    if(touch_edges)
      curve[k].x = (float)k / (float)(num_nodes - 1);
    else
      curve[k].x = ((float)k + 0.5f) / (float)num_nodes;
    curve[k].y = 0.5f;
  }
}

static void _colorzones(dt_iop_module_t *m, const char *field, const void *old)
{
  int *channel = _P(int, m, "channel");
  _colorzones_node_t *curve = _P(_colorzones_node_t, m, "curve");
  int *num_nodes = _P(int, m, "curve_num_nodes");
  int *type = _P(int, m, "curve_type");
  float *strength = _P(float, m, "strength");
  int *mode = _P(int, m, "mode");
  if(!channel || !curve || !num_nodes || !type || !strength || !mode) return;
  // _reset_parameters: selecting by another channel starts all curves over
  // (the channel and spline version stay)
  if(_is(field, "channel"))
  {
    for(int ch = 0; ch < _COLORZONES_CHANNELS; ch++)
    {
      num_nodes[ch] = 2;
      type[ch] = _CATMULL_ROM;
      _colorzones_reset_nodes(curve + ch * _COLORZONES_MAXNODES, num_nodes[ch], *channel != _COLORZONES_h);
    }
    *strength = 0.0f;
    *mode = _COLORZONES_MODE_SMOOTH;
  }
}

// ---- borders (borders.c) -------------------------------------------------------

static const float _borders_aspect_ratios[]
  = { 0.0f, 3.0f, 95.0f / 33.0f, 2.39f, 2.0f, 16.0f / 9.0f, 5.0f / 3.0f, 14.0f / 8.5f, PHI, 16.0f / 10.0f,
      3.0f / 2.0f, 297.0f / 210.0f, M_SQRT2, 7.0f / 5.0f, 4.0f / 3.0f, 11.0f / 8.5f, 14.0f / 11.0f,
      5.0f / 4.0f, 1.0f, -1.0f };
static const float _borders_pos_ratios[] = { 0.5f, 1.0f / 3.0f, 3.0f / 8.0f, 5.0f / 8.0f, 2.0f / 3.0f };

// the first preset within 0.01 of the value, if there is one: gui_changed
// sets the presets combobox to it, and its callback (_aspect_changed,
// _position_h_changed, _position_v_changed) stores the preset's exact value
static void _borders_snap(float *value, const float *ratios, const size_t count)
{
  size_t k;
  for(k = 0; k < count; k++)
    if(fabsf(*value - ratios[k]) < 0.01f) break;
  if(k < count) *value = ratios[k];
}

static void _borders(dt_iop_module_t *m, const char *field, const void *old)
{
  float *aspect = _P(float, m, "aspect");
  float *pos_h = _P(float, m, "pos_h");
  float *pos_v = _P(float, m, "pos_v");
  if(!aspect || !pos_h || !pos_v) return;
  if(_is(field, "aspect")) _borders_snap(aspect, _borders_aspect_ratios, G_N_ELEMENTS(_borders_aspect_ratios));
  if(_is(field, "pos_h")) _borders_snap(pos_h, _borders_pos_ratios, G_N_ELEMENTS(_borders_pos_ratios));
  if(_is(field, "pos_v")) _borders_snap(pos_v, _borders_pos_ratios, G_N_ELEMENTS(_borders_pos_ratios));
}

// ---- negadoctor (negadoctor.c) ---------------------------------------------

enum { _NEGADOCTOR_FILMSTOCK_NB = 0 };

static void _negadoctor(dt_iop_module_t *m, const char *field, const void *old)
{
  const int *stock = _P(int, m, "film_stock");
  float *dmin = _P(float, m, "Dmin");
  if(!stock || !dmin) return;
  // black and white film has one substrate color: the red one is copied to
  // green and blue through their sliders, which keep 4 digits of a percentage.
  // the exposure branch only converts the slider's EV (_own_units, server.c)
  if(_is(field, "Dmin[0]") && *stock == _NEGADOCTOR_FILMSTOCK_NB)
    dmin[1] = dmin[2] = roundf(dmin[0] * 1e6f) / 1e6f;
}

// ---- demosaic (demosaic.c) -----------------------------------------------------

enum
{
  _DEMOSAIC_XTRANS = 1024,
  _DEMOSAIC_DUAL = 2048,
  _DEMOSAIC_VNG4 = 2,
  _DEMOSAIC_PASSTHROUGH_MONOCHROME = 3,
  _DEMOSAIC_PASSTHROUGH_COLOR = 4,
  _DEMOSAIC_RCD = 5,
  _DEMOSAIC_RCD_DUAL = _DEMOSAIC_DUAL | _DEMOSAIC_RCD,
  _DEMOSAIC_MARKESTEIJN = _DEMOSAIC_XTRANS | 1,
  _DEMOSAIC_MARKESTEIJN_3 = _DEMOSAIC_XTRANS | 2,
  _DEMOSAIC_MARKEST3_DUAL = _DEMOSAIC_DUAL | _DEMOSAIC_MARKESTEIJN_3,
  _DEMOSAIC_MONO = 7
};

static void _demosaic(dt_iop_module_t *m, const char *field, const void *old)
{
  int *method = _P(int, m, "demosaicing_method");
  if(!method) return;
  const dt_image_t *oimg = &m->dev->image_storage;
  // the sensor is known once the image was loaded
  if(!oimg->buf_dsc.channels) return;
  // a method of the wrong sensor type is replaced, whatever was changed
  // (gui_update runs this too). the image's monochrome flag, which the
  // window follows when the method is switched to or from passthrough
  // (monochrome), is not a setting and stays
  const gboolean true_monochrome = dt_image_is_mono_sraw(oimg);
  const gboolean bayer4 = oimg->flags & DT_IMAGE_4BAYER;
  const gboolean bayer = oimg->buf_dsc.filters != 9u && !bayer4 && !true_monochrome;
  const gboolean xtrans = oimg->buf_dsc.filters == 9u;
  int use_method = *method;
  const gboolean xmethod = use_method & _DEMOSAIC_XTRANS;
  const gboolean is_dual = use_method & _DEMOSAIC_DUAL;
  if(bayer && xmethod) use_method = is_dual ? _DEMOSAIC_RCD_DUAL : _DEMOSAIC_RCD;
  if(xtrans && !xmethod) use_method = is_dual ? _DEMOSAIC_MARKEST3_DUAL : _DEMOSAIC_MARKESTEIJN;
  if(true_monochrome) use_method = _DEMOSAIC_MONO;
  const gboolean bayerpassing
    = use_method == _DEMOSAIC_PASSTHROUGH_MONOCHROME || use_method == _DEMOSAIC_PASSTHROUGH_COLOR;
  if(bayer4 && !bayerpassing) use_method = _DEMOSAIC_VNG4;
  *method = use_method;
}

// ---- color harmonizer (colorharmonizer.c) ------------------------------------

enum { _HARMONIZER_CUSTOM = 9, _HARMONIZER_MAX_NODES = 4, _HARMONIZER_LUT_STEPS = 720 };

// the hue conversion tables of the module (init_global), built once: UCS hue
// to the RYB hue of the vectorscope and back
static float _harmonizer_ucs_to_ryb[_HARMONIZER_LUT_STEPS], _harmonizer_ryb_to_ucs[_HARMONIZER_LUT_STEPS];

// _find_max_chroma
static float _harmonizer_max_chroma(const float hue)
{
  const float L_white = Y_to_dt_UCS_L_star(1.0f);
  const float H = hue * DT_2PI_F - M_PI_F;
  const float J = 0.65f;
  float C_lo = 0.f, C_hi = 2.f;
  for(int iter = 0; iter < 16; iter++)
  {
    const float C_mid = (C_lo + C_hi) * 0.5f;
    dt_aligned_pixel_t JCH = { J, C_mid, H, 0.f };
    dt_aligned_pixel_t sRGB;
    dt_UCS_JCH_to_sRGB(JCH, L_white, sRGB);
    if(sRGB[0] >= 0.f && sRGB[1] >= 0.f && sRGB[2] >= 0.f && sRGB[0] <= 1.f && sRGB[1] <= 1.f && sRGB[2] <= 1.f)
      C_lo = C_mid;
    else
      C_hi = C_mid;
  }
  return C_lo;
}

// _ucs_hue_to_ryb_hue, with _hue_to_srgb
static float _harmonizer_ucs_hue_to_ryb_hue(const float ucs_hue)
{
  const float L_white = Y_to_dt_UCS_L_star(1.0f);
  const float H = ucs_hue * DT_2PI_F - M_PI_F;
  dt_aligned_pixel_t JCH = { 0.65f, _harmonizer_max_chroma(ucs_hue) * 0.85f, H, 0.f };
  dt_aligned_pixel_t sRGB;
  dt_UCS_JCH_to_sRGB(JCH, L_white, sRGB);
  const dt_aligned_pixel_t srgb = { CLAMP(sRGB[0], 0.f, 1.f), CLAMP(sRGB[1], 0.f, 1.f), CLAMP(sRGB[2], 0.f, 1.f), 0.f };
  dt_aligned_pixel_t lrgb;
  dt_sRGB_to_linear_sRGB(srgb, lrgb);
  dt_aligned_pixel_t HCV;
  dt_RGB_2_HCV(lrgb, HCV);
  return dt_rgb_hue_to_ryb_hue(HCV[0]);
}

// _build_hue_luts
static void _harmonizer_build_luts(void)
{
  static gsize done = 0;
  if(!g_once_init_enter(&done)) return;
  for(int i = 0; i < _HARMONIZER_LUT_STEPS; i++)
    _harmonizer_ucs_to_ryb[i] = _harmonizer_ucs_hue_to_ryb_hue(i / (float)_HARMONIZER_LUT_STEPS);
  for(int j = 0; j < _HARMONIZER_LUT_STEPS; j++)
  {
    const float target = j / (float)_HARMONIZER_LUT_STEPS;
    float best_dist = 1.0f, best_ucs = 0.0f;
    for(int i = 0; i < _HARMONIZER_LUT_STEPS; i++)
    {
      float d = fabsf(_harmonizer_ucs_to_ryb[i] - target);
      if(d > 0.5f) d = 1.0f - d;
      if(d < best_dist)
      {
        best_dist = d;
        best_ucs = i / (float)_HARMONIZER_LUT_STEPS;
      }
    }
    _harmonizer_ryb_to_ucs[j] = best_ucs;
  }
  g_once_init_leave(&done, 1);
}

// _hue_lerp
static float _harmonizer_hue_lerp(float a, float b, const float t)
{
  if(b - a > 0.5f) b -= 1.0f;
  else if(a - b > 0.5f) a -= 1.0f;
  float r = a + t * (b - a);
  if(r < 0.0f) r += 1.0f;
  return r;
}

// _ucs_to_ryb_fast, _ryb_to_ucs_fast
static float _harmonizer_lookup(const float *lut, const float hue)
{
  const float pos = hue * _HARMONIZER_LUT_STEPS;
  const int i0 = (int)pos % _HARMONIZER_LUT_STEPS;
  const int i1 = (i0 + 1) % _HARMONIZER_LUT_STEPS;
  return _harmonizer_hue_lerp(lut[i0], lut[i1], pos - (int)pos);
}

static void _colorharmonizer(dt_iop_module_t *m, const char *field, const void *old)
{
  const int *rule = _P(int, m, "rule");
  const float *anchor_hue = _P(float, m, "anchor_hue");
  float *custom_hue = _P(float, m, "custom_hue");
  int *num_custom_nodes = _P(int, m, "num_custom_nodes");
  if(!rule || !anchor_hue || !custom_hue || !num_custom_nodes) return;
  // switching to custom: the nodes start where the old rule had them. the
  // window takes them from the vectorscope's guide when it shows one, which
  // only its histogram remembers; here they come from the rule itself
  // (_init_custom_nodes_from_rule, get_harmony_nodes)
  if(_is(field, "rule") && *rule == _HARMONIZER_CUSTOM)
  {
    const int old_rule = _OLD(int, m, old, "rule");
    if(old_rule == _HARMONIZER_CUSTOM) return;
    _harmonizer_build_luts();
    const int rotation = (int)roundf(_harmonizer_lookup(_harmonizer_ucs_to_ryb, *anchor_hue) * 360.0f) % 360;
    float node_angles[_HARMONIZER_MAX_NODES];
    int num_nodes = 0;
    dt_color_harmony_get_sector_angles((dt_color_harmony_type_t)(old_rule + 1), rotation, node_angles, &num_nodes);
    for(int i = 0; i < num_nodes; i++)
      custom_hue[i] = _harmonizer_lookup(_harmonizer_ryb_to_ucs, node_angles[i]);
    *num_custom_nodes = CLAMP(num_nodes, 2, _HARMONIZER_MAX_NODES);
    if(num_nodes < 2) custom_hue[1] = custom_hue[0];
  }
}

// -----------------------------------------------------------------------------

static const struct { const char *op; dt_api_also_changed_t fn; } _ports[] = {
  { "exposure", _exposure },
  { "agx", _agx },
  { "channelmixerrgb", _channelmixerrgb },
  { "profile_gamma", _profile_gamma },
  { "temperature", _temperature },
  { "spektrafilm", _spektrafilm },
  { "bilat", _bilat },
  { "colorharmonizer", _colorharmonizer },
  { "hazeremoval", _hazeremoval },
  { "colormapping", _colormapping },
  { "colorzones", _colorzones },
  { "borders", _borders },
  { "demosaic", _demosaic },
  { "negadoctor", _negadoctor },
};

dt_api_also_changed_t dt_api_also_changed_find(const char *op)
{
  for(size_t i = 0; i < G_N_ELEMENTS(_ports); i++)
    if(!g_strcmp0(op, _ports[i].op)) return _ports[i].fn;
  return NULL;
}
