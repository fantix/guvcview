/*******************************************************************************#
#           guvcview              http://guvcview.berlios.de                    #
#                                                                               #
#           Paulo Assis <pj.assis@gmail.com>                                    #
#           Nobuhiro Iwamatsu <iwamatsu@nigauri.org>                            #
#                             Add UYVY color support(Macbook iSight)            #
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

#include <portaudio.h>
#include <SDL/SDL.h>
#include <glib.h>
#include <glib/gprintf.h>

#include "defs.h"
#include "video.h"
#include "guvcview.h"
#include "v4l2uvc.h"
#include "avilib.h"
#include "colorspaces.h"
#include "video_filters.h"
#include "audio_effects.h"
#include "jpgenc.h"
#include "autofocus.h"
#include "picture.h"
#include "ms_time.h"
#include "string_utils.h"
#include "mp2.h"
#include "vcodecs.h"
#include "callbacks.h"
#include "create_video.h"
/*-------------------------------- Main Video Loop ---------------------------*/ 
/* run in a thread (SDL overlay)*/
void *main_loop(void *data)
{
	struct ALL_DATA *all_data = (struct ALL_DATA *) data;
	
	struct paRecordData *pdata = all_data->pdata;
	struct GLOBAL *global = all_data->global;
	struct focusData *AFdata = all_data->AFdata;
	struct vdIn *videoIn = all_data->videoIn;

	SDL_Event event;
	/*the main SDL surface*/
	SDL_Surface *pscreen = NULL;
	SDL_Overlay *overlay=NULL;
	SDL_Rect drect;
	const SDL_VideoInfo *info;
	char driver[128];
	
	struct JPEG_ENCODER_STRUCTURE *jpeg_struct=NULL;
	struct lavcData *lavc_data = NULL;

	struct audio_effects *aud_eff = init_audio_effects ();
	
	BYTE *p = NULL;
	BYTE *pim= NULL;
	BYTE *pvid=NULL;

	int last_focus = 0;
	if (global->AFcontrol) 
	{
		last_focus = get_focus(videoIn->fd);
		/*make sure we wait for focus to settle on first check*/
		if (last_focus < 0) last_focus=255;
	}
	
	static Uint32 SDL_VIDEO_Flags =
		SDL_ANYFORMAT | SDL_DOUBLEBUF | SDL_RESIZABLE;
 
	/*----------------------------- Test SDL capabilities ---------------------*/
	if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER) < 0) 
	{
		g_printerr("Couldn't initialize SDL: %s\n", SDL_GetError());
		exit(1);
	}
	
	/* For this version, we will use hardware acceleration as default*/
	if(global->hwaccel) 
	{
		if ( ! getenv("SDL_VIDEO_YUV_HWACCEL") ) putenv("SDL_VIDEO_YUV_HWACCEL=1");
		if ( ! getenv("SDL_VIDEO_YUV_DIRECT") ) putenv("SDL_VIDEO_YUV_DIRECT=1"); 
	}
	else 
	{
		if ( ! getenv("SDL_VIDEO_YUV_HWACCEL") ) putenv("SDL_VIDEO_YUV_HWACCEL=0");
		if ( ! getenv("SDL_VIDEO_YUV_DIRECT") ) putenv("SDL_VIDEO_YUV_DIRECT=0"); 
	}
	 
	if (SDL_VideoDriverName(driver, sizeof(driver)) && global->debug) 
	{
		g_printf("Video driver: %s\n", driver);
	}
	
	info = SDL_GetVideoInfo();

	if (info->wm_available && global->debug) g_printf("A window manager is available\n");

	if (info->hw_available) 
	{
		if (global->debug) 
			g_printf("Hardware surfaces are available (%dK video memory)\n", info->video_mem);

		SDL_VIDEO_Flags |= SDL_HWSURFACE;
	}
	if (info->blit_hw) 
	{
		if (global->debug) g_printf("Copy blits between hardware surfaces are accelerated\n");

		SDL_VIDEO_Flags |= SDL_ASYNCBLIT;
	}
	
	if (global->debug) 
	{
		if (info->blit_hw_CC) g_printf ("Colorkey blits between hardware surfaces are accelerated\n");
		if (info->blit_hw_A) g_printf("Alpha blits between hardware surfaces are accelerated\n");
		if (info->blit_sw) g_printf ("Copy blits from software surfaces to hardware surfaces are accelerated\n");
		if (info->blit_sw_CC) g_printf ("Colorkey blits from software surfaces to hardware surfaces are accelerated\n");
		if (info->blit_sw_A) g_printf("Alpha blits from software surfaces to hardware surfaces are accelerated\n");
		if (info->blit_fill) g_printf("Color fills on hardware surfaces are accelerated\n");
	}

	if (!(SDL_VIDEO_Flags & SDL_HWSURFACE))
	{
		SDL_VIDEO_Flags |= SDL_SWSURFACE;
	}

	SDL_WM_SetCaption(global->WVcaption, NULL); 

	/* enable key repeat */
	SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY,SDL_DEFAULT_REPEAT_INTERVAL);
	 
	/*------------------------------ SDL init video ---------------------*/
	pscreen = SDL_SetVideoMode( videoIn->width, 
		videoIn->height, 
		global->bpp,
		SDL_VIDEO_Flags);
		 
	overlay = SDL_CreateYUVOverlay(videoIn->width, videoIn->height,
		SDL_YUY2_OVERLAY, pscreen);
	
	p = (unsigned char *) overlay->pixels[0];
	
	drect.x = 0;
	drect.y = 0;
	drect.w = pscreen->w;
	drect.h = pscreen->h;
	
	
	while (videoIn->signalquit) 
	{
		/*-------------------------- Grab Frame ----------------------------------*/
		if (uvcGrab(videoIn) < 0) 
		{
			g_printerr("Error grabbing image \n");
			videoIn->signalquit=0;
			g_snprintf(global->WVcaption,20,"GUVCVideo - CRASHED");
			SDL_WM_SetCaption(global->WVcaption, NULL);
			g_thread_exit((void *) -2);
		} 
		else 
		{
			/*reset video start time to first frame capture time */  
			if(global->framecount < 2) 
			{
				global->Vidstarttime = ns_time(); //nanoseconds
				global->v_ts = 0;
			}
			else
			{
				global->v_ts = ns_time() - global->Vidstarttime; //nanoseconds
				//printf("start: %lu, timestamp: %llu\n",(unsigned long) global->Vidstarttime, global->v_ts);
			}

			if (global->FpsCount) 
			{/* sets fps count in window title bar */
				global->frmCount++;
				if (global->DispFps>0) 
				{ /*set every 2 sec*/
					g_snprintf(global->WVcaption,24,"GUVCVideo - %3.2f fps",global->DispFps);
					SDL_WM_SetCaption(global->WVcaption, NULL);
					global->frmCount=0;/*resets*/
					global->DispFps=0;
				}
			}
	
			/*---------------- autofocus control ------------------*/
		
			if (global->AFcontrol && (global->autofocus || AFdata->setFocus)) 
			{ /*AFdata = NULL if no focus control*/
				if (AFdata->focus < 0) 
				{
					/*starting autofocus*/
					AFdata->focus = AFdata->left; /*start left*/
					if (set_focus (videoIn->fd, AFdata->focus) != 0) 
						g_printerr("ERROR: couldn't set focus to %d\n", AFdata->focus);
					/*number of frames until focus is stable*/
					/*1.4 ms focus time - every 1 step*/
					AFdata->focus_wait = (int) abs(AFdata->focus-last_focus)*1.4/(1000/videoIn->fps)+1;
					last_focus = AFdata->focus;
				} 
				else 
				{
					if (AFdata->focus_wait == 0) 
					{
						AFdata->sharpness=getSharpness (videoIn->framebuffer, videoIn->width, 
							videoIn->height, 5);
						if (global->debug) 
							g_printf("sharp=%d focus_sharp=%d foc=%d right=%d left=%d ind=%d flag=%d\n",
								AFdata->sharpness,AFdata->focus_sharpness,
								AFdata->focus, AFdata->right, AFdata->left, 
								AFdata->ind, AFdata->flag);
						AFdata->focus=getFocusVal (AFdata);
						if ((AFdata->focus != last_focus)) 
						{
							if (set_focus (videoIn->fd, AFdata->focus) != 0) 
								g_printerr("ERROR: couldn't set focus to %d\n", 
									AFdata->focus);
							/*number of frames until focus is stable*/
							/*1.4 ms focus time - every 1 step*/
							AFdata->focus_wait = (int) abs(AFdata->focus-last_focus)*1.4/(1000/videoIn->fps)+1;
						}
						last_focus = AFdata->focus;
					} 
					else 
					{
						AFdata->focus_wait--;
						if (global->debug) g_printf("Wait Frame: %d\n",AFdata->focus_wait);
					}
				}
			}
		}
		/*------------------------- Filter Frame ---------------------------------*/
		g_mutex_lock(global->mutex);
		if(global->Frame_Flags>0)
		{
			if((global->Frame_Flags & YUV_MIRROR)==YUV_MIRROR) 
				yuyv_mirror(videoIn->framebuffer,videoIn->width,videoIn->height);
			
			if((global->Frame_Flags & YUV_UPTURN)==YUV_UPTURN)
				yuyv_upturn(videoIn->framebuffer,videoIn->width,videoIn->height);
				
			if((global->Frame_Flags & YUV_NEGATE)==YUV_NEGATE)
				yuyv_negative (videoIn->framebuffer,videoIn->width,videoIn->height);
				
			if((global->Frame_Flags & YUV_MONOCR)==YUV_MONOCR) 
				yuyv_monochrome (videoIn->framebuffer,videoIn->width,videoIn->height);
		   
			if((global->Frame_Flags & YUV_PIECES)==YUV_PIECES)
				pieces (videoIn->framebuffer, videoIn->width, videoIn->height, 16 );
			
		}
		g_mutex_unlock(global->mutex);
		/*-------------------------capture Image----------------------------------*/
		if (videoIn->capImage)
		{
			switch(global->imgFormat) 
			{
				case 0:/*jpg*/
					/* Save directly from MJPG frame */
					if((global->Frame_Flags==0) && (videoIn->formatIn==V4L2_PIX_FMT_MJPEG)) 
					{
						if(SaveJPG(videoIn->ImageFName,videoIn->buf.bytesused,videoIn->tmpbuffer)) 
						{
							g_printerr ("Error: Couldn't capture Image to %s \n",
								videoIn->ImageFName);
						}
					} 
					else if ((global->Frame_Flags==0) && (videoIn->formatIn==V4L2_PIX_FMT_JPEG))
					{
						if (SaveBuff(videoIn->ImageFName,videoIn->buf.bytesused,videoIn->tmpbuffer))
						{
							g_printerr ("Error: Couldn't capture Image to %s \n",
								videoIn->ImageFName);
						}
					}
					else 
					{ /* use built in encoder */
						if (!global->jpeg)
						{ 
							global->jpeg = g_new0(BYTE, global->jpeg_bufsize);
						}
						if(!jpeg_struct) 
						{
							jpeg_struct = g_new0(struct JPEG_ENCODER_STRUCTURE, 1);
							
							/* Initialization of JPEG control structure */
							initialization (jpeg_struct,videoIn->width,videoIn->height);
	
							/* Initialization of Quantization Tables  */
							initialize_quantization_tables (jpeg_struct);
						} 

						global->jpeg_size = encode_image(videoIn->framebuffer, global->jpeg, 
							jpeg_struct,1, videoIn->width, videoIn->height);
							
						if(SaveBuff(videoIn->ImageFName,global->jpeg_size,global->jpeg)) 
						{ 
							g_printerr ("Error: Couldn't capture Image to %s \n",
							videoIn->ImageFName);
						}
					}
					break;

				case 1:/*bmp*/
					if(pim==NULL) 
					{  
						/*24 bits -> 3bytes     32 bits ->4 bytes*/
						pim = g_new0(BYTE, (pscreen->w)*(pscreen->h)*3);
					}
			
					yuyv2bgr(videoIn->framebuffer,pim,videoIn->width,videoIn->height);
					
			
					if(SaveBPM(videoIn->ImageFName, videoIn->width, videoIn->height, 24, pim)) 
					{
						g_printerr ("Error: Couldn't capture Image to %s \n",
						videoIn->ImageFName);
					} 
					break;
					
				case 2:/*png*/
					if(pim==NULL) 
					{  
						/*24 bits -> 3bytes     32 bits ->4 bytes*/
						pim = g_new0(BYTE, (pscreen->w)*(pscreen->h)*3);
					}
			
					yuyv2rgb(videoIn->framebuffer,pim,videoIn->width,videoIn->height);
					
					write_png(videoIn->ImageFName, videoIn->width, videoIn->height,pim);
			}
			videoIn->capImage=FALSE;
			if (global->debug) g_printf("saved image to:%s\n",videoIn->ImageFName);
		}
		/*---------------------------capture Video---------------------------------*/
		if (videoIn->capVid)
		{
			videoIn->VidCapStop = FALSE;
			write_video_frame(all_data, (void *) &(jpeg_struct), (void *) &(lavc_data), (void *) &(pvid));
			/*----------------------- add audio -----------------------------*/
			if ((global->Sound_enable) && (pdata->audio_flag>0)) 
			{
				g_mutex_lock( pdata->mutex );
					sync_audio_frame(all_data);
					//if(global->debug) g_printf("audio: %lu frames per buffer and %d total samples\n",
					//	pdata->framesPerBuffer, pdata->numSamples);
					/*run effects on data*/
					/*echo*/
					if((pdata->snd_Flags & SND_ECHO)==SND_ECHO) 
					{
						Echo(pdata, aud_eff, 300, 0.5);
					}
					else
					{
						close_DELAY(aud_eff->ECHO);
						aud_eff->ECHO = NULL;
					}
					/*fuzz*/
					if((pdata->snd_Flags & SND_FUZZ)==SND_FUZZ) 
					{
						Fuzz(pdata, aud_eff);
					}
					else
					{
						close_FILT(aud_eff->HPF);
						aud_eff->HPF = NULL;
					}
					/*reverb*/
					if((pdata->snd_Flags & SND_REVERB)==SND_REVERB) 
					{
						Reverb(pdata, aud_eff, 50);
					}
					else
					{
						close_REVERB(aud_eff);
					}
					/*wahwah*/
					if((pdata->snd_Flags & SND_WAHWAH)==SND_WAHWAH) 
					{
						WahWah (pdata, aud_eff, 1.5, 0, 0.7, 0.3, 2.5);
					}
					else
					{
						close_WAHWAH(aud_eff->wahData);
						aud_eff->wahData = NULL;
					}
					/*Ducky*/
					if((pdata->snd_Flags & SND_DUCKY)==SND_DUCKY) 
					{
						change_pitch(pdata, aud_eff, 2);
					}
					else
					{
						close_pitch (aud_eff);
					}
				g_mutex_unlock( pdata->mutex );
				
				write_audio_frame(all_data);
			}
		} /*video and audio capture have stopped */
		else
		{
			if(lavc_data != NULL)
			{
				int nf = clean_lavc(&(lavc_data));
				if(global->debug) g_printf(" total frames: %d  -- encoded: %d\n", global->framecount, nf);
				//if (global->framecount > nf)
				//	global->framecount = nf;
			}
	
			videoIn->VidCapStop=TRUE;
		}
	
		/*------------------------- Display Frame --------------------------------*/
		SDL_LockYUVOverlay(overlay);
		memcpy(p, videoIn->framebuffer,
			videoIn->width * (videoIn->height) * 2);
		SDL_UnlockYUVOverlay(overlay);
		SDL_DisplayYUVOverlay(overlay, &drect);
	
		/*sleep for a while*/
		if(global->vid_sleep)
			SDL_Delay(global->vid_sleep);
		
		/*------------------------- Read Key events ------------------------------*/
		if (videoIn->PanTilt) 
		{
			/* Poll for events */
			while( SDL_PollEvent(&event) )
			{
				if(event.type==SDL_KEYDOWN) 
				{
					switch( event.key.keysym.sym )
					{
						/* Keyboard event */
						/* Pass the event data onto PrintKeyInfo() */
						case SDLK_DOWN:
							/*Tilt Down*/
							uvcPanTilt (videoIn->fd,0,INCPANTILT*(global->TiltStep),0);
							break;
							
						case SDLK_UP:
							/*Tilt UP*/
							uvcPanTilt (videoIn->fd,0,-INCPANTILT*(global->TiltStep),0);
							break;
							
						case SDLK_LEFT:
							/*Pan Left*/
							uvcPanTilt (videoIn->fd,-INCPANTILT*(global->PanStep),0,0);
							break;
							
						case SDLK_RIGHT:
							/*Pan Right*/
							uvcPanTilt (videoIn->fd,INCPANTILT*(global->PanStep),0,0);
							break;
						default:
							break;
					}
				}

			}
		}

	}/*loop end*/

	if(lavc_data != NULL)
	{
		int nf = clean_lavc(&lavc_data);
		if(global->debug) g_printf(" total frames: %d  -- encoded: %d\n", global->framecount, nf);
		lavc_data = NULL;
	}
	/*check if thread exited while in Video capture mode*/
	if (videoIn->capVid) 
	{
		/*stop capture*/
		global->Vidstoptime = ms_time();
		videoIn->VidCapStop=TRUE;
		videoIn->capVid = FALSE;
		pdata->capVid = videoIn->capVid;
		if (global->debug) g_printf("stoping Video capture\n");
		closeVideoFile(all_data);
	}
	
	if (global->debug) g_printf("Thread terminated...\n");
	p = NULL;
	g_free(jpeg_struct);
	jpeg_struct=NULL;
	g_free(pim);
	pim=NULL;
	g_free(pvid);
	pvid=NULL;
	if (global->debug) g_printf("cleaning Thread allocations: 100%%\n");
	fflush(NULL);//flush all output buffers 
	
	close_audio_effects (aud_eff);
	SDL_FreeYUVOverlay(overlay);
	SDL_Quit();   

	if (global->debug) g_printf("SDL Quit\n");

	global = NULL;
	AFdata = NULL;
	videoIn = NULL;
	return ((void *) 0);
}



