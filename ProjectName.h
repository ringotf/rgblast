
#pragma once


#define sms_clock_pal_khz 53203
#define sms_clock_ntsc_khz 53693

#define pixels_in_scanline 360 //250 //256 

#define scanlines_in_active_area  310 //190 //193  

#define sms_v_lines_to_skip 70

#define sms_pixel_width 256 //250 //256
#define sms_pixel_height 192 //190 //192
//#define sms_pixel_x_offset 0 
#define sms_pixel_x_offset_dvi 0 


#ifndef DVI_VERTICAL_REPEAT_SMS
#define DVI_VERTICAL_REPEAT_SMS 2
#endif