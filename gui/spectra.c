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

#define MTR_URI SPR_URI "#"
#define MTR_GUI "ui"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "lv2/lv2plug.in/ns/extensions/ui/ui.h"
#include "../src/uris.h"

#ifndef MIN
#define MIN(A,B) ( (A) < (B) ? (A) : (B) )
#endif
#ifndef MAX
#define MAX(A,B) ( (A) > (B) ? (A) : (B) )
#endif

typedef struct {
  LV2_Atom_Forge forge;
  LV2_URID_Map*  map;
  SpectraLV2URIs uris;

  LV2UI_Write_Function write;
  LV2UI_Controller controller;

  RobWidget *hbox;
  RobTkLbl  *lbl_test;

  float    rate;
  uint32_t n_channels;

} SpectraUI;


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
  LV2_Atom* msg = (LV2_Atom*)lv2_atom_forge_blank(&ui->forge, &frame, 1, ui->uris.ui_off);
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
  LV2_Atom* msg = (LV2_Atom*)lv2_atom_forge_blank(&ui->forge, &frame, 1, ui->uris.ui_on);
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
  float peak = 0;
  for (uint32_t i=0; i < n_elem; ++i) {
    const float p = fabsf(data[i]);
    if (p > peak) peak = p;
  }
  char tmp[64];
  snprintf(tmp, 64, "c:%d n:%d l:%.2f\n", channel, n_elem, peak);
  robtk_lbl_set_text(ui->lbl_test, tmp);
}

/******************************************************************************
 * RobWidget
 */

static RobWidget * toplevel(SpectraUI* ui, void * const top)
{
  /* main widget: layout */
  ui->hbox = rob_hbox_new(FALSE, 2);
  robwidget_make_toplevel(ui->hbox, top);
  ROBWIDGET_SETNAME(ui->hbox, "spectra");

  ui->lbl_test = robtk_lbl_new("---------Test----------");
  /* main layout */
  rob_hbox_child_pack(ui->hbox, robtk_lbl_widget(ui->lbl_test), FALSE);

  return ui->hbox;
}

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

  ui->rate       = 48000;

  map_spectra_uris(ui->map, &ui->uris);
  lv2_atom_forge_init(&ui->forge, ui->map);

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

  robtk_lbl_destroy(ui->lbl_test);
  rob_box_destroy(ui->hbox);

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
  if (format == ui->uris.atom_eventTransfer
      && atom->type == ui->uris.atom_Blank
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
      // TODO re-init..
    }
  }
}

static const void*
extension_data(const char* uri)
{
  return NULL;
}

/* vi:set ts=8 sts=2 sw=2: */
