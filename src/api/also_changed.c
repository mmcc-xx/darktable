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

// -----------------------------------------------------------------------------

static const struct { const char *op; dt_api_also_changed_t fn; } _ports[] = {
  { "exposure", _exposure },
  { "agx", _agx },
  { "channelmixerrgb", _channelmixerrgb },
};

dt_api_also_changed_t dt_api_also_changed_find(const char *op)
{
  for(size_t i = 0; i < G_N_ELEMENTS(_ports); i++)
    if(!g_strcmp0(op, _ports[i].op)) return _ports[i].fn;
  return NULL;
}
