/*
 * UAE - The Un*x Amiga Emulator
 *
 * Drive Click Emulation Support Functions
 *
 * Copyright 2004 James Bagg, Toni Wilen
 */


#include "sysconfig.h"
#include "sysdeps.h"

#ifdef DRIVESOUND

#include "uae.h"
#include "options.h"
#include "audio.h"
#include "sounddep/sound.h"
#include "zfile.h"
#include "fsdb.h"
#include "events.h"
#include "driveclick.h"

#include <xgraphics.h>

static struct drvsample drvs[4][DS_END];
static int freq = 44100;

static int drv_starting[4], drv_spinning[4], drv_has_spun[4], drv_has_disk[4];

static int click_initialized, wave_initialized;
#define DS_SHIFT 10
static int sample_step;

static uae_s16 *clickbuffer;

uae_s16 *decodewav (uae_u8 *s, int *lenp)
{
	uae_s16 *dst;
	uae_u8 *src = s;
	int len;

	if (memcmp (s, "RIFF", 4))
		return 0;
	if (memcmp (s + 8, "WAVE", 4))
		return 0;
	s += 12;
	len = *lenp;
	while (s < src + len) {
		if (!memcmp (s, "fmt ", 4))
			freq = s[8 + 4] | (s[8 + 5] << 8);
		if (!memcmp (s, "data", 4)) {
			s += 4;
			len = s[0] | (s[1] << 8) | (s[2] << 16) | (s[3] << 24);
			dst = xmalloc (uae_s16, len / 2);

			memcpy (dst, s + 4, len);

			XGEndianSwapMemory(dst, dst, XGENDIAN_8IN16, sizeof(short), len/2);

			*lenp = len / 2;
			return dst;
		}
		s += 8 + (s[4] | (s[5] << 8) | (s[6] << 16) | (s[7] << 24));
	}
	return 0;
}

static int loadsample (TCHAR *path, struct drvsample *ds)
{
	struct zfile *f;
	uae_u8 *buf;
	int size;
	TCHAR name[MAX_DPATH];

	f = zfile_fopen (path, "rb", ZFD_NORMAL);
	if (!f) {
		_tcscpy (name, path);
		_tcscat (name, ".wav");
		f = zfile_fopen (name, "rb", ZFD_NORMAL);
		if (!f) {
			write_log ("driveclick: can't open '%s' (or '%s')\n", path, name);
			return 0;
		}
	}
	zfile_fseek (f, 0, SEEK_END);
	size = zfile_ftell (f);
	buf = xmalloc (uae_u8, size);
	zfile_fseek (f, 0, SEEK_SET);
	zfile_fread (buf, size, 1, f);
	zfile_fclose (f);
	ds->len = size;
	ds->p = decodewav (buf, &ds->len);
	xfree (buf);
	return 1;
}

static void freesample (struct drvsample *s)
{
	xfree (s->p);
	s->p = 0;
}

