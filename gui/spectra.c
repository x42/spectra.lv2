/* simple spectrum analyzer
 *
 * Copyright (C) 2013 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define RTK_URI SPR_URI "#"
#define RTK_GUI "ui"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "lv2/lv2plug.in/ns/extensions/ui/ui.h"
#include "../src/uris.h"

#include "fft.c"

#ifndef MIN
#define MIN(A,B) ( (A) < (B) ? (A) : (B) )
#endif
#ifndef MAX
#define MAX(A,B) ( (A) > (B) ? (A) : (B) )
#endif

/* widget, window size */
#define WWIDTH  (ui->xyp->w_width)
#define WHEIGHT (ui->xyp->w_height)

/* annotation border left/top */
#define AWIDTH  (35.)
#define AHEIGHT (25.)

/* actual data area size */
#define DWIDTH  (WWIDTH - AWIDTH)
#define DHEIGHT (WHEIGHT - AHEIGHT)

struct FFTLogscale {
  float log_rate;
  float log_base;
  float data_size;
  float rate;
};

static void fl_init(struct FFTLogscale *fl, uint32_t window_size, double rate) {
  fl->data_size = window_size / 2;
  fl->log_rate  = (1.0f - 10000.0f/rate) / ((5000.0f/rate) * (5000.0f/rate));
  fl->log_base  = log10f(1.0f + fl->log_rate);
  fl->rate = rate;
}

static float ft_x_deflect_bin(struct FFTLogscale *fl, float b) {
  assert(fl->data_size > 0);
  return fast_log10(1.0 + b * fl->log_rate / (float) fl->data_size) / fl->log_base;
}

typedef struct {
  LV2_Atom_Forge forge;
  LV2_URID_Map*  map;
  SpectraLV2URIs uris;

  LV2UI_Write_Function write;
  LV2UI_Controller controller;

  RobWidget *vbox;
  RobTkXYp  *xyp;
  cairo_surface_t *ann_power;

  float rate;
  float ann_rate;
  uint32_t n_channels;
  float min_dB, max_dB, step_dB;

  uint32_t window_size;
  int      pink_scale;

  struct FFTAnalysis *fa;
  struct FFTLogscale fl;
  float *p_x, *p_y;

} SpectraUI;


static void draw_scales(SpectraUI* ui) {
  float x, y;
  robtk_xydraw_set_surface(ui->xyp, NULL);

  if (ui->ann_power) {
    cairo_surface_destroy (ui->ann_power);
  }

  ui->ann_power = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, WWIDTH, WHEIGHT);
  cairo_t *cr = cairo_create (ui->ann_power);

  cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
  cairo_rectangle(cr, 0.0, 0.0, WWIDTH, WHEIGHT);
  cairo_fill(cr);

  const float divisor = ui->rate / 2.0 / (float) fftx_bins(ui->fa);

  cairo_set_font_size(cr, 9);
  cairo_text_extents_t t_ext;

  char buf[32];
  /* horiz lines, dB */
  double dashes[] = { 3.0, 5.0 };
  cairo_set_line_width (cr, 1.0);

  for (float dB = 0; dB > ui->min_dB; dB -= ui->step_dB ) {
    sprintf(buf, "%+0.0fdB", dB );

    y = (dB - ui->min_dB) / (ui->max_dB - ui->min_dB );
    y = WHEIGHT - DHEIGHT * y;

    if (dB == 0.0) {
      cairo_set_dash(cr, NULL, 0, 0);
    } else {
      cairo_set_dash(cr, dashes, 2, 0.0);
    }

    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_move_to(cr, AWIDTH, rintf(y) + .5);
    cairo_line_to(cr, WWIDTH, rintf(y) + .5);
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
    cairo_text_extents(cr, buf, &t_ext);
    cairo_move_to(cr, AWIDTH - 2 - t_ext.width - t_ext.x_bearing, y + t_ext.height /2.0 - 1.0);
    cairo_show_text(cr, buf);
    cairo_stroke(cr);

  }

  /* freq scale */
  cairo_set_line_width (cr, 1.25);
  cairo_set_dash(cr, NULL, 0, 0);

  for (int32_t i = 0; i < 41; ++i) {
    if (i < 7 && (i%4)) continue;
    if (i==8) continue;
    const double f_m = pow(2, (i - 17) / 3.) * 1000.0;
    x = ft_x_deflect_bin(&ui->fl, f_m / divisor) * DWIDTH + AWIDTH;

    if (f_m >= ui->rate * .5) {
      break;
    }

    if (f_m < 1000.0) {
      sprintf(buf, "%0.0fHz", f_m);
    } else {
      sprintf(buf, "%0.1fkHz", f_m/1000.0);
    }

    cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
    cairo_move_to(cr, x + 2.0, 3.0);

    cairo_rotate(cr, M_PI / 2.0);
    cairo_show_text(cr, buf);
    cairo_rotate(cr, -M_PI / 2.0);
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_move_to(cr, rintf(x) - .5, WHEIGHT);
    cairo_line_to(cr, rintf(x) - .5, 0.0);
    cairo_stroke(cr);
  }

  cairo_destroy(cr);
  robtk_xydraw_set_surface(ui->xyp, ui->ann_power);
}

