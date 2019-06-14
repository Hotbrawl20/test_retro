/*
Copyright (c) 2012, Broadcom Europe Ltd
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// A simple demo using dispmanx to display get screenshot

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rfb/rfb.h>
#include <rfb/keysym.h>

#include <signal.h>
#include <setjmp.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/time.h>
#include <unistd.h>
#include "bcm_host.h"
#include <assert.h>

#define BPP      2

/* 15 frames per second (if we can) */
#define PICTURE_TIMEOUT (1.0/15.0)

DISPMANX_DISPLAY_HANDLE_T display;
DISPMANX_RESOURCE_HANDLE_T resource;
DISPMANX_MODEINFO_T info;
void *image;
void *back_image;

int r_x0, r_y0, r_x1, r_y1;

/* for compatibility of non-android systems */
#ifndef ANDROID
#define KEY_SOFT1 KEY_UNKNOWN
#define KEY_SOFT2 KEY_UNKNOWN
#define KEY_CENTER	KEY_UNKNOWN
#define KEY_SHARP KEY_UNKNOWN
#define KEY_STAR KEY_UNKNOWN
#endif

jmp_buf env;
int ufile;

/*
 * throttle camera updates
*/
int TimeToTakePicture()
{
	static struct timeval now = { 0, 0 }, then = {
	0, 0};
	double elapsed, dnow, dthen;

	gettimeofday(&now, NULL);

	dnow = now.tv_sec + (now.tv_usec / 1000000.0);
	dthen = then.tv_sec + (then.tv_usec / 1000000.0);
	elapsed = dnow - dthen;

	if (elapsed > PICTURE_TIMEOUT)
		memcpy((char *)&then, (char *)&now, sizeof(struct timeval));
	return elapsed > PICTURE_TIMEOUT;
}

/*
 * simulate grabbing a picture from some device
 */
int TakePicture(unsigned char *buffer)
{
	static int last_line = 0, fps = 0, fcount = 0;
	int line = 0;
	int i, j;
	struct timeval now;
	int found;

	VC_IMAGE_TRANSFORM_T transform = 0;
	VC_RECT_T rect;

	vc_dispmanx_snapshot(display, resource, transform);

	vc_dispmanx_rect_set(&rect, 0, 0, info.width, info.height);
	vc_dispmanx_resource_read_data(resource, &rect, image, info.width * 2);

	unsigned short *image_p = (unsigned short *)image;
	unsigned short *buffer_p = (unsigned short *)buffer;

	// find y0, y1
	found = 0;
	unsigned short *back_image_p = (unsigned short *)back_image;
	for (i = 0; i < info.height && !found; i++) {
		for (j = 0; j < info.width; j++) {
			if (back_image_p[i * info.width + j] !=
			    image_p[i * info.width + j]) {
				r_y0 = i;
				found = 1;
				break;
			}
		}
	}

	found = 0;
	for (i = info.height - 1; i >= r_y0 && !found; i--) {
		for (j = 0; j < info.width; j++) {
			if (back_image_p[i * info.width + j] !=
			    image_p[i * info.width + j]) {
				r_y1 = i + 1;
				found = 1;
				break;
			}
		}
	}

	found = 0;
	for (i = 0; i < info.width && !found; i++) {
		for (j = r_y0; j < r_y1; j++) {
			if (back_image_p[j * info.width + i] !=
			    image_p[j * info.width + i]) {
				r_x0 = i;
				found = 1;
				break;
			}
		}
	}

	found = 0;
	for (i = info.width - 1; i >= r_x0 && !found; i--) {
		for (j = r_y0; j < r_y1; j++) {
			if (back_image_p[j * info.width + i] !=
			    image_p[j * info.width + i]) {
				r_x1 = i + 1;
				found = 1;
				break;
			}
		}
	}

	for (j = r_y0; j < r_y1; ++j) {
		for (i = r_x0; i < r_x1; ++i) {
			unsigned short tbi = image_p[j * info.width + i];

			unsigned short R5 = (tbi >> 11);
			unsigned short G5 = ((tbi >> 6) & 0x1f);
			unsigned short B5 = tbi & 0x1f;

			tbi = (B5 << 10) | (G5 << 5) | R5;

			buffer_p[j * info.width + i] = tbi;
		}
	}

	memcpy(back_image, image, info.width * info.height * 2);

	/*
	 * simulate the passage of time
	 *
	 * draw a simple black line that moves down the screen. The faster the
	 * client, the more updates it will get, the smoother it will look!
	 */
	gettimeofday(&now, NULL);
	line = now.tv_usec / (1000000 / info.height);
	if (line > info.height)
		line = info.height - 1;
	//memset(&buffer[(info.width * BPP) * line], 0, (info.width * BPP));
	/* frames per second (informational only) */
	fcount++;
	if (last_line > line) {
		fps = fcount;
		fcount = 0;
	}
	last_line = line;
	fprintf(stderr, "%03d/%03d Picture (%03d fps) ", line, info.height,
		fps);

	fprintf(stderr, "x0=%d, y0=%d, x1=%d, y1=%d              \r", r_x0,
		r_y0, r_x1, r_y1);
	/* success!   We have a new picture! */
	return (1 == 1);
}

void sig_handler(int signo)
{
	longjmp(env, 1);
}

void initUinput()
{
	struct uinput_user_dev uinp;
	int retcode, i;

	ufile = open("/dev/uinput", O_WRONLY | O_NDELAY);
	printf("open /dev/uinput returned %d.\n", ufile);
	if (ufile == 0) {
		printf("Could not open uinput.\n");
		exit(-1);
	}

	memset(&uinp, 0, sizeof(uinp));
	strncpy(uinp.name, "VNCServer SimKey", 20);
	uinp.id.version = 4;
	uinp.id.bustype = BUS_USB;

	ioctl(ufile, UI_SET_EVBIT, EV_KEY);

	for (i = 0; i < KEY_MAX; i++) {	//I believe this is to tell UINPUT what keys we can make?
		ioctl(ufile, UI_SET_KEYBIT, i);
	}

	retcode = write(ufile, &uinp, sizeof(uinp));
	printf("First write returned %d.\n", retcode);

	retcode = (ioctl(ufile, UI_DEV_CREATE));
	printf("ioctl UI_DEV_CREATE returned %d.\n", retcode);
	if (retcode) {
		printf("Error create uinput device %d.\n", retcode);
		exit(-1);
	}
}

static int keysym2scancode(rfbKeySym key)
{
	//printf("keysym: %04X\n", key);

	int scancode = 0;

	int code = (int)key;
	if (code >= '0' && code <= '9') {
		scancode = (code & 0xF) - 1;
		if (scancode < 0)
			scancode += 10;
		scancode += KEY_1;
	} else if (code >= 0xFF50 && code <= 0xFF58) {
		static const uint16_t map[] =
		    { KEY_HOME, KEY_LEFT, KEY_UP, KEY_RIGHT, KEY_DOWN,
			KEY_PAGEUP, KEY_PAGEDOWN, KEY_END, 0
		};
		scancode = map[code & 0xF];
	} else if (code >= 0xFFE1 && code <= 0xFFEE) {
		static const uint16_t map[] = { KEY_LEFTSHIFT, KEY_LEFTSHIFT,
			KEY_LEFTCTRL, KEY_LEFTCTRL,
			KEY_LEFTSHIFT, KEY_LEFTSHIFT,
			0, 0,
			KEY_LEFTALT, KEY_RIGHTALT,
			0, 0, 0, 0
		};
		scancode = map[code & 0xF];
	} else if ((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z')) {
		static const uint16_t map[] = {
			KEY_A, KEY_B, KEY_C, KEY_D, KEY_E,
			KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
			KEY_K, KEY_L, KEY_M, KEY_N, KEY_O,
			KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
			KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z
		};
		scancode = map[(code & 0x5F) - 'A'];
	} else {
		switch (code) {
		case XK_space:
			scancode = KEY_SPACE;
			break;

		case XK_exclam:
			scancode = KEY_1;
			break;
		case XK_at:
			scancode = KEY_2;
			break;
		case XK_numbersign:
			scancode = KEY_3;
			break;
		case XK_dollar:
			scancode = KEY_4;
			break;
		case XK_percent:
			scancode = KEY_5;
			break;
		case XK_asciicircum:
			scancode = KEY_6;
			break;
		case XK_ampersand:
			scancode = KEY_7;
			break;
		case XK_asterisk:
			scancode = KEY_8;
			break;
		case XK_parenleft:
			scancode = KEY_9;
			break;
		case XK_parenright:
			scancode = KEY_0;
			break;
		case XK_minus:
			scancode = KEY_MINUS;
			break;
		case XK_underscore:
			scancode = KEY_MINUS;
			break;
		case XK_equal:
			scancode = KEY_EQUAL;
			break;
		case XK_plus:
			scancode = KEY_EQUAL;
			break;
		case XK_BackSpace:
			scancode = KEY_BACKSPACE;
			break;
		case XK_Tab:
			scancode = KEY_TAB;
			break;

		case XK_braceleft:
			scancode = KEY_LEFTBRACE;
			break;
		case XK_braceright:
			scancode = KEY_RIGHTBRACE;
			break;
		case XK_bracketleft:
			scancode = KEY_LEFTBRACE;
			break;
		case XK_bracketright:
			scancode = KEY_RIGHTBRACE;
			break;
		case XK_Return:
			scancode = KEY_ENTER;
			break;

		case XK_semicolon:
			scancode = KEY_SEMICOLON;
			break;
		case XK_colon:
			scancode = KEY_SEMICOLON;
			break;
		case XK_apostrophe:
			scancode = KEY_APOSTROPHE;
			break;
		case XK_quotedbl:
			scancode = KEY_APOSTROPHE;
			break;
		case XK_grave:
			scancode = KEY_GRAVE;
			break;
		case XK_asciitilde:
			scancode = KEY_GRAVE;
			break;
		case XK_backslash:
			scancode = KEY_BACKSLASH;
			break;
		case XK_bar:
			scancode = KEY_BACKSLASH;
			break;

		case XK_comma:
			scancode = KEY_COMMA;
			break;
		case XK_less:
			scancode = KEY_COMMA;
			break;
		case XK_period:
			scancode = KEY_DOT;
			break;
		case XK_greater:
			scancode = KEY_DOT;
			break;
		case XK_slash:
			scancode = KEY_SLASH;
			break;
		case XK_question:
			scancode = KEY_SLASH;
			break;
		case XK_Caps_Lock:
			scancode = KEY_CAPSLOCK;
			break;

		case XK_F1:
			scancode = KEY_F1;
			break;
		case XK_F2:
			scancode = KEY_F2;
			break;
		case XK_F3:
			scancode = KEY_F3;
			break;
		case XK_F4:
			scancode = KEY_F4;
			break;
		case XK_F5:
			scancode = KEY_F5;
			break;
		case XK_F6:
			scancode = KEY_F6;
			break;
		case XK_F7:
			scancode = KEY_F7;
			break;
		case XK_F8:
			scancode = KEY_F8;
			break;
		case XK_F9:
			scancode = KEY_F9;
			break;
		case XK_F10:
			scancode = KEY_F10;
			break;
		case XK_Num_Lock:
			scancode = KEY_NUMLOCK;
			break;
		case XK_Scroll_Lock:
			scancode = KEY_SCROLLLOCK;
			break;

		case XK_Page_Down:
			scancode = KEY_PAGEDOWN;
			break;
		case XK_Insert:
			scancode = KEY_INSERT;
			break;
		case XK_Delete:
			scancode = KEY_DELETE;
			break;
		case XK_Page_Up:
			scancode = KEY_PAGEUP;
			break;
		case XK_Escape:
			scancode = KEY_ESC;
			break;

		case 0x0003:
			scancode = KEY_CENTER;
			break;
		}
	}

	return scancode;
}

static void dokey(rfbBool down, rfbKeySym key, rfbClientPtr cl)
{

	struct input_event event;

	if (down) {

		memset(&event, 0, sizeof(event));
		gettimeofday(&event.time, NULL);
		event.type = EV_KEY;
		event.code = keysym2scancode(key);	//nomodifiers!
		event.value = 1;	//key pressed
		write(ufile, &event, sizeof(event));

		memset(&event, 0, sizeof(event));
		gettimeofday(&event.time, NULL);
		event.type = EV_SYN;
		event.code = SYN_REPORT;	//not sure what this is for? i'm guessing its some kind of sync thing?
		event.value = 0;
		write(ufile, &event, sizeof(event));

	} else {
		memset(&event, 0, sizeof(event));
		gettimeofday(&event.time, NULL);
		event.type = EV_KEY;
		event.code = keysym2scancode(key);	//nomodifiers!
		event.value = 0;	//key realeased
		write(ufile, &event, sizeof(event));

		memset(&event, 0, sizeof(event));
		gettimeofday(&event.time, NULL);
		event.type = EV_SYN;
		event.code = SYN_REPORT;	//not sure what this is for? i'm guessing its some kind of sync thing?
		event.value = 0;
		write(ufile, &event, sizeof(event));

	}
}

int main(int argc, char *argv[])
{
	VC_IMAGE_TYPE_T type = VC_IMAGE_RGB565;

	uint32_t vc_image_ptr;

	int ret, end;
	long usec;

	uint32_t screen = 0;

	if (signal(SIGINT, sig_handler) == SIG_ERR) {
		fprintf(stderr, "error setting sighandler\n");
		exit(-1);
	}

	bcm_host_init();

	printf("Open display[%i]...\n", screen);
	display = vc_dispmanx_display_open(screen);

	ret = vc_dispmanx_display_get_info(display, &info);
	assert(ret == 0);
	printf("Display is %d x %d\n", info.width, info.height);

	image = calloc(1, info.width * 2 * info.height);

	assert(image);

	back_image = calloc(1, info.width * 2 * info.height);

	assert(back_image);

	r_x0 = r_y0 = 0;
	r_x1 = info.width;
	r_y1 = info.height;
	resource = vc_dispmanx_resource_create(type,
					       info.width,
					       info.height, &vc_image_ptr);

	rfbScreenInfoPtr server =
	    rfbGetScreen(&argc, argv, info.width, info.height, 5, 3, BPP);
	if (!server)
		return 0;
	server->desktopName = "Live Video Feed Example";
	server->frameBuffer = (char *)malloc(info.width * info.height * BPP);
	server->alwaysShared = (1 == 1);
	server->kbdAddEvent = dokey;

	printf("Server bpp:%d\n", server->serverFormat.bitsPerPixel);
	printf("Server bigEndian:%d\n", server->serverFormat.bigEndian);
	printf("Server redShift:%d\n", server->serverFormat.redShift);
	printf("Server blueShift:%d\n", server->serverFormat.blueShift);
	printf("Server greeShift:%d\n", server->serverFormat.greenShift);

	/* Initialize the server */
	rfbInitServer(server);

	end = setjmp(env);
	if (end != 0)
		goto end;

	initUinput();

	/* Loop, processing clients and taking pictures */
	while (rfbIsActive(server)) {
		if (TimeToTakePicture())
			if (TakePicture((unsigned char *)server->frameBuffer))
				rfbMarkRectAsModified(server, r_x0, r_y0, r_x1,
						      r_y1);

		usec = server->deferUpdateTime * 1000;
		rfbProcessEvents(server, usec);
	}

 end:

	// destroy the device
	ioctl(ufile, UI_DEV_DESTROY);
	close(ufile);

	ret = vc_dispmanx_resource_delete(resource);
	assert(ret == 0);
	ret = vc_dispmanx_display_close(display);
	assert(ret == 0);
	printf("\nDone\n");

	return 0;

}
