// SPDX-License-Identifier: MIT
/*
 * uemis.c
 *
 * UEMIS SDA file importer
 * AUTHOR:  Dirk Hohndel - Copyright 2011
 *
 * Licensed under the MIT license.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "gettext.h"

#include "uemis.h"
#include "divesite.h"
#include "sample.h"
#include <libdivecomputer/parser.h>
#include <libdivecomputer/version.h>

/*
 * following code is based on code found in at base64.sourceforge.net/b64.c
 * AUTHOR:         Bob Trower 08/04/01
 * COPYRIGHT:      Copyright (c) Trantor Standard Systems Inc., 2001
 * NOTE:           This source code may be used as you wish, subject to
 *                 the MIT license.
 */
/*
 * Translation Table to decode (created by Bob Trower)
 */
static const char cd64[] = "|$$$}rstuvwxyz{$$$$$$$>?@ABCDEFGHIJKLMNOPQRSTUVW$$$$$$XYZ[\\]^_`abcdefghijklmnopq";

/*
 * decodeblock -- decode 4 '6-bit' characters into 3 8-bit binary bytes
 */
static void decodeblock(unsigned char in[4], unsigned char out[3])
{
	out[0] = (unsigned char)(in[0] << 2 | in[1] >> 4);
	out[1] = (unsigned char)(in[1] << 4 | in[2] >> 2);
	out[2] = (unsigned char)(((in[2] << 6) & 0xc0) | in[3]);
}

/*
 * decode a base64 encoded stream discarding padding, line breaks and noise
 */
static void decode(uint8_t *inbuf, uint8_t *outbuf, int inbuf_len)
{
	uint8_t in[4], out[3], v;
	int i, len, indx_in = 0, indx_out = 0;

	while (indx_in < inbuf_len) {
		for (len = 0, i = 0; i < 4 && (indx_in < inbuf_len); i++) {
			v = 0;
			while ((indx_in < inbuf_len) && v == 0) {
				v = inbuf[indx_in++];
				v = ((v < 43 || v > 122) ? 0 : cd64[v - 43]);
				if (v)
					v = ((v == '$') ? 0 : v - 61);
			}
			if (indx_in < inbuf_len) {
				len++;
				if (v)
					in[i] = (v - 1);
			} else
				in[i] = 0;
		}
		if (len) {
			decodeblock(in, out);
			for (i = 0; i < len - 1; i++)
				outbuf[indx_out++] = out[i];
		}
	}
}
/* end code from Bob Trower */

/*
 * convert the base64 data blog
 */
static int uemis_convert_base64(char *base64, uint8_t **data)
{
	int len, datalen;

	len = strlen(base64);
	datalen = (len / 4 + 1) * 3;
	if (datalen < 0x123 + 0x25)
		/* less than header + 1 sample??? */
		fprintf(stderr, "suspiciously short data block %d\n", datalen);

	*data = malloc(datalen);
	if (!*data) {
		fprintf(stderr, "Out of memory\n");
		return 0;
	}
	decode((unsigned char *)base64, *data, len);

	if (memcmp(*data, "Dive\01\00\00", 7))
		fprintf(stderr, "Missing Dive100 header\n");

	return datalen;
}

struct uemis_helper {
	uint32_t diveid;
	int lbs;
	int divespot;
	struct dive_site *dive_site;
	struct uemis_helper *next;
};
static struct uemis_helper *uemis_helper = NULL;

static struct uemis_helper *uemis_get_helper(uint32_t diveid)
{
	struct uemis_helper **php = &uemis_helper;
	struct uemis_helper *hp = *php;

	while (hp) {
		if (hp->diveid == diveid)
			return hp;
		if (hp->next) {
			hp = hp->next;
			continue;
		}
		php = &hp->next;
		break;
	}
	hp = *php = calloc(1, sizeof(struct uemis_helper));
	hp->diveid = diveid;
	hp->next = NULL;
	return hp;
}

static void uemis_weight_unit(int diveid, int lbs)
{
	struct uemis_helper *hp = uemis_get_helper(diveid);
	if (hp)
		hp->lbs = lbs;
}

int uemis_get_weight_unit(uint32_t diveid)
{
	struct uemis_helper *hp = uemis_helper;
	while (hp) {
		if (hp->diveid == diveid)
			return hp->lbs;
		hp = hp->next;
	}
	/* odd - we should have found this; default to kg */
	return 0;
}

void uemis_mark_divelocation(int diveid, int divespot, struct dive_site *ds)
{
	struct uemis_helper *hp = uemis_get_helper(diveid);
	hp->divespot = divespot;
	hp->dive_site = ds;
}

/* support finding a dive spot based on the diveid */
int uemis_get_divespot_id_by_diveid(uint32_t diveid)
{
	struct uemis_helper *hp = uemis_helper;
	while (hp) {
		if (hp->diveid == diveid)
			return hp->divespot;
		hp = hp->next;
	}
	return -1;
}