static void processclicks (struct drvsample *ds)
{
	unsigned int n = 0;
	unsigned int nClick = 0;

	for (n = 0; n < CLICK_TRACKS; n++)  {
		ds->indexes[n] = 0;
		ds->lengths[n] = 0;
	}
	for(n = 0; n < ds->len; n++) {
		uae_s16 smp = ds->p[n];
		if (smp > 0x6ff0 && nClick < CLICK_TRACKS)  {
			ds->indexes[nClick] = n - 128;
			ds->lengths[nClick] = 2800;
			nClick ++;
			n += 3000;
		}
	}
	if (nClick == 0) {
		for(n = 0; n < CLICK_TRACKS; n++) {
			ds->indexes[n] = 0;
			ds->lengths[n] = ds->len;
		}
	} else {
		if (nClick == 1) {
			ds->lengths[0] = ds->len - ds->indexes[0];
			for(n = 1; n < CLICK_TRACKS; n++) {
				ds->indexes[n] = ds->indexes[0];
				ds->lengths[n] = ds->lengths[0];
			}
		} else  {
			for(n = nClick; n < CLICK_TRACKS; n++) {
				ds->indexes[n] = ds->indexes[nClick-1];
				ds->lengths[n] = ds->lengths[nClick-1];
			}
		}
	}
}
void driveclick_init (void)
{
	int v, vv, i, j;
	TCHAR tmp[MAX_DPATH];
	
	driveclick_fdrawcmd_detect ();
	driveclick_free ();
	vv = 0;
	for (i = 0; i < 4; i++) {
		struct floppyslot *fs = &currprefs.floppyslots[i];
 
		for (j = 0; j < CLICK_TRACKS; j++)  {
			drvs[i][DS_CLICK].indexes[j] = 0;
			drvs[i][DS_CLICK].lengths[j] = 0;
		}
		if (fs->dfxclick) {
			if (fs->dfxclick > 0) {
				v = 0;
				switch(fs->dfxclick)
				{
				case 1:
					if (driveclick_loadresource (drvs[i], fs->dfxclick))
						v = 3;
					for (j = 0; j < CLICK_TRACKS; j++)
						drvs[i][DS_CLICK].lengths[j] = drvs[i][DS_CLICK].len;
					wave_initialized = 1;
					break;
				default:
					if (driveclick_fdrawcmd_open (fs->dfxclick - 2))
						v = 1;
					break;
				}
			} else if (fs->dfxclick == -1) {
				TCHAR path2[MAX_DPATH];
				wave_initialized = 1;
				for (j = 0; j < CLICK_TRACKS; j++)
					drvs[i][DS_CLICK].lengths[j] = drvs[i][DS_CLICK].len;
/*				_stprintf (tmp, "%splugins%cfloppysounds%c", "./", FSDB_DIR_SEPARATOR, FSDB_DIR_SEPARATOR, FSDB_DIR_SEPARATOR);
				if (my_existsdir (tmp))
					_tcscpy (path2, tmp);
				else*/
					_stprintf (path2, "GAME:\\DATA\\", "\\", FSDB_DIR_SEPARATOR);
				_stprintf (tmp, "%sdrive_click%s",
					path2, fs->dfxclickexternal);
				v = loadsample (tmp, &drvs[i][DS_CLICK]);
				if (v)
					processclicks (&drvs[i][DS_CLICK]);
				_stprintf (tmp, "%sdrive_spin%s",
					path2, fs->dfxclickexternal);
				v += loadsample (tmp, &drvs[i][DS_SPIN]);
				_stprintf (tmp, "%sdrive_spinnd%s",
					path2, fs->dfxclickexternal);
				v += loadsample (tmp, &drvs[i][DS_SPINND]);
				_stprintf (tmp, "%sdrive_startup%s",
					path2, fs->dfxclickexternal);
				v += loadsample (tmp, &drvs[i][DS_START]);
				_stprintf (tmp, "%sdrive_snatch%s",
					path2, fs->dfxclickexternal);
				v += loadsample (tmp, &drvs[i][DS_SNATCH]);
			}
			if (v == 0) {
				int j;
				for (j = 0; j < DS_END; j++)
					freesample (&drvs[i][j]);
				fs->dfxclick = changed_prefs.floppyslots[i].dfxclick = 0;
			} else {
				vv++;
			}
			for (j = 0; j < DS_END; j++)
				drvs[i][j].len <<= DS_SHIFT;
			drvs[i][DS_CLICK].pos = drvs[i][DS_CLICK].len;
			drvs[i][DS_SNATCH].pos = drvs[i][DS_SNATCH].len;
		}
	}
	driveclick_reset ();
	if (vv > 0)
		click_initialized = 1;
	if (v != 5) { click_initialized= 0; wave_initialized = 0;}
}

void driveclick_reset (void)
{
	xfree (clickbuffer);
	clickbuffer = NULL;
	if (!wave_initialized)
		return;
	clickbuffer = xmalloc (uae_s16, paula_sndbufsize / 2);
	sample_step = (freq << DS_SHIFT) / currprefs.sound_freq;
}

void driveclick_free (void)
{
	int i, j;

	driveclick_fdrawcmd_close (0);
	driveclick_fdrawcmd_close (1);
	for (i = 0; i < 4; i++) {
		for (j = 0; j < DS_END; j++)
			freesample (&drvs[i][j]);
		drv_starting[i] = 0;
		drv_spinning[i] = 0;
		drv_has_spun[i] = 0;
		drv_has_disk[i] = 0;
	}
	memset (drvs, 0, sizeof (drvs));
	click_initialized = 0;
	wave_initialized = 0;
	driveclick_reset ();
}

static int driveclick_active (void)
{
	int i;
	for (i = 0; i < 4; i++) {
		if (currprefs.floppyslots[i].dfxclick) {
			if (drv_spinning[i] || drv_starting[i])
				return 1;
		}
	}
	return 0;
}

STATIC_INLINE uae_s16 getsample (void)
{
	uae_s32 smp = 0;
	int div = 0, i;

	for (i = 0; i < 4; i++) {
		if (currprefs.floppyslots[i].dfxclick) {
			struct drvsample *ds_start = &drvs[i][DS_START];
			struct drvsample *ds_spin = drv_has_disk[i] ? &drvs[i][DS_SPIN] : &drvs[i][DS_SPINND];
			struct drvsample *ds_click = &drvs[i][DS_CLICK];
			struct drvsample *ds_snatch = &drvs[i][DS_SNATCH];
			div += 2;
			if (drv_spinning[i] || drv_starting[i]) {
				if (drv_starting[i] && drv_has_spun[i]) {
					if (ds_start->p && ds_start->pos < ds_start->len) {
						smp = ds_start->p[ds_start->pos >> DS_SHIFT];
						ds_start->pos += sample_step;
					} else {
						drv_starting[i] = 0;
					}
				} else if (drv_starting[i] && drv_has_spun[i] == 0) {
					if (ds_snatch->p && ds_snatch->pos < ds_snatch->len) {
						smp = ds_snatch->p[ds_snatch->pos >> DS_SHIFT];
						ds_snatch->pos += sample_step;
					} else {
						drv_starting[i] = 0;
						ds_start->pos = ds_start->len;
						drv_has_spun[i] = 1;
					}
				}
				if (ds_spin->p && drv_starting[i] == 0) {
					if (ds_spin->pos >= ds_spin->len)
						ds_spin->pos -= ds_spin->len;
					smp = ds_spin->p[ds_spin->pos >> DS_SHIFT];
					ds_spin->pos += sample_step;
				}
			}
			if (ds_click->p && ds_click->pos < ds_click->len) {
				smp += ds_click->p[ds_click->pos >> DS_SHIFT];
				ds_click->pos += sample_step;
			}
		}
	}
	if (!div)
		return 0;
	return smp / div;
}

static int clickcnt;

static void mix (void)
{
	int total = ((uae_u8*)paula_sndbufpt - (uae_u8*)paula_sndbuffer) / (currprefs.sound_stereo ? 4 : 2);

	if (currprefs.dfxclickvolume > 0) {
		while (clickcnt < total) {
			clickbuffer[clickcnt++] = getsample () * (100 - currprefs.dfxclickvolume) / 100;
		}
	} else {
		while (clickcnt < total) {
			clickbuffer[clickcnt++] = getsample ();
		}
	}
}

STATIC_INLINE uae_s16 limit (uae_s32 v)
{
	if (v < -32768)
		v = -32768;
	if (v > 32767)
		v = 32767;
	return v;
}

void driveclick_mix (uae_s16 *sndbuffer, int size, int channelmask)
{
	int i;

	if (!wave_initialized)
		return;
	mix ();
	clickcnt = 0;
	switch (get_audio_nativechannels (currprefs.sound_stereo))
	{
	case 6:
		for (i = 0; i < size / 6; i++) {
			uae_s16 s = clickbuffer[i];
			if (channelmask & 1)
				sndbuffer[0] = limit (((sndbuffer[0] + s) * 2) / 3);
			else
				sndbuffer[0] = sndbuffer[0] * 2 / 3;
			if (channelmask & 2)
				sndbuffer[1] = limit (((sndbuffer[1] + s) * 2) / 3);
			else
				sndbuffer[1] = sndbuffer[1] * 2 / 3;
			if (channelmask & 4)
				sndbuffer[2] = limit (((sndbuffer[2] + s) * 2) / 3);
			else
				sndbuffer[2] = sndbuffer[2] * 2 / 3;
			if (channelmask & 8)
				sndbuffer[3] = limit (((sndbuffer[3] + s) * 2) / 3);
			else
				sndbuffer[3] = sndbuffer[3] * 2 / 3;
			if (channelmask & 16)
				sndbuffer[4] = limit (((sndbuffer[4] + s) * 2) / 3);
			else
				sndbuffer[4] = sndbuffer[4] * 2 / 3;
			if (channelmask & 32)
				sndbuffer[5] = limit (((sndbuffer[5] + s) * 2) / 3);
			else
				sndbuffer[5] = sndbuffer[5] * 2 / 3;
			sndbuffer += 6;
		}
		break;
	case 4:
		for (i = 0; i < size / 4; i++) {
			uae_s16 s = clickbuffer[i];
			if (channelmask & 1)
				sndbuffer[0] = limit (((sndbuffer[0] + s) * 2) / 3);
			else
				sndbuffer[0] = sndbuffer[0] * 2 / 3;
			if (channelmask & 2)
				sndbuffer[1] = limit (((sndbuffer[1] + s) * 2) / 3);
			else
				sndbuffer[1] = sndbuffer[1] * 2 / 3;
			if (channelmask & 4)
				sndbuffer[2] = limit (((sndbuffer[2] + s) * 2) / 3);
			else
				sndbuffer[2] = sndbuffer[2] * 2 / 3;
			if (channelmask & 8)
				sndbuffer[3] = limit (((sndbuffer[3] + s) * 2) / 3);
			else
				sndbuffer[3] = sndbuffer[3] * 2 / 3;
			sndbuffer += 4;
		}
		break;
	case 2:
		for (i = 0; i < size / 2; i++) {
			uae_s16 s = clickbuffer[i];
			if (channelmask & 1)
				sndbuffer[0] = limit (((sndbuffer[0] + s) * 2) / 3);
			else
				sndbuffer[0] = sndbuffer[0] * 2 / 3;
			if (channelmask & 2)
				sndbuffer[1] = limit (((sndbuffer[1] + s) * 2) / 3);
			else
				sndbuffer[1] = sndbuffer[1] * 2 / 3;
			sndbuffer += 2;
		}
		break;
	case 1:
		for (i = 0; i < size; i++) {
			uae_s16 s = clickbuffer[i];
			if (channelmask & 1)
				sndbuffer[0] = limit (((sndbuffer[0] + s) * 2) / 3);
			else
				sndbuffer[0] = sndbuffer[0] * 2 / 3;
			sndbuffer++;
		}
		break;
	}
}

