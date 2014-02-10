/*******************************************************************************#
#           guvcview              http://guvcview.sourceforge.net               #
#                                                                               #
#           Paulo Assis <pj.assis@gmail.com>                                    #
#                                                                               #
# This program is free software; you can redistribute it and/or modify          #
# it under the terms of the GNU General Public License as published by          #
# the Free Software Foundation; either version 2 of the License, or             #
# (at your option) any later version.                                           #
#                                                                               #
# This program is distributed in the hope that it will be useful,               #
# but WITHOUT ANY WARRANTY; without even the implied warranty of                #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                 #
# GNU General Public License for more details.                                  #
#                                                                               #
# You should have received a copy of the GNU General Public License             #
# along with this program; if not, write to the Free Software                   #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA     #
#                                                                               #
********************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "gview.h"
#include "gviewv4l2core.h"
#include "gviewrender.h"
#include "core_time.h"

#include "video_capture.h"
#include "options.h"
#include "gui.h"

int debug_level = 0;

static __THREAD_TYPE capture_thread;

/*
 * signal callback
 * args:
 *    signum - signal number
 *
 * return: none
 */
void signal_callback_handler(int signum)
{
	printf("GUVCVIEW Caught signal %d\n", signum);

	switch(signum)
	{
		case SIGINT:
			/* Terminate program */
			quit_callback(NULL);
			break;

		case SIGUSR1:
			/* (start/stop) record video */
			break;

		case SIGUSR2:
			/* save image */
			video_capture_save_image();
			break;
	}
}

int main(int argc, char *argv[])
{

	// Register signal and signal handler
	signal(SIGINT,  signal_callback_handler);
	signal(SIGUSR1, signal_callback_handler);
	signal(SIGUSR2, signal_callback_handler);

	if(options_parse(argc, argv))
		return 0;

	options_t *my_options = options_get();

	debug_level = my_options->verbosity;
	set_v4l2_verbosity(debug_level);

	v4l2_dev_t *device = init_v4l2_dev(my_options->device);

	/*select render API*/
	int render = RENDER_SDL1;

	if(strcmp(my_options->render, "none") == 0)
		render = RENDER_NONE;
	else if(strcmp(my_options->render, "sdl1") == 0)
		render = RENDER_SDL1;
	else
		render = RENDER_SDL1;

	int gui = GUI_GTK3;

	if(strcmp(my_options->gui, "none") == 0)
		gui = GUI_NONE;
	else if(strcmp(my_options->render, "gtk3") == 0)
		gui = GUI_GTK3;
	else
		gui = GUI_GTK3;

	if(device)
		set_render_flag(render);
	else
		return -1;

	capture_loop_data_t cl_data;
	cl_data.options = (void *) my_options;
	cl_data.device = (void *) device;

	int ret = __THREAD_CREATE(&capture_thread, capture_loop, (void *) &cl_data);

	if(ret)
		fprintf(stderr, "GUVCVIEW: Video thread creation failed\n");


	gui_attach(device, gui, 800, 600);

	/*run the gui loop*/
	gui_run();

	__THREAD_JOIN(capture_thread);

	if(device)
		close_v4l2_dev(device);

	if(debug_level > 0)
		printf("GUVCVIEW: good bye\n");
	return 0;
}
