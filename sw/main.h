#pragma once


#define sms_clock_pal_khz 53203.0f
#define sms_clock_ntsc_khz 53693.0f

#ifdef VIDEO_MODE_NTSC
#define sms_clock_khz sms_clock_ntsc_khz
#endif
#ifdef VIDEO_MODE_PAL
#define sms_clock_khz sms_clock_pal_khz
#endif
#ifdef VIDEO_MODE_PAL60
#define sms_clock_khz sms_clock_pal_khz
#endif

#define pixels_in_scanline 300 //250 //256 

#define scanlines_in_active_area  300 //190 //193  

#define sms_v_lines_to_skip_pal 45 //8 //89
#define sms_v_lines_to_skip_ntsc 18 //"ntsc" mode on a pal sms???

#ifdef VIDEO_MODE_NTSC
#define sms_v_lines_to_skip sms_v_lines_to_skip_ntsc
#endif
#ifdef VIDEO_MODE_PAL
#define sms_v_lines_to_skip sms_v_lines_to_skip_pal
#endif
#ifdef VIDEO_MODE_PAL60
#define sms_v_lines_to_skip sms_v_lines_to_skip_ntsc
#endif

#define sms_pixel_width 256 //256 //256
#define sms_pixel_height 192 //190 //192
//#define sms_pixel_x_offset 0 
#define sms_pixel_x_offset_dvi 37 

#ifndef DVI_VERTICAL_REPEAT_SMS
#define DVI_VERTICAL_REPEAT_SMS 2
#endif

//adc audio capture
#define sms_audio_pin 26
