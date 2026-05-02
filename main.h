
#pragma once


#define sms_clock_pal_khz 53203
#define sms_clock_ntsc_khz 53693

#ifdef VIDEO_MODE_PAL
#define sms_clock_khz sms_clock_pal_khz
#else
#define sms_clock_khz sms_clock_ntsc_khz
#endif

#define pixels_in_scanline 280 //250 //256 

#define scanlines_in_active_area  310 //190 //193  


#define sms_v_lines_to_skip_pal 69
#define sms_v_lines_to_skip_ntsc 42 //"ntsc" mode on a pal sms???

#ifdef VIDEO_MODE_PAL
#define sms_v_lines_to_skip sms_v_lines_to_skip_pal
#else
#define sms_v_lines_to_skip sms_v_lines_to_skip_ntsc
#endif


#define sms_pixel_width 256 //250 //256
#define sms_pixel_height 192 //190 //192
//#define sms_pixel_x_offset 0 
#define sms_pixel_x_offset_dvi 26 


#ifndef DVI_VERTICAL_REPEAT_SMS
#define DVI_VERTICAL_REPEAT_SMS 2
#endif


//adc audio capture
#define sms_audio_pin 26


void setup_ws2812_led();