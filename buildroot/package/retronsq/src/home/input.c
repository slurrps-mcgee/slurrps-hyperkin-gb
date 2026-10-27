/*
 * The home screen's input: pads and the keyboard as a few abstract buttons.
 *
 * Pads are mapped with RetroArch's own autoconfig files (the .cfg files in
 * retroarch/autoconfig on the SD card, e.g. Hyperkin_SQ.cfg), found by USB
 * vendor/product id or by name, so the home screen and RetroArch
 * always agree on which button is which. RetroArch's udev joypad driver
 * and SDL's Linux joystick code number buttons and axes the same way, so
 * the numbers in those files mean the same thing here. A pad with no file
 * gets a common default layout.
 *
 * Up/down/left/right and L/R repeat while held, so long lists scroll.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "home.h"
#include "../lib/sqcart.h"

#define AUTOCONFIG_DIR	"/userdata/retroarch/autoconfig"
#define MAX_PADS	4
#define AXIS_THRESHOLD	16000
#define REPEAT_DELAY	350	/* ms before a held direction repeats */
#define REPEAT_EVERY	70

enum bind_type { BIND_NONE, BIND_BUTTON, BIND_AXIS, BIND_HAT };
struct binding {
	enum bind_type type;
	int index;	/* button, axis or hat number */
	int dir;	/* axis: -1/+1; hat: SDL_HAT_UP... */
};

#define TEST_PAD_ID (-100)	/* input_test_pad's pretend pad */

struct pad {
	SDL_Joystick* js;
	int pretend;		/* tests: no device behind it */
	SDL_JoystickID id;
	struct binding map[BTN_COUNT];
	int down[BTN_COUNT];
};

static struct pad pads[MAX_PADS];

/* Pressed buttons waiting to be handled */
static int queue[32];
static int queue_len;

/* Held directions, for auto-repeat */
static int held[BTN_COUNT];
static Uint32 held_since[BTN_COUNT], last_repeat[BTN_COUNT];

static const char* cfg_key[BTN_COUNT] = {
	"up", "down", "left", "right", "a", "b", "x", "y", "l", "r", "start", "select"
};

static void push(int b)
{
	if (queue_len < (int)(sizeof(queue) / sizeof(queue[0])))
		queue[queue_len++] = b;
}

void input_inject(enum button b)
{
	push(b);
}

int input_next(void)
{
	int b;
	if (queue_len == 0)
		return -1;
	b = queue[0];
	memmove(queue, queue + 1, --queue_len * sizeof(queue[0]));
	return b;
}

static int repeats(int b)
{
	return b == BTN_UP || b == BTN_DOWN || b == BTN_LEFT || b == BTN_RIGHT ||
		b == BTN_L || b == BTN_R;
}

/* A button went down or up (from any pad or the keyboard) */
static void set_held(int b, int down)
{
	if (down && !held[b]) {
		push(b);
		held_since[b] = last_repeat[b] = SDL_GetTicks();
	}
	held[b] = down;
}

int input_repeat(Uint32 now)
{
	int b, next = -1;
	for (b = 0; b < BTN_COUNT; b++) {
		Uint32 due;
		if (!held[b] || !repeats(b))
			continue;
		due = now - held_since[b] < REPEAT_DELAY ? held_since[b] + REPEAT_DELAY
			: last_repeat[b] + REPEAT_EVERY;
		if ((Sint32)(now - due) >= 0) {
			push(b);
			last_repeat[b] = now;
			due = now + REPEAT_EVERY;
		}
		if (next < 0 || (int)(due - now) < next)
			next = (int)(due - now);
	}
	return next;
}

/* --- mapping a pad ---------------------------------------------------------- */

/* A RetroArch binding value: "7" (button), "-1"/"+1" (axis), "h0up" (hat) */
static int parse_binding(const char* v, struct binding* b, int axis)
{
	if (axis && (v[0] == '-' || v[0] == '+')) {
		b->type = BIND_AXIS;
		b->dir = v[0] == '-' ? -1 : 1;
		b->index = atoi(v + 1);
		return 1;
	}
	if (!axis && v[0] == 'h') {
		char* dir;
		b->type = BIND_HAT;
		b->index = (int)strtol(v + 1, &dir, 10);
		b->dir = !strcmp(dir, "up") ? SDL_HAT_UP : !strcmp(dir, "down") ? SDL_HAT_DOWN
			: !strcmp(dir, "left") ? SDL_HAT_LEFT : SDL_HAT_RIGHT;
		return 1;
	}
	if (!axis && v[0] >= '0' && v[0] <= '9') {
		b->type = BIND_BUTTON;
		b->index = atoi(v);
		return 1;
	}
	return 0;
}

