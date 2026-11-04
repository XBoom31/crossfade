/* daily_accent.h - one shared, date-seeded accent colour.
 *
 * The colour is derived purely from the local calendar date, so every app
 * computes exactly the same value on a given day without talking to each
 * other or reading a state file. It changes at local midnight.
 *
 * This is the fallback when the desktop does not expose an accent colour
 * through the XDG desktop portal (or when P100_ACCENT is not set).
 */
#ifndef DAILY_ACCENT_H
#define DAILY_ACCENT_H

#include <time.h>

static inline unsigned int
accent_hash(int y, int m, int d)
{
	/* Better mixing for more randomness */
	unsigned int h = 2166136261u; /* FNV-1a */
	h ^= (unsigned int) y;
	h *= 16777619u;
	h ^= (unsigned int) (y >> 8);
	h *= 16777619u;
	h ^= (unsigned int) m;
	h *= 16777619u;
	h ^= (unsigned int) d;
	h *= 16777619u;
	h ^= (unsigned int) (h >> 13);
	h *= 0x5bd1e995u;
	h ^= (unsigned int) (h >> 15);
	h ^= (unsigned int) (h << 11);
	h += (unsigned int) (h >> 17);
	return h;
}

/* Write "#rrggbb" for the given date. */
static inline void
accent_for_day(int y, int m, int d, char out[8])
{
	static const char hex[] = "0123456789abcdef";
	unsigned int h = accent_hash(y, m, d);

	/* Use full 360 degree hue range for unlimited colors */
	int hue = (int)(h % 360);
	/* Moderate saturation for readability: 65-85% */
	int sat = 650 + (int)((h >> 8) % 200);
	if (sat > 850) sat = 850;
	/* Darker value for better contrast with white text: 45-65% */
	int val = 450 + (int)((h >> 16) % 200);
	if (val > 650) val = 650;
	if (val < 450) val = 450;

	float hf = hue / 60.0f;
	int i = (int)hf;
	float f = hf - i;
	float v = val / 1000.0f;
	float s = sat / 1000.0f;
	float p = v * (1.0f - s);
	float q = v * (1.0f - s * f);
	float t = v * (1.0f - s * (1.0f - f));

	float rf = 0, gf = 0, bf = 0;
	switch (i % 6) {
		case 0: rf = v; gf = t; bf = p; break;
		case 1: rf = q; gf = v; bf = p; break;
		case 2: rf = p; gf = v; bf = t; break;
		case 3: rf = p; gf = q; bf = v; break;
		case 4: rf = t; gf = p; bf = v; break;
		case 5: rf = v; gf = p; bf = q; break;
		default: rf = v; gf = t; bf = p; break;
	}

	int r = (int)(rf * 255.0f + 0.5f);
	int g = (int)(gf * 255.0f + 0.5f);
	int b = (int)(bf * 255.0f + 0.5f);

	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	if (r < 0) r = 0;
	if (g < 0) g = 0;
	if (b < 0) b = 0;

	out[0] = '#';
	out[1] = hex[(r >> 4) & 0xf];
	out[2] = hex[r & 0xf];
	out[3] = hex[(g >> 4) & 0xf];
	out[4] = hex[g & 0xf];
	out[5] = hex[(b >> 4) & 0xf];
	out[6] = hex[b & 0xf];
	out[7] = '\0';
}

/* Optional RGB helper for toolkits that want ints (0..255). */
static inline void
accent_rgb(int y, int m, int d, int *r, int *g, int *b)
{
	char hex[8];
	unsigned int rr, gg, bb;

	accent_for_day(y, m, d, hex);
	rr = (unsigned int)((hex[1] <= '9' ? hex[1] - '0' : hex[1] - 'a' + 10) << 4)
	   | (unsigned int)(hex[2] <= '9' ? hex[2] - '0' : hex[2] - 'a' + 10);
	gg = (unsigned int)((hex[3] <= '9' ? hex[3] - '0' : hex[3] - 'a' + 10) << 4)
	   | (unsigned int)(hex[4] <= '9' ? hex[4] - '0' : hex[4] - 'a' + 10);
	bb = (unsigned int)((hex[5] <= '9' ? hex[5] - '0' : hex[5] - 'a' + 10) << 4)
	   | (unsigned int)(hex[6] <= '9' ? hex[6] - '0' : hex[6] - 'a' + 10);
	*r = (int)rr;
	*g = (int)gg;
	*b = (int)bb;
}

#endif /* DAILY_ACCENT_H */