static void reinitialize_fft(SpectraUI* ui) {
  uint32_t fft_size = MIN (16384, MAX (1024, ui->window_size));
  fft_size--;
  fft_size |= fft_size >> 1;
  fft_size |= fft_size >> 2;
  fft_size |= fft_size >> 4;
  fft_size |= fft_size >> 8;
  fft_size |= fft_size >> 16;
  fft_size++;
  fft_size = MIN (16384, fft_size);

  if (ui->fa && ui->fa->window_size == fft_size) {
    return;
  }
  fftx_free(ui->fa);
  free(ui->p_x);
  free(ui->p_y);
  ui->fa = (struct FFTAnalysis*) malloc(sizeof(struct FFTAnalysis));
  fftx_init(ui->fa, fft_size, ui->rate, 60);
  fl_init(&ui->fl, fft_size, ui->rate);
  ui->p_x = (float*) malloc(fftx_bins(ui->fa) * sizeof(float));
  ui->p_y = (float*) malloc(fftx_bins(ui->fa) * sizeof(float));
}

/******************************************************************************
 * Communication with DSP backend -- send/receive settings
 */


/** notfiy backend that UI is closed */
static void ui_disable(LV2UI_Handle handle)
{
  SpectraUI* ui = (SpectraUI*)handle;

  uint8_t obj_buf[64];
  lv2_atom_forge_set_buffer(&ui->forge, obj_buf, 64);
  LV2_Atom_Forge_Frame frame;
  lv2_atom_forge_frame_time(&ui->forge, 0);
  LV2_Atom* msg = (LV2_Atom*)x_forge_object(&ui->forge, &frame, 1, ui->uris.ui_off);
  lv2_atom_forge_pop(&ui->forge, &frame);
  ui->write(ui->controller, 0, lv2_atom_total_size(msg), ui->uris.atom_eventTransfer, msg);
}

/** notify backend that UI is active:
 * request state and enable data-transmission */
static void ui_enable(LV2UI_Handle handle)
{
  SpectraUI* ui = (SpectraUI*)handle;
  uint8_t obj_buf[64];
  lv2_atom_forge_set_buffer(&ui->forge, obj_buf, 64);
  LV2_Atom_Forge_Frame frame;
  lv2_atom_forge_frame_time(&ui->forge, 0);
  LV2_Atom* msg = (LV2_Atom*)x_forge_object(&ui->forge, &frame, 1, ui->uris.ui_on);
  lv2_atom_forge_pop(&ui->forge, &frame);
  ui->write(ui->controller, 0, lv2_atom_total_size(msg), ui->uris.atom_eventTransfer, msg);
}

/******************************************************************************
 * WIDGET CALLBACKS
 */


/******************************************************************************/

/** this callback runs in the "communication" thread of the LV2-host
 * -- invoked via port_event(); please see notes there.
 *
 *  it acts as 'glue' between LV2 port_event() and GTK expose_event_callback()
 *
 *  This is a wrapper, around above 'update_scope_real()' it allows
 *  to update the UI state in sync with the 1st channel and
 *  upsamples the data if neccesary.
 */