/* Squeeze spaces, so " Hyperkin     SQ Game Pad   " matches SDL's name */
static void squeeze(const char* in, char* out, size_t len)
{
	size_t o = 0;
	int space = 1;
	for (; *in && o + 1 < len; in++) {
		if (*in == ' ' || *in == '\t') {
			if (!space)
				out[o++] = ' ';
			space = 1;
		} else {
			out[o++] = *in;
			space = 0;
		}
	}
	while (o > 0 && out[o - 1] == ' ')
		o--;
	out[o] = '\0';
}

/* Read one autoconfig file into map if it is for this pad; 1 when used */
static int load_autoconfig(const char* file, const char* name, int vid, int pid,
	struct binding* map)
{
	char line[256], key[64], value[128], dev[128] = "", want[128];
	int file_vid = -1, file_pid = -1, b;
	struct binding tmp[BTN_COUNT];
	FILE* fp = fopen(file, "r");

	if (!fp)
		return 0;
	memset(tmp, 0, sizeof(tmp));
	while (fgets(line, sizeof(line), fp)) {
		char* q;
		if (sscanf(line, " %63[a-z_] = \"%127[^\"]\"", key, value) != 2)
			continue;
		if (!strcmp(key, "input_device"))
			squeeze(value, dev, sizeof(dev));
		else if (!strcmp(key, "input_vendor_id"))
			file_vid = atoi(value);
		else if (!strcmp(key, "input_product_id"))
			file_pid = atoi(value);
		else if (!strncmp(key, "input_", 6) && (q = strrchr(key, '_')) != NULL) {
			int axis = !strcmp(q, "_axis");
			if (!axis && strcmp(q, "_btn"))
				continue;
			*q = '\0';
			for (b = 0; b < BTN_COUNT; b++)
				if (!strcmp(key + 6, cfg_key[b]) &&
						(tmp[b].type == BIND_NONE || axis == 0))
					parse_binding(value, &tmp[b], axis);
		}
	}
	fclose(fp);

	squeeze(name, want, sizeof(want));
	if (!((vid > 0 && file_vid == vid && file_pid == pid) || (dev[0] && !strcmp(dev, want))))
		return 0;
	memcpy(map, tmp, sizeof(tmp));
	return 1;
}

static void default_map(struct binding* map)
{
	static const int buttons[BTN_COUNT] = { -1, -1, -1, -1, 1, 0, 3, 2, 4, 5, 7, 6 };
	int b;
	for (b = 0; b < BTN_COUNT; b++) {
		map[b].type = buttons[b] < 0 ? BIND_NONE : BIND_BUTTON;
		map[b].index = buttons[b];
	}
	map[BTN_UP] = (struct binding){ BIND_AXIS, 1, -1 };
	map[BTN_DOWN] = (struct binding){ BIND_AXIS, 1, 1 };
	map[BTN_LEFT] = (struct binding){ BIND_AXIS, 0, -1 };
	map[BTN_RIGHT] = (struct binding){ BIND_AXIS, 0, 1 };
}

static void load_map(struct pad* p, const char* name, int vid, int pid);

static void open_pad(int device_index)
{
	struct pad* p = NULL;
	int i;
	const char* name;

	for (i = 0; i < MAX_PADS && !p; i++)
		if (!pads[i].js && !pads[i].pretend)
			p = &pads[i];
	if (!p)
		return;
	memset(p, 0, sizeof(*p));
	p->js = SDL_JoystickOpen(device_index);
	if (!p->js)
		return;
	p->id = SDL_JoystickInstanceID(p->js);
	name = SDL_JoystickName(p->js) ? SDL_JoystickName(p->js) : "";
	load_map(p, name, SDL_JoystickGetVendor(p->js), SDL_JoystickGetProduct(p->js));
}

/* Map a pad with the autoconfig file made for it, else the default layout */
static void load_map(struct pad* p, const char* name, int vid, int pid)
{
	char dir_path[SQ_PATH_LEN], file[SQ_PATH_LEN];
	struct dirent* d;
	DIR* dir;
	int found = 0;

	dir = opendir(sq_path(dir_path, sizeof(dir_path), AUTOCONFIG_DIR));
	while (dir && !found && (d = readdir(dir)) != NULL) {
		if (!sq_ends_with(d->d_name, ".cfg"))
			continue;
		snprintf(file, sizeof(file), "%s/%s", dir_path, d->d_name);
		found = load_autoconfig(file, name, vid, pid, p->map);
		if (found)
			sq_log("pad \"%s\": %s", name, d->d_name);
	}
	if (dir)
		closedir(dir);
	if (!found) {
		sq_log("pad \"%s\": no autoconfig, default layout", name);
		default_map(p->map);
	}
}