void uemis_set_divelocation(int divespot, char *text, double longitude, double latitude)
{
	struct uemis_helper *hp = uemis_helper;
	while (hp) {
		if (hp->divespot == divespot) {
			struct dive_site *ds = hp->dive_site;
			if (ds) {
				ds->name = strdup(text);
				ds->location = create_location(latitude, longitude);
			}
		}
		hp = hp->next;
	}
}

/* Create events from the flag bits and other data in the sample;
 * These bits basically represent what is displayed on screen at sample time.
 * Many of these 'warnings' are way hyper-active and seriously clutter the
 * profile plot - so these are disabled by default
 *
 * we mark all the strings for translation, but we store the untranslated
 * strings and only convert them when displaying them on screen - this way
 * when we write them to the XML file we'll always have the English strings,
 * regardless of locale
 */
static void uemis_event(struct dive *dive, struct divecomputer *dc, struct sample *sample, uemis_sample_t *u_sample)
{
	uint8_t *flags = u_sample->flags;
	int stopdepth;
	static int lastndl;

	if (flags[1] & 0x01)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "Safety stop violation"));
	if (flags[1] & 0x08)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "Speed alarm"));
#if WANT_CRAZY_WARNINGS
	if (flags[1] & 0x06) /* both bits 1 and 2 are a warning */
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "Speed warning"));
	if (flags[1] & 0x10)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "pO₂ green warning"));
#endif
	if (flags[1] & 0x20)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "pO₂ ascend warning"));
	if (flags[1] & 0x40)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "pO₂ ascend alarm"));
	/* flags[2] reflects the deco / time bar
	 * flags[3] reflects more display details on deco and pO2 */
	if (flags[4] & 0x01)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "Tank pressure info"));
	if (flags[4] & 0x04)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "RGT warning"));
	if (flags[4] & 0x08)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "RGT alert"));
	if (flags[4] & 0x40)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "Tank change suggested"));
	if (flags[4] & 0x80)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "Depth limit exceeded"));
	if (flags[5] & 0x01)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "Max deco time warning"));
	if (flags[5] & 0x04)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "Dive time info"));
	if (flags[5] & 0x08)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "Dive time alert"));
	if (flags[5] & 0x10)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "Marker"));
	if (flags[6] & 0x02)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "No tank data"));
	if (flags[6] & 0x04)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "Low battery warning"));
	if (flags[6] & 0x08)
		add_event(dc, sample->time.seconds, 0, 0, 0, QT_TRANSLATE_NOOP("gettextFromC", "Low battery alert"));
/* flags[7] reflects the little on screen icons that remind of previous
	 * warnings / alerts - not useful for events */

#if UEMIS_DEBUG & 32
	int i, j;
	for (i = 0; i < 8; i++) {
		printf(" %d: ", 29 + i);
		for (j = 7; j >= 0; j--)
			printf("%c", flags[i] & 1 << j ? '1' : '0');
	}
	printf("\n");
#endif
	/* now add deco / NDL
	 * we don't use events but store this in the sample - that makes much more sense
	 * for the way we display this information
	 * What we know about the encoding so far:
	 * flags[3].bit0 | flags[5].bit1 != 0 ==> in deco
	 * flags[0].bit7 == 1 ==> Safety Stop
	 * otherwise NDL */
	stopdepth = rel_mbar_to_depth(u_sample->hold_depth, dive);
	if ((flags[3] & 1) | (flags[5] & 2)) {
		/* deco */
		sample->in_deco = true;
		sample->stopdepth.mm = stopdepth;
		sample->stoptime.seconds = u_sample->hold_time * 60;
		sample->ndl.seconds = 0;
	} else if (flags[0] & 128) {
		/* safety stop - distinguished from deco stop by having
		 * both ndl and stop information */
		sample->in_deco = false;
		sample->stopdepth.mm = stopdepth;
		sample->stoptime.seconds = u_sample->hold_time * 60;
		sample->ndl.seconds = lastndl;
	} else {
		/* NDL */
		sample->in_deco = false;
		lastndl = sample->ndl.seconds = u_sample->hold_time * 60;
		sample->stopdepth.mm = 0;
		sample->stoptime.seconds = 0;
	}
#if UEMIS_DEBUG & 32
	printf("%dm:%ds: p_amb_tol:%d surface:%d holdtime:%d holddepth:%d/%d ---> stopdepth:%d stoptime:%d ndl:%d\n",
	       sample->time.seconds / 60, sample->time.seconds % 60, u_sample->p_amb_tol, dive->dc.surface_pressure.mbar,
	       u_sample->hold_time, u_sample->hold_depth, stopdepth, sample->stopdepth.mm, sample->stoptime.seconds, sample->ndl.seconds);
#endif
}

/*
 * parse uemis base64 data blob into struct dive
 */