static void dr_audio_activate (void)
{
	if (audio_activate ())
		clickcnt = 0;
}

void driveclick_click (int drive, int cyl)
{
	static int prevcyl[4];

	if (!click_initialized)
		return;
	if (!currprefs.floppyslots[drive].dfxclick)
		return;
	if (prevcyl[drive] == 0 && cyl == 0) // "noclick" check
		return;
	dr_audio_activate ();
	prevcyl[drive] = cyl;
	if (!wave_initialized) {
		driveclick_fdrawcmd_seek (currprefs.floppyslots[drive].dfxclick - 2, cyl);
		return;
	}
	mix ();
	drvs[drive][DS_CLICK].pos = drvs[drive][DS_CLICK].indexes[cyl] << DS_SHIFT;
	drvs[drive][DS_CLICK].len = (drvs[drive][DS_CLICK].indexes[cyl] + (drvs[drive][DS_CLICK].lengths[cyl] / 2)) << DS_SHIFT;
}

void driveclick_motor (int drive, int running)
{
	if (!click_initialized)
		return;
	if (!currprefs.floppyslots[drive].dfxclick)
		return;
	if (!wave_initialized) {
		driveclick_fdrawcmd_motor (currprefs.floppyslots[drive].dfxclick - 2, running);
		return;
	}
	mix ();
	if (running == 0) {
		drv_starting[drive] = 0;
		drv_spinning[drive] = 0;
	} else {
		if (drv_spinning[drive] == 0) {
			dr_audio_activate();
			drv_starting[drive] = 1;
			drv_spinning[drive] = 1;
			if (drv_has_disk[drive] && drv_has_spun[drive] == 0 && drvs[drive][DS_SNATCH].pos >= drvs[drive][DS_SNATCH].len)
				drvs[drive][DS_SNATCH].pos = 0;
			if (running == 2)
				drvs[drive][DS_START].pos = 0;
			drvs[drive][DS_SPIN].pos = 0;
		}
	}
}

void driveclick_insert (int drive, int eject)
{
	if (!click_initialized)
		return;
	if (!wave_initialized)
		return;
	if (!currprefs.floppyslots[drive].dfxclick)
		return;
	if (eject)
		drv_has_spun[drive] = 0;
	if (drv_has_disk[drive] == 0 && !eject)
		dr_audio_activate ();
	drv_has_disk[drive] = !eject;
}

void driveclick_check_prefs (void)
{
	int i;

	if (!config_changed)
		return;
	driveclick_fdrawcmd_vsync ();
	if (driveclick_active ())
		dr_audio_activate ();
	if (currprefs.dfxclickvolume != changed_prefs.dfxclickvolume ||
		currprefs.floppyslots[0].dfxclick != changed_prefs.floppyslots[0].dfxclick ||
		currprefs.floppyslots[1].dfxclick != changed_prefs.floppyslots[1].dfxclick ||
		currprefs.floppyslots[2].dfxclick != changed_prefs.floppyslots[2].dfxclick ||
		currprefs.floppyslots[3].dfxclick != changed_prefs.floppyslots[3].dfxclick ||
		_tcscmp (currprefs.floppyslots[0].dfxclickexternal, changed_prefs.floppyslots[0].dfxclickexternal) ||
		_tcscmp (currprefs.floppyslots[1].dfxclickexternal, changed_prefs.floppyslots[1].dfxclickexternal) ||
		_tcscmp (currprefs.floppyslots[2].dfxclickexternal, changed_prefs.floppyslots[2].dfxclickexternal) ||
		_tcscmp (currprefs.floppyslots[3].dfxclickexternal, changed_prefs.floppyslots[3].dfxclickexternal))
	{
		currprefs.dfxclickvolume = changed_prefs.dfxclickvolume;
		for (i = 0; i < 4; i++) {
			currprefs.floppyslots[i].dfxclick = changed_prefs.floppyslots[i].dfxclick;
			_tcscpy (currprefs.floppyslots[i].dfxclickexternal, changed_prefs.floppyslots[i].dfxclickexternal);
		}
		driveclick_init ();
	}
}

#endif