static void update_spectrum(SpectraUI* ui, const uint32_t channel, const size_t n_elem, float const * data)
{
  /* this callback runs in the "communication" thread of the LV2-host
   * usually a g_timeout() at ~25fps
   */
  if (channel > ui->n_channels) {
    return;
  }

  /* TODO multi-channel */
  if (channel != 0) {
    return;
  }

  if (cairo_image_surface_get_width(ui->ann_power) != WWIDTH || cairo_image_surface_get_height(ui->ann_power) != WHEIGHT) {
    draw_scales(ui);
  }

  const float rwidth = DWIDTH / WWIDTH;
  const float rheight = DHEIGHT / WHEIGHT;
  const float aoffs_x = AWIDTH / WWIDTH;
  const float min_coeff = pow10f(.1 * ui->min_dB);
  const float hscale = rheight / (ui->max_dB - ui->min_dB);
  const int pink = ui->pink_scale;

  if (!fftx_run(ui->fa, n_elem, data)) {
    uint32_t p = 0;
    uint32_t b = fftx_bins(ui->fa);
    for (uint32_t i = 1; i < b-1; i++) {
#if 0
      float ffpow = ui->fa->power[i];
      if (pink) {
	float norm = fftx_freq_at_bin(ui->fa, i) / ui->fa->freq_per_bin;
	if (norm <= 1) { norm = 1; }
	ffpow *= norm * .5;
      }
#else
      const float ffpow = pink ? (ui->fa->power[i] * i * .5) : ui->fa->power[i];
#endif
      if (ffpow < min_coeff) {
	continue;
	ui->p_x[p] = ft_x_deflect_bin(&ui->fl, i) * rwidth + aoffs_x;
	ui->p_y[p] = 0;
      } else {
	ui->p_x[p] = ft_x_deflect_bin(&ui->fl, fftx_freq_at_bin(ui->fa, i) / ui->fa->freq_per_bin) * rwidth + aoffs_x;
	ui->p_y[p] = (fftx_power_to_dB (ffpow) - ui->min_dB) * hscale;
      }
      p++;
    }
    robtk_xydraw_set_points(ui->xyp, p, ui->p_x, ui->p_y);
  }
}

/******************************************************************************
 * RobWidget
 */

static void xydraw_size_request(RobWidget* handle, int* w, int* h) {
  *w = 800;
  *h = 400;
}

static void xydraw_size_allocate(RobWidget* handle, int w, int h) {
  RobTkXYp* d = (RobTkXYp*)GET_HANDLE(handle);
  d->w_width = w;
  d->w_height = h;
  d->map_xw = w;
  d->map_yh = h;
  robwidget_set_size(d->rw, w, h);
}

static RobWidget * toplevel(SpectraUI* ui, void * const top)
{
  ui->vbox = rob_vbox_new(FALSE, 2);
  robwidget_make_toplevel(ui->vbox, top);
  ROBWIDGET_SETNAME(ui->vbox, "spectra");

  ui->xyp = robtk_xydraw_new(800, 400);
  robwidget_set_size_allocate(ui->xyp->rw, xydraw_size_allocate);
  robwidget_set_size_request(ui->xyp->rw, xydraw_size_request);

  robtk_xydraw_set_linewidth(ui->xyp, 1.5);
  robtk_xydraw_set_drawing_mode(ui->xyp, RobTkXY_ymax_zline);

  rob_vbox_child_pack(ui->vbox, robtk_xydraw_widget(ui->xyp), TRUE, TRUE);

  ui->ann_power = NULL;
  draw_scales(ui);
  //robtk_xydraw_set_color(ui->xyz, .2, .9, .1, 1.0);

  robtk_xydraw_set_surface(ui->xyp, ui->ann_power);

  return ui->vbox;
}

#define LVGL_RESIZEABLE

/******************************************************************************
 * LV2
 */

static LV2UI_Handle
instantiate(
    void* const               ui_toplevel,
    const LV2UI_Descriptor*   descriptor,
    const char*               plugin_uri,
    const char*               bundle_path,
    LV2UI_Write_Function      write_function,
    LV2UI_Controller          controller,
    RobWidget**               widget,
    const LV2_Feature* const* features)
{
  SpectraUI* ui = (SpectraUI*)calloc(1, sizeof(SpectraUI));

  if (!ui) {
    fprintf(stderr, "Spectra.lv2 UI: out of memory\n");
    return NULL;
  }

  ui->map = NULL;
  *widget = NULL;

  if (!strncmp(plugin_uri, SPR_URI "#Mono", 31 + 5 )) {
    ui->n_channels = 1;
  } else if (!strncmp(plugin_uri, SPR_URI "#Stereo", 31 + 7)) {
    ui->n_channels = 2;
  } else {
    free(ui);
    return NULL;
  }

  for (int i = 0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_URID_URI "#map")) {
      ui->map = (LV2_URID_Map*)features[i]->data;
    }
  }

  if (!ui->map) {
    fprintf(stderr, "Spectra.lv2 UI: Host does not support urid:map\n");
    free(ui);
    return NULL;
  }

  /* initialize private data structure */
  ui->write      = write_function;
  ui->controller = controller;

  ui->rate    = 48000;
  ui->min_dB  = -92.0;
  ui->max_dB  =  6.0;
  ui->step_dB =  6.0;

  ui->window_size = 4096;
  ui->pink_scale  =  0;

  map_spectra_uris(ui->map, &ui->uris);
  lv2_atom_forge_init(&ui->forge, ui->map);

  reinitialize_fft(ui);

  *widget = toplevel(ui, ui_toplevel);
  ui_enable(ui);
  return ui;
}

