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
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef HAVE_LV2_1_18_6
#include <lv2/core/lv2.h>
#else
#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#endif

#include "./uris.h"

static bool printed_capacity_warning = false;

typedef struct {
	/* I/O ports */
	float const*             input[MAX_CHANNELS];
	float*                   output[MAX_CHANNELS];
	const LV2_Atom_Sequence* control;
	LV2_Atom_Sequence*       notify;

	/* atom-forge and URI mapping */
	LV2_URID_Map*        map;
	SpectraLV2URIs       uris;
	LV2_Atom_Forge       forge;
	LV2_Atom_Forge_Frame frame;

	uint32_t n_channels;
	double   rate;

	/* the state of the UI is stored here, so that
   * the GUI can be displayed & closed
   * without loosing current settings.
   */
	bool ui_active;
	bool send_settings_to_ui;

} Spectra;

static LV2_Handle
instantiate (const LV2_Descriptor*     descriptor,
             double                    rate,
             const char*               bundle_path,
             const LV2_Feature* const* features)
{
	(void)descriptor;  /* unused variable */
	(void)bundle_path; /* unused variable */

	Spectra* self = (Spectra*)calloc (1, sizeof (Spectra));
	if (!self) {
		return NULL;
	}

	int i;
	for (i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_URID__map)) {
			self->map = (LV2_URID_Map*)features[i]->data;
		}
	}

	if (!self->map) {
		fprintf (stderr, "Spectra.lv2 error: Host does not support urid:map\n");
		free (self);
		return NULL;
	}

	if (!strncmp (descriptor->URI, SPR_URI "#Mono", 31 + 5)) {
		self->n_channels = 1;
	} else {
		free (self);
		return NULL;
	}

	assert (self->n_channels <= MAX_CHANNELS);

	self->ui_active           = false;
	self->send_settings_to_ui = false;
	self->rate                = rate;

	lv2_atom_forge_init (&self->forge, self->map);
	map_spectra_uris (self->map, &self->uris);
	return (LV2_Handle)self;
}

static void
connect_port (LV2_Handle handle,
              uint32_t   port,
              void*      data)
{
	Spectra* self = (Spectra*)handle;

	switch ((PortIndex)port) {
		case SPR_CONTROL:
			self->control = (const LV2_Atom_Sequence*)data;
			break;
		case SPR_NOTIFY:
			self->notify = (LV2_Atom_Sequence*)data;
			break;
		default:
			if (port > SPR_WINDOW && port <= SPR_WINDOW + 2 * MAX_CHANNELS) {
				int chn = (port - SPR_WINDOW - 1) / 2;
				if (port & 1) {
					self->input[chn] = (float const*)data;
				} else {
					self->output[chn] = (float*)data;
				}
			}
			break;
	}
}

/** forge atom-vector of raw data */
static void
tx_rawaudio (LV2_Atom_Forge* forge, SpectraLV2URIs* uris,
             const int32_t channel, const size_t n_samples, void const* data)
{
	LV2_Atom_Forge_Frame frame;
	/* forge container object of type 'rawaudio' */
	lv2_atom_forge_frame_time (forge, 0);
	x_forge_object (forge, &frame, 1, uris->rawaudio);

	/* add integer attribute 'channelid' */
	lv2_atom_forge_property_head (forge, uris->channelid, 0);
	lv2_atom_forge_int (forge, channel);

	/* add vector of floats raw 'audiodata' */
	lv2_atom_forge_property_head (forge, uris->audiodata, 0);
	lv2_atom_forge_vector (forge, sizeof (float), uris->atom_Float, n_samples, data);

	/* close off atom-object */
	lv2_atom_forge_pop (forge, &frame);
}