void input_open(void)
{
	int i;
	for (i = 0; i < SDL_NumJoysticks(); i++)
		open_pad(i);
}

void input_close(void)
{
	int i;
	for (i = 0; i < MAX_PADS; i++)
		if (pads[i].js)
			SDL_JoystickClose(pads[i].js);
}

/* A mapped button of a pad went down or up */
static void set_pad(struct pad* p, int b, int down)
{
	if (down != p->down[b]) {
		p->down[b] = down;
		set_held(b, down);
	}
}

/* A pad event, judged by the event's own value: the pad's current state
 * may already be past it (a quick tap's press and release can arrive
 * together, and then the press would be lost) */
static void pad_event(struct pad* p, enum bind_type type, int index, int value)
{
	int b;
	for (b = 0; b < BTN_COUNT; b++) {
		struct binding* m = &p->map[b];
		if (m->type != type || m->index != index)
			continue;
		switch (type) {
		case BIND_BUTTON: set_pad(p, b, value != 0); break;
		case BIND_AXIS: set_pad(p, b, value * m->dir > AXIS_THRESHOLD); break;
		case BIND_HAT: set_pad(p, b, (value & m->dir) != 0); break;
		default: break;
		}
	}
}

static struct pad* find_pad(SDL_JoystickID id)
{
	int i;
	for (i = 0; i < MAX_PADS; i++)
		if ((pads[i].js || pads[i].pretend) && pads[i].id == id)
			return &pads[i];
	return NULL;
}

void input_test_pad(int vid, int pid)
{
	int i;
	for (i = 0; i < MAX_PADS; i++)
		if (!pads[i].js && !pads[i].pretend) {
			memset(&pads[i], 0, sizeof(pads[i]));
			pads[i].pretend = 1;
			pads[i].id = TEST_PAD_ID;
			load_map(&pads[i], "", vid, pid);
			return;
		}
}

static int key_button(SDL_Keycode k)
{
	switch (k) {
	case SDLK_UP: return BTN_UP;
	case SDLK_DOWN: return BTN_DOWN;
	case SDLK_LEFT: return BTN_LEFT;
	case SDLK_RIGHT: return BTN_RIGHT;
	case SDLK_RETURN: case SDLK_x: return BTN_A;
	case SDLK_BACKSPACE: case SDLK_ESCAPE: case SDLK_z: return BTN_B;
	case SDLK_s: return BTN_X;
	case SDLK_a: case SDLK_f: return BTN_Y;
	case SDLK_PAGEUP: case SDLK_q: return BTN_L;
	case SDLK_PAGEDOWN: case SDLK_w: return BTN_R;
	case SDLK_SPACE: return BTN_START;
	case SDLK_TAB: return BTN_SELECT;
	default: return -1;
	}
}

void input_event(const SDL_Event* ev)
{
	struct pad* p;
	int b;

	switch (ev->type) {
	case SDL_JOYDEVICEADDED:
		if (!find_pad(SDL_JoystickGetDeviceInstanceID(ev->jdevice.which)))
			open_pad(ev->jdevice.which);
		break;
	case SDL_JOYDEVICEREMOVED:
		p = find_pad(ev->jdevice.which);
		if (p) {
			for (b = 0; b < BTN_COUNT; b++)
				if (p->down[b])
					set_held(b, 0);
			SDL_JoystickClose(p->js);
			p->js = NULL;
		}
		break;
	case SDL_JOYBUTTONDOWN:
	case SDL_JOYBUTTONUP:
		if ((p = find_pad(ev->jbutton.which)) != NULL)
			pad_event(p, BIND_BUTTON, ev->jbutton.button, ev->jbutton.state == SDL_PRESSED);
		break;
	case SDL_JOYAXISMOTION:
		if ((p = find_pad(ev->jaxis.which)) != NULL)
			pad_event(p, BIND_AXIS, ev->jaxis.axis, ev->jaxis.value);
		break;
	case SDL_JOYHATMOTION:
		if ((p = find_pad(ev->jhat.which)) != NULL)
			pad_event(p, BIND_HAT, ev->jhat.hat, ev->jhat.value);
		break;
	case SDL_KEYDOWN:
	case SDL_KEYUP:
		if (ev->key.repeat)
			break;
		b = key_button(ev->key.keysym.sym);
		if (b >= 0)
			set_held(b, ev->type == SDL_KEYDOWN);
		break;
	default:
		break;
	}
}