static enum LVGLResize
plugin_scale_mode(LV2UI_Handle handle)
{
  return LVGL_LAYOUT_TO_FIT;
}

static void
cleanup(LV2UI_Handle handle)
{
  SpectraUI* ui = (SpectraUI*)handle;
  /* send message to DSP backend:
   * save state & disable message transmission
   */
  ui_disable(ui);

  robtk_xydraw_destroy(ui->xyp);
  cairo_surface_destroy (ui->ann_power);
  rob_box_destroy(ui->vbox);
  fftx_free(ui->fa);
  free(ui->p_x);
  free(ui->p_y);

  free(ui);
}

/** receive data from the DSP-backend.
 *
 * this callback runs in the "communication" thread of the LV2-host
 * jalv and ardour do this via a g_timeout() function at ~25fps
 *
 * the atom-events from the DSP backend are written into a ringbuffer
 * in the host (in the DSP|jack realtime thread) the host then
 * empties this ringbuffer by sending port_event()s to the UI at some
 * random time later.  When CPU and DSP load are large the host-buffer
 * may overflow and some events may get lost.
 *
 * This thread does is not [usually] the 'drawing' thread (it does not
 * have X11 or gl context).
 */
static void
port_event(LV2UI_Handle handle,
           uint32_t     port_index,
           uint32_t     buffer_size,
           uint32_t     format,
           const void*  buffer)
{
  SpectraUI* ui = (SpectraUI*)handle;
  LV2_Atom* atom = (LV2_Atom*)buffer;

  /* check type of data received
   *  format == 0: [float] control-port event
   *  format > 0: message
   *  Every event message is sent as separate port-event
   */
  if (format == 0) {
    const float val = *((const float*)buffer);
    switch (port_index) {
      case SPR_FFTSIZE:
	if (ui->window_size != val) {
	  ui->window_size = val;
	}
	reinitialize_fft(ui);
	draw_scales(ui);
	break;
      case SPR_WEIGHT:
	ui->pink_scale = (val != 0);
	break;
      default:
	break;
    }
  }
  else if (format == ui->uris.atom_eventTransfer
      && (atom->type == ui->uris.atom_Blank || atom->type == ui->uris.atom_Object)
      )
  {
    /* cast the buffer to Atom Object */
    LV2_Atom_Object* obj = (LV2_Atom_Object*)atom;
    LV2_Atom *a0 = NULL;
    LV2_Atom *a1 = NULL;
    if (
	/* handle raw-audio data objects */
	obj->body.otype == ui->uris.rawaudio
	/* retrieve properties from object and
	 * check that there the [here] two required properties are set.. */
	&& 2 == lv2_atom_object_get(obj, ui->uris.channelid, &a0, ui->uris.audiodata, &a1, NULL)
	/* ..and non-null.. */
	&& a0
	&& a1
	/* ..and match the expected type */
	&& a0->type == ui->uris.atom_Int
	&& a1->type == ui->uris.atom_Vector
	)
    {
      /* single integer value can be directly dereferenced */
      const int32_t chn = ((LV2_Atom_Int*)a0)->body;

      /* dereference and typecast vector pointer */
      LV2_Atom_Vector* vof = (LV2_Atom_Vector*)LV2_ATOM_BODY(a1);
      /* check if atom is indeed a vector of the expected type*/
      if (vof->atom.type == ui->uris.atom_Float) {
	/* get number of elements in vector
	 * = (raw 8bit data-length - header-length) / sizeof(expected data type:float) */
	const size_t n_elem = (a1->size - sizeof(LV2_Atom_Vector_Body)) / vof->atom.size;
	/* typecast, dereference pointer to vector */
	const float *data = (float*) LV2_ATOM_BODY(&vof->atom);
	/* call function that handles the actual data */
	update_spectrum(ui, chn, n_elem, data);
      }
    }
    else if (
	/* handle 'state/settings' data object */
	obj->body.otype == ui->uris.ui_state
	/* retrieve properties from object and
	 * check that there the [here] three required properties are set.. */
	&& 1 == lv2_atom_object_get(obj,
	  ui->uris.samplerate, &a0, NULL)
	/* ..and non-null.. */
	&& a0
	/* ..and match the expected type */
	&& a0->type == ui->uris.atom_Float
	)
    {
      ui->rate = ((LV2_Atom_Float*)a0)->body;
      reinitialize_fft(ui);
      draw_scales(ui);
    }
  }
}

static const void*
extension_data(const char* uri)
{
  return NULL;
}

/* vi:set ts=8 sts=2 sw=2: */