static void
run (LV2_Handle handle, uint32_t n_samples)
{
	Spectra*       self     = (Spectra*)handle;
	const size_t   size     = (sizeof (float) * n_samples + 64) * self->n_channels;
	const uint32_t capacity = self->notify->atom.size;

	/* check if atom-port buffer is large enough to hold
   * all audio-samples and configuration settings */
	if (capacity < size + 160 + self->n_channels * 32) {
		if (!printed_capacity_warning) {
			fprintf (stderr, "Spectra.lv2 error: LV2 comm-buffersize is insufficient %d/%ld bytes.\n",
			         capacity, size + 160 + self->n_channels * 32);
			printed_capacity_warning = true;
		}
		return;
	}

	/* prepare forge buffer and initialize atom-sequence */
	lv2_atom_forge_set_buffer (&self->forge, (uint8_t*)self->notify, capacity);
	lv2_atom_forge_sequence_head (&self->forge, &self->frame, 0);

	/* Send settings to UI */
	if (self->send_settings_to_ui && self->ui_active) {
		self->send_settings_to_ui = false;
		/* forge container object of type 'ui_state' */
		LV2_Atom_Forge_Frame frame;
		lv2_atom_forge_frame_time (&self->forge, 0);
		x_forge_object (&self->forge, &frame, 1, self->uris.ui_state);
		/* forge attributes for 'ui_state' */
		lv2_atom_forge_property_head (&self->forge, self->uris.samplerate, 0);
		lv2_atom_forge_float (&self->forge, self->rate);

		/* close-off frame */
		lv2_atom_forge_pop (&self->forge, &frame);
	}

	/* Process incoming events from GUI */
	if (self->control) {
#if 0
    printf("CTRL size %d\n", (self->control)->atom.size);
    for(uint8_t xx=0; xx < (self->control)->atom.size + 8; ++xx) {
      uint8_t d = ((uint8_t*)(self->control))[xx];
      printf("%02x%s", d, (xx%4)==3? "|": " ");
    }
    printf("\n");
#endif
		LV2_Atom_Event* ev = lv2_atom_sequence_begin (&(self->control)->body);
		/* for each message from UI... */
		while (!lv2_atom_sequence_is_end (&(self->control)->body, (self->control)->atom.size, ev)) {
			/* .. only look at atom-events.. */
			if (ev->body.type == self->uris.atom_Blank || ev->body.type == self->uris.atom_Object) {
				const LV2_Atom_Object* obj = (LV2_Atom_Object*)&ev->body;
				//printf("BLANK recv.. %x %x %x %x\n", obj->atom.size, obj->atom.type, obj->body.id, obj->body.otype);
				/* interpret atom-objects: */
				if (obj->body.otype == self->uris.ui_on) {
					/* UI was activated */
					self->ui_active           = true;
					self->send_settings_to_ui = true;
				} else if (obj->body.otype == self->uris.ui_off) {
					/* UI was closed */
					self->ui_active = false;
				}
			}
			ev = lv2_atom_sequence_next (ev);
		}
	}

	/* process audio data */
	for (uint32_t c = 0; c < self->n_channels; ++c) {
		if (self->ui_active) {
			/* if UI is active, send raw audio data to UI */
			tx_rawaudio (&self->forge, &self->uris, c, n_samples, self->input[c]);
		}
		/* if not processing in-place, forward audio */
		if (self->input[c] != self->output[c]) {
			memcpy (self->output[c], self->input[c], sizeof (float) * n_samples);
		}
	}

	/* close off atom-sequence */
	lv2_atom_forge_pop (&self->forge, &self->frame);
}

static void
cleanup (LV2_Handle handle)
{
	free (handle);
}

/* clang-format off */
#define mkdesc(ID, NAME)                         \
  static const LV2_Descriptor descriptor##ID = { \
    SPR_URI NAME,                                \
    instantiate,                                 \
    connect_port,                                \
    NULL,                                        \
    run,                                         \
    NULL,                                        \
    cleanup,                                     \
    NULL                                         \
  };
/* clang-format on */

mkdesc (0, "#Mono")
    mkdesc (1, "#Mono_gtk")
        mkdesc (2, "#Stereo")
            mkdesc (3, "#Stereo_gtk")

                LV2_SYMBOL_EXPORT
    const LV2_Descriptor* lv2_descriptor (uint32_t index)
{
	switch (index) {
		case 0:
			return &descriptor0;
		case 1:
			return &descriptor1;
		case 2:
			return &descriptor2;
		case 3:
			return &descriptor3;
		default:
			return NULL;
	}
}