void uemis_parse_divelog_binary(char *base64, void *datap)
{
	int datalen;
	int i;
	uint8_t *data;
	struct sample *sample = NULL;
	uemis_sample_t *u_sample;
	struct dive *dive = datap;
	struct divecomputer *dc = &dive->dc;
	int template, gasoffset;
	uint8_t active = 0;

	datalen = uemis_convert_base64(base64, &data);
	dive->dc.airtemp.mkelvin = C_to_mkelvin((*(uint16_t *)(data + 45)) / 10.0);
	dive->dc.surface_pressure.mbar = *(uint16_t *)(data + 43);
	if (*(uint8_t *)(data + 19))
		dive->dc.salinity = SEAWATER_SALINITY; /* avg grams per 10l sea water */
	else
		dive->dc.salinity = FRESHWATER_SALINITY; /* grams per 10l fresh water */

	/* this will allow us to find the last dive read so far from this computer */
	dc->model = strdup("Uemis Zurich");
	dc->deviceid = *(uint32_t *)(data + 9);
	dc->diveid = *(uint16_t *)(data + 7);
	/* remember the weight units used in this dive - we may need this later when
	 * parsing the weight */
	uemis_weight_unit(dc->diveid, *(uint8_t *)(data + 24));
	/* dive template in use:
	   0 = air
	   1 = nitrox (B)
	   2 = nitrox (B+D)
	   3 = nitrox (B+T+D)
	   uemis cylinder data is insane - it stores seven tank settings in a block
	   and the template tells us which of the four groups of tanks we need to look at
	 */
	gasoffset = template = *(uint8_t *)(data + 115);
	if (template == 3)
		gasoffset = 4;
	if (template == 0)
		template = 1;
	for (i = 0; i < template; i++) {
		float volume = *(float *)(data + 116 + 25 * (gasoffset + i)) * 1000.0f;
		/* uemis always assumes a working pressure of 202.6bar (!?!?) - I first thought
		 * it was 3000psi, but testing against all my dives gets me that strange number.
		 * Still, that's of course completely bogus and shows they don't get how
		 * cylinders are named in non-metric parts of the world...
		 * we store the incorrect working pressure to get the SAC calculations "close"
		 * but the user will have to correct this manually
		 */
		cylinder_t *cyl = get_or_create_cylinder(dive, i);
		cyl->type.size.mliter = lrintf(volume);
		cyl->type.workingpressure.mbar = 202600;
		cyl->gasmix.o2.permille = *(uint8_t *)(data + 120 + 25 * (gasoffset + i)) * 10;
		cyl->gasmix.he.permille = 0;
	}
	/* first byte of divelog data is at offset 0x123 */
	i = 0x123;
	u_sample = (uemis_sample_t *)(data + i);
	while ((i <= datalen) && (data[i] != 0 || data[i + 1] != 0)) {
		if (u_sample->active_tank != active) {
			if (u_sample->active_tank >= dive->cylinders.nr) {
				fprintf(stderr, "got invalid sensor #%d was #%d\n", u_sample->active_tank, active);
			} else {
				active = u_sample->active_tank;
				add_gas_switch_event(dive, dc, u_sample->dive_time, active);
			}
		}
		sample = prepare_sample(dc);
		sample->time.seconds = u_sample->dive_time;
		sample->depth.mm = rel_mbar_to_depth(u_sample->water_pressure, dive);
		sample->temperature.mkelvin = C_to_mkelvin(u_sample->dive_temperature / 10.0);
		add_sample_pressure(sample, active, (u_sample->tank_pressure_high * 256 + u_sample->tank_pressure_low) * 10);
		sample->cns = u_sample->cns;
		uemis_event(dive, dc, sample, u_sample);
		finish_sample(dc);
		i += 0x25;
		u_sample++;
	}
	if (sample)
		dive->dc.duration.seconds = sample->time.seconds - 1;

	/* get data from the footer */
	char buffer[24];

	snprintf(buffer, sizeof(buffer), "%1u.%02u", data[18], data[17]);
	add_extra_data(dc, "FW Version", buffer);
	snprintf(buffer, sizeof(buffer), "%08x", *(uint32_t *)(data + 9));
	add_extra_data(dc, "Serial", buffer);
	snprintf(buffer, sizeof(buffer), "%d", *(uint16_t *)(data + i + 35));
	add_extra_data(dc, "main battery after dive", buffer);
	snprintf(buffer, sizeof(buffer), "%0u:%02u", FRACTION(*(uint16_t *)(data + i + 24), 60));
	add_extra_data(dc, "no fly time", buffer);
	snprintf(buffer, sizeof(buffer), "%0u:%02u", FRACTION(*(uint16_t *)(data + i + 26), 60));
	add_extra_data(dc, "no dive time", buffer);
	snprintf(buffer, sizeof(buffer), "%0u:%02u", FRACTION(*(uint16_t *)(data + i + 28), 60));
	add_extra_data(dc, "desat time", buffer);
	snprintf(buffer, sizeof(buffer), "%u", *(uint16_t *)(data + i + 30));
	add_extra_data(dc, "allowed altitude", buffer);

	return;
}
