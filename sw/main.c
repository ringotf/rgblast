#include <stdio.h>
#include <string.h>

#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "pico/flash.h"

#include "hardware/adc.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"

#include "vsync_detector.pio.h"
#include "pio_6bpp_color_read.pio.h"
//#include "flash_adc.pio.h"

#include "libdvi/dvi.h"
#include "libdvi/dvi_serialiser.h"
#include "libdvi/dvi_timing.h"

#include "main.h"


#define IMAGE_SIZE_PIXELS (pixels_in_scanline*scanlines_in_active_area)
#define PIXELS_PER_WORD 1 //5 // 5px*6bpp+2bits
#define IMAGE_SIZE_WORDS (IMAGE_SIZE_PIXELS/PIXELS_PER_WORD)
#define IMAGE_SIZE_BYTES (IMAGE_SIZE_WORDS) //(IMAGE_SIZE_WORDS*4)


__attribute__((aligned(0x8)))
 unsigned char TWO_BIT_LUT[8] = {
    0b00,
    0b01,
    0b10,
    0b10,
    0b11,
    0b11,
    0b11,
    0b11
};

__attribute__((aligned(0x8)))
 unsigned char ONE_BIT_LUT[8] = {
    0b00,
    0b10,
    0b10,
    0b11,
};

__attribute__((aligned(0x200)))
unsigned char SIX_BIT_LUT[512];


void generate_lut()
{
    printf("%s: Generating Lut...\n", __func__);

    for(uint16_t r = 0; r < 8; r++)
    { 
        for(uint16_t g = 0; g < 8; g++)
        {
            for(uint16_t b = 0; b < 8; b++)
            {

                uint16_t r_value = TWO_BIT_LUT[r];
                uint16_t g_value = TWO_BIT_LUT[g];
                uint16_t b_value = TWO_BIT_LUT[b];

                uint16_t lut_value = (r_value << 4) | (g_value << 2) | (b_value);

                uint16_t lut_index = (r * 64) + (g * 8) + b;
                SIX_BIT_LUT[lut_index] = lut_value;

                
                //printf("R: %d G: %d B: %d Index: %04X %012b, Value: %04X %06b\n", r, g, b, lut_index, lut_index, lut_value, lut_value);

            }
        }
    }

}


PIO vidPIO = pio0;
uint sm_sync = 0;
uint sm_pixels = 1;
uint sm_pixels_read = 2;


uint offset_sync;
uint offset_pixels;
uint offset_pixels_read;

//uint32_t IMAGE_DATA[IMAGE_SIZE_WORDS+1];
unsigned char IMAGE_DATA[IMAGE_SIZE_WORDS];
unsigned char * IMAGE_DATA_ADDRESS = IMAGE_DATA;

int dma_chan_capture;
int dma_chan_reset;

int dma_chan_lookup_capture;
int dma_chan_write_capture;
int dma_chan_lookup_reset;
int dma_chan_write_reset;


bool frameReady;

//PIO adcPIO = pio0; //pio1;
//uint sm_flashadc_red = 0;

// sync reading
// const uint SYNC_OUT_PIN = 22; // this will have to be disabled with FTDI, not enough pins
const uint SYNC_IN_PIN = 10; //15;

// pixel reading
// const uint RGB_IN_STATUS_OUT_PIN = 28;  // this will have to be disabled with FTDI, not enough pins
//const uint RGB_IN_START_PIN = 6; //9; // 9 10 b, 11 12 g, 13 14 r

//const uint BLUE_ADC_IN_PIN = 27; //16; // 16, 17, 18
//const uint BLUE_ADC_OUT_PIN = 6; //9;

//const uint GREEN_ADC_IN_PIN = 20; //19; // 19, 20, 21
//const uint GREEN_ADC_OUT_PIN = 8; //11;

const uint RED_ADC_IN_PIN = 0; //26; // 26, 27, 28
//const uint RED_ADC_OUT_PIN = 10; //13;

/*
const uint RED_NOT_ENABLE_PIN = 20;
const uint GREEN_NOT_ENABLE_PIN = 21;
const uint BLUE_NOT_ENABLE_PIN = 22;
*/

const uint NTSC_PAL_PIN = 11;
const uint PAUSE_PIN = 20;

const uint SWITCH_R_PIN = 21;
const uint SWITCH_GB_PIN = 22;


const uint USER_BTN_PIN = 24;

#define VDP_NTSC_MODE 1
#define VDP_PAL_MODE 0


//libdvi output
#define dvi_d2_pins_base 16
#define dvi_d1_pins_base 18
#define dvi_d0_pins_base 12
#define dvi_clk_pins_base 14

//
//libdvi config
//

#define dvi_pio pio1
#define dvi_tmds_sm_0 1
#define dvi_tmds_sm_1 2
#define dvi_tmds_sm_2 3

// DVDD 1.2V (1.1V seems ok too)
//#define FRAME_WIDTH 320
//#define FRAME_HEIGHT 240
#define VREG_VSEL VREG_VOLTAGE_1_20

#define DVI_TIMING dvi_timing_640x480p_60hz

struct dvi_inst dvi0;

static struct dvi_serialiser_cfg pico_sms_rgb_conf = {
	.pio = dvi_pio,
	.sm_tmds = {dvi_tmds_sm_0, dvi_tmds_sm_1, dvi_tmds_sm_2},
	.pins_tmds = {dvi_d0_pins_base, dvi_d1_pins_base, dvi_d2_pins_base},
	.pins_clk = dvi_clk_pins_base,
	.invert_diffpairs = false
};

#define DVI_DMA_IRQ DMA_IRQ_0

#define DVI_AUDIO_CTS 28000
#define DVI_AUDIO_BUFFER_SIZE 256
audio_sample_t dvi_audio_buffer[DVI_AUDIO_BUFFER_SIZE];


//
//ADC audio capture
//
#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_CHANNEL_COUNT 1 //4 //2 audio + 1 brightness potentiometer, but needs to be power of 2 for dma and ring buffer to work ok 
#define AUDIO_BUFFER_BITS 8 //11
#define AUDIO_BUFFER_SIZE (1 << AUDIO_BUFFER_BITS)
//#define AUDIO_BUFFER_SIZE 1024

__attribute__((aligned((1 << AUDIO_BUFFER_BITS))))
__attribute__((section(".time_critical.ram")))
static uint16_t audio_buffer[AUDIO_BUFFER_SIZE];

static const uint32_t audio_buffer_addr = (uint32_t)audio_buffer;


//
//draws a pattern of colour bars vertically with varying intensity horizontally
//fills the frame buffer which is larger than the lcd, appears clipped in the output is normal
//
void fill_framebuffer_with_test_pattern() {
	uint16_t test_divs = pixels_in_scanline / 8;	
	uint16_t row_size = scanlines_in_active_area / 3;
	uint16_t row_diff = 1;

	for(uint32_t y = 0; y < scanlines_in_active_area; y++) {
		unsigned char row_val = (y / row_size);
		row_val = row_diff * row_val;
		row_val = 3 - row_val;

		//uint16_t row_val = 15;

		for(uint32_t x = 0; x < pixels_in_scanline; x++) {
			uint32_t pixel = 0;

            pixel = 0;

			if(x < test_divs)
			{
				//white
				pixel |= row_val;
				pixel |= (row_val << 4);
				pixel |= (row_val << 2);
			}
			else if (x < test_divs * 2)
			{
				//yellow
				pixel |= (row_val << 4);
				pixel |= (row_val << 2);
				
			}
			else if (x < test_divs * 3)
			{
				//teal
				pixel |= row_val;
				pixel |= (row_val << 2);
			
			}
			else if (x < test_divs * 4)
			{
				//green
				pixel |= (row_val << 2);
			}
			else if (x < test_divs * 5)
			{
				//purple
				pixel |= row_val;
				pixel |= (row_val << 4);
			}
			else if (x < test_divs * 6)
			{
				//red
				pixel |= (row_val << 4);
			}
			else if (x < test_divs * 7)
			{
				//blue
				pixel |= row_val;
			}
			

			IMAGE_DATA[x + (y * pixels_in_scanline)]  = pixel;
			//framebuffer2[x + (y * pixels_in_scanline)]  = ~pixel;
			//framebuffer2[x + (y * pixels_in_scanline)]  = pixel;

		}
	}

	//draws an outline around the rendered portion, in theory
	/*if(gg_now)
	{
		draw_rectangle_empty(framebuffer, gg_pixel_x_offset + 45, gg_v_lines_to_skip, gg_pixel_width, gg_pixel_height, 0xFFF);
		draw_rectangle_empty(framebuffer2, gg_pixel_x_offset + 45, gg_v_lines_to_skip, gg_pixel_width, gg_pixel_height, 0xFFF);
	}
	else
	{
		draw_rectangle_empty(framebuffer, sms_pixel_x_offset, sms_v_lines_to_skip, sms_pixel_width, sms_pixel_height, 0xFFF);
		draw_rectangle_empty(framebuffer2, sms_pixel_x_offset, sms_v_lines_to_skip, sms_pixel_width, sms_pixel_height, 0xFFF);
	}*/

}


struct repeating_timer adc_timer;
struct repeating_timer dvi_audio_timer;
volatile uint16_t audio_write_pos = 0;
volatile uint16_t audio_read_pos = 0;

volatile int64_t accum_l = 0;
volatile int64_t accum_r = 0;
volatile uint16_t decimate = 0;
static int16_t prev_l = 0;

static const uint16_t audio_bias_midpoint = 2048; //ADC is 0-4095 2048 is 1.65v midpoint of 3.3v
//uint16_t audio_bias_midpoint = 1900; //1.53v
//static const int16_t audio_bias_midpoint = 1948; //1.57v
//uint16_t audio_bias_midpoint = 1775; //1.43v
//uint16_t audio_bias_midpoint = 1737; //1.40v
//uint16_t audio_bias_midpoint = 1514; //1.22v
//uint16_t audio_bias_midpoint = 1365; //1365 is midpoint of 1.1v
//uint16_t audio_bias_midpoint = 1340; //1.08v
//uint16_t audio_bias_midpoint = 1290; //1.04v
//uint16_t audio_bias_midpoint = 1240; //1.0v


int adc_dma_chan_sample = -1;
int adc_dma_chan_control = -1;

bool __not_in_flash_func(dvi_audio_timer_callback_dma)(struct repeating_timer *t)
{

	//while(true)
	{
		//get how many audio samples for the dvi buffer
		int size = get_write_size(&dvi0.audio_ring, true);
		if(size <= 0) return true;
		if(size >= DVI_AUDIO_BUFFER_SIZE)
		{
			size = DVI_AUDIO_BUFFER_SIZE;
		}

		//get where we need to write the audio sample to
		audio_sample_t *audio_ptr = get_write_pointer(&dvi0.audio_ring);
		uint32_t audio_offset = get_write_offset(&dvi0.audio_ring);
		audio_sample_t sample;

		//Should be using trans_count as write_addr is unreliable
		uint32_t current_trans_count = dma_hw->ch[adc_dma_chan_sample].transfer_count;
		uint32_t samples_written = AUDIO_BUFFER_SIZE - current_trans_count;

		audio_write_pos = samples_written - (samples_written % 2);
		
		printf("Trans Count: %u | Buffer Size: %u | samples requested: %u | Samples Available: %d | write_pos: %u | read_pos: %u \n",
           current_trans_count, AUDIO_BUFFER_SIZE, size, audio_write_pos-audio_read_pos, audio_write_pos, audio_read_pos);
		//printf("Trans Count: %u | Buffer Size: %u | samples requested: %u | Samples written: %u | write_pos: %u | read_pos: %u | FIFO level: %d\n",
        //   current_trans_count, AUDIO_BUFFER_SIZE, size, samples_written, audio_write_pos, audio_read_pos, adc_fifo_get_level());
		//printf("DMA addr: 0x%08X | Samples written: %u | write_pos: %u | read_pos: %u | FIFO level: %d | Request Size: %d | Buffer Size: %d\n",
        //   current_dma_addr, samples_written, audio_write_pos, audio_read_pos, adc_fifo_get_level(), size, AUDIO_BUFFER_SIZE);
		
		for(int cnt = 0; cnt < size; cnt++)
		{
			if(audio_write_pos == audio_read_pos)
			{
				//break;
				 sample.channels[0] = 0; //silence on underrun?
				 sample.channels[1] = 0;
			}
			else 
			{				

				sample.channels[0] =  (int16_t)(audio_buffer[audio_read_pos]) - audio_bias_midpoint ;
                sample.channels[1] =  (int16_t)(audio_buffer[audio_read_pos]) - audio_bias_midpoint ; //sample.channels[0] ;    

                /*if(audio_read_pos %50 == 0)
                {
                    printf("audio_read_pos: %d, buffer: %d, sample: %d\n", audio_read_pos, audio_buffer[audio_read_pos], sample.channels[0]);
                }*/

                //increment read position
				audio_read_pos = ((audio_read_pos + 1) % AUDIO_BUFFER_SIZE);
                

				//audio_read_pos = ((audio_read_pos + 1) % AUDIO_BUFFER_SIZE);   

				//audio_read_pos = ((audio_read_pos + 1) % AUDIO_BUFFER_SIZE);         

				//audio_read_pos = ((audio_read_pos + 1) % AUDIO_BUFFER_SIZE);
		
				audio_offset = (audio_offset + 1) % (DVI_AUDIO_BUFFER_SIZE - 1);
                
			}

			*audio_ptr++ = sample;
			increase_write_pointer(&dvi0.audio_ring, 1);
			audio_ptr = get_write_pointer(&dvi0.audio_ring);

		}

	}
	
    return true;
}

/*
bool __not_in_flash_func(adc_callback_test)(struct repeating_timer *t)
{
    adc_select_input(sms_audio_pin);
    uint16_t adc_value = adc_read();
    printf("adc_value: %d\n", adc_value);
    
    return true;
}
*/

void config_audio()
{

    printf("%s: Configuring Audio, buffer size: %d\n", __func__, AUDIO_BUFFER_SIZE);

	for(uint32_t i = 0; i < AUDIO_BUFFER_SIZE; i++)
	{
		//audio_buffer[i] = 2048;
        audio_buffer[i] = 1234;
	}

	adc_init();
	adc_gpio_init(sms_audio_pin); //enable adc and disabled gpio on these pins
    //adc_gpio_init(27);
    //adc_gpio_init(28);
    //adc_gpio_init(29);
	
	adc_set_temp_sensor_enabled(false);

    //adc_select_input(sms_audio_pin);

	//add_repeating_timer_ms(1, adc_callback_test, NULL, &dvi_audio_timer);

	adc_set_round_robin(0b1); //sample adc pins 1 and 2 i.e. 26/27 and 28 for brightness potentiometer, 29 is ignored but needs to be read to fit everything in a power of 2 buffer

	float clk_div = (48 * 1000 * 1000) / (AUDIO_SAMPLE_RATE * (AUDIO_CHANNEL_COUNT)); //need to include the brightness potentiometer
	adc_set_clkdiv(clk_div - 1.0f);
		
	adc_fifo_setup(true, true, 1, false, false);


	//
	//adc dma config
	//
	adc_dma_chan_sample = dma_claim_unused_channel(true);	
	adc_dma_chan_control = dma_claim_unused_channel(true);
    
    printf("%s: adc_dma_chan_sample: %d\n", __func__, adc_dma_chan_sample);
    printf("%s: adc_dma_chan_control: %d\n", __func__, adc_dma_chan_control);

	dma_channel_config adc_dma_config_sample = dma_channel_get_default_config(adc_dma_chan_sample);
	channel_config_set_transfer_data_size(&adc_dma_config_sample, DMA_SIZE_16);
	channel_config_set_read_increment(&adc_dma_config_sample, false);
	channel_config_set_write_increment(&adc_dma_config_sample, true);
	channel_config_set_dreq(&adc_dma_config_sample, DREQ_ADC);
	channel_config_set_ring(&adc_dma_config_sample, true, AUDIO_BUFFER_BITS+1); //+1 ???? for 2 channels??? idk
	channel_config_set_chain_to(&adc_dma_config_sample, adc_dma_chan_control);
	channel_config_set_enable(&adc_dma_config_sample, true);

	dma_channel_configure(adc_dma_chan_sample, 
		&adc_dma_config_sample, 
		audio_buffer, 
		&adc_hw->fifo, 
		AUDIO_BUFFER_SIZE, 
		false);


	dma_channel_config adc_dma_config_control = dma_channel_get_default_config(adc_dma_chan_control);
	channel_config_set_transfer_data_size(&adc_dma_config_control, DMA_SIZE_32);
	channel_config_set_read_increment(&adc_dma_config_control, false);
	channel_config_set_write_increment(&adc_dma_config_control, false);
	channel_config_set_dreq(&adc_dma_config_control, DREQ_FORCE);
	channel_config_set_chain_to(&adc_dma_config_control, adc_dma_chan_sample);
	channel_config_set_enable(&adc_dma_config_control, true);

	dma_channel_configure(adc_dma_chan_control, 
		&adc_dma_config_control, 
		&dma_hw->ch[adc_dma_chan_sample].al2_write_addr_trig, 
		&audio_buffer_addr, 
		1, 
		false);


	dma_channel_start(adc_dma_chan_control);
    
	//timer for sending dvi audio
	add_repeating_timer_ms(1, dvi_audio_timer_callback_dma, NULL, &dvi_audio_timer);

	adc_run(true);
	

}


//
//secondary dvi output loop
//
void core1_main() 
{
    //allow the other core to take over if the flash becomes active
    //flash_safe_execute_core_init(); //this breaks audio right now - this core doesnt need to do anything in flash though... right??

	
	//
	//DVI INIT
	//
	dvi0.timing = &DVI_TIMING;
	dvi0.ser_cfg = pico_sms_rgb_conf;
	dvi0.vertical_repeat = DVI_VERTICAL_REPEAT_DEFAULT;
	dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
	
	//DVI AUDIO

 	for(uint32_t i = 0; i < DVI_AUDIO_BUFFER_SIZE; i++)
	{
		dvi_audio_buffer[i].channels[0] = 0;
		dvi_audio_buffer[i].channels[1] = 0;
	}		

	dvi_get_blank_settings(&dvi0)->top = 0;
	dvi_get_blank_settings(&dvi0)->bottom = 0;
	dvi_audio_sample_buffer_set(&dvi0, dvi_audio_buffer, DVI_AUDIO_BUFFER_SIZE);
	dvi_set_audio_freq(&dvi0, AUDIO_SAMPLE_RATE, DVI_AUDIO_CTS, 6272);


	//configure audio events on current core
	config_audio();


	while(true)
	{
		
		/*if(last_gg)
		{
			printf("starting DVI in GG mode\n");
			dvi0.vertical_repeat = DVI_VERTICAL_REPEAT_GG;
			dvi_register_irqs_this_core(&dvi0, DVI_DMA_IRQ);
			dvi_start(&dvi0);
			dvi_scanbuf_main_12bpp_noqueue_gg(&dvi0, framebuffer, framebuffer2, dma_chan_fb1_write, dma_chan_fb2_write);
			printf("stopping DVI in GG mode\n");
		}
		else*/
		{
			printf("%s: starting DVI in SMS mode\n", __func__);
			dvi0.vertical_repeat = DVI_VERTICAL_REPEAT_SMS;
			dvi_register_irqs_this_core(&dvi0, DVI_DMA_IRQ);
			dvi_start(&dvi0);
			//dvi_scanbuf_main_12bpp_noqueue_sms(&dvi0, framebuffer, framebuffer2, dma_chan_fb1_write, dma_chan_fb2_write);
            dvi_scanbuf_main_12bpp_noqueue_sms(&dvi0, IMAGE_DATA);
			printf("%s: stopping DVI in SMS mode\n", __func__);
		}
		dvi_stop(&dvi0);
	}

}

/*
static inline void flash_adc_program_init(PIO pio, uint sm, uint offset, uint sampleInPin, uint bitsOutPin) {
    pio_sm_config c = flash_adc_program_get_default_config(offset);
    sm_config_set_in_pins(&c, sampleInPin);
    sm_config_set_out_pins(&c, bitsOutPin, 2);
    sm_config_set_set_pins(&c, bitsOutPin, 2);
    pio_gpio_init(pio, bitsOutPin);
    pio_gpio_init(pio, bitsOutPin+1);
    pio_sm_set_consecutive_pindirs(pio, sm, bitsOutPin, 2, true);
    //pio_sm_set_consecutive_pindirs(pio, sm, sampleInPin, 3, false);
    pio_sm_set_consecutive_pindirs(pio, sm, sampleInPin, 3, false);
    //sm_config_set_in_shift(&c, false, false, 3);
    sm_config_set_in_shift(&c, false, false, 2);
    // Initialize and enable the state machine.
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}
*/

static inline void vsync_detector_program_init(PIO pio, uint sm, uint offset, uint syncInPin) {


    pio_gpio_init(pio, syncInPin);

    pio_sm_config c = vsync_detector_program_get_default_config(offset);
    //sm_config_set_set_pins(&c, outPin, 1);
    //sm_config_set_out_pins(&c, outPin, 1);
    sm_config_set_in_pins(&c, syncInPin);
    //pio_gpio_init(pio, outPin);
    //pio_sm_set_consecutive_pindirs(pio, sm, outPin, 1, true);
    pio_sm_set_consecutive_pindirs(pio, sm, syncInPin, 1, false);
    sm_config_set_jmp_pin(&c, syncInPin);
    //sm_config_set_clkdiv(&c, 2); // 133/2 = 66.5MHz
    //sm_config_set_clkdiv(&c, 3); 
    //sm_config_set_clkdiv(&c, (DVI_TIMING.bit_clk_khz / sms_clock_pal_khz));
    //sm_config_set_clkdiv(&c, (DVI_TIMING.bit_clk_khz / sms_clock_khz));
    
    sm_config_set_clkdiv(&c, 1);

    //sm_config_set_clkdiv(&c, 1);
    
    // Initialize and enable the state machine.
    pio_sm_init(pio, sm, offset, &c);

    pio_sm_set_jmp_pin(pio, sm, syncInPin);
    
    //pio_sm_set_enabled(pio, sm, true);
}

static inline void pio_6bpp_color_read_program_init(PIO pio, uint sm, uint offset, uint startPin) {
    pio_sm_config c = pio_6bpp_color_read_program_get_default_config(offset);
    //sm_config_set_in_pins(&c, startPin);
    //pio_sm_set_consecutive_pindirs(pio, sm, startPin, 6, false);
    //sm_config_set_in_shift(&c, false, true, 6);

    //sm_config_set_clkdiv(&c, 1);
    //sm_config_set_clkdiv(&c, (DVI_TIMING.bit_clk_khz / sms_clock_pal_khz));

    //float clockdiv = (float)(DVI_TIMING.bit_clk_khz / sms_clock_pal_khz) / 2.f;
    float clockdiv = (float)(DVI_TIMING.bit_clk_khz / sms_clock_khz) / 2.f;
    sm_config_set_clkdiv(&c, clockdiv);
    printf("%s: Pixel ClockDiv: %f\n", __func__, clockdiv);

    // Initialize and enable the state machine.
    pio_sm_init(pio, sm, offset, &c);
}

static inline void pio_6bpp_color_read_pixel_program_init(PIO pio, uint sm, uint offset, uint startPin) {
    
    
    pio_sm_config c = pio_6bpp_color_read_pixel_program_get_default_config(offset);
    sm_config_set_in_pins(&c, startPin);
    pio_sm_set_consecutive_pindirs(pio, sm, startPin, 9, false);

    sm_config_set_in_shift(&c, false, true, 32);
    //sm_config_set_in_shift(&c, false, true, 6);

    //sm_config_set_sideset_pins(&c, RED_NOT_ENABLE_PIN);
    
    //pio_gpio_init(pio, SWITCH_R_PIN);
    //pio_gpio_init(pio, SWITCH_GB_PIN);
    //pio_sm_set_consecutive_pindirs(pio, sm, SWITCH_R_PIN, 2, true);
    //sm_config_set_sideset_pins(&c, SWITCH_R_PIN);

    //sm_config_set_clkdiv(&c, (DVI_TIMING.bit_clk_khz / sms_clock_pal_khz));
    sm_config_set_clkdiv(&c, 1.0f);
    
    //float clockdiv = (float)(DVI_TIMING.bit_clk_khz / sms_clock_pal_khz) / 2.f;
    //printf("Read Pixel ClockDiv: %f\n", clockdiv);
    //sm_config_set_clkdiv(&c, clockdiv);

    // Initialize and enable the state machine.
    pio_sm_init(pio, sm, offset, &c);
}


void claim_pio_sm()
{
	pio_sm_claim(vidPIO, sm_sync);
    pio_sm_claim(vidPIO, sm_pixels);
    pio_sm_claim(vidPIO, sm_pixels_read);
}

void config_pio()
{

    offset_sync = pio_add_program(vidPIO, &vsync_detector_program);
    //uint sm_sync = pio_claim_unused_sm(vidPIO, true);
    

    gpio_set_input_enabled(SYNC_IN_PIN, true);

    // PIO pixelReadPIO = pio1;
    offset_pixels = pio_add_program(vidPIO, &pio_6bpp_color_read_program);
    

    offset_pixels_read = pio_add_program(vidPIO, &pio_6bpp_color_read_pixel_program);
    

    /*for (uint i=0; i<6; i++) {
        gpio_set_input_enabled(RGB_IN_START_PIN+i, true);
    }*/


    // Force 224 vertical sync in vsync register
    //pio_sm_put_blocking(vidPIO, sm_sync, 223);
    pio_sm_put_blocking(vidPIO, sm_sync, scanlines_in_active_area-1);
    pio_sm_exec_wait_blocking(vidPIO, sm_sync, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(vidPIO, sm_sync, pio_encode_mov(pio_y, pio_osr));

    // force 320 pixels wide into hsync
    //pio_sm_put_blocking(vidPIO, sm_pixels, 63);
    pio_sm_put_blocking(vidPIO, sm_pixels, pixels_in_scanline-1);
    pio_sm_exec_wait_blocking(vidPIO, sm_pixels, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(vidPIO, sm_pixels, pio_encode_mov(pio_y, pio_osr));


    uint32_t lut_address = ((uint32_t)SIX_BIT_LUT) >> 9;
    pio_sm_put_blocking(vidPIO, sm_pixels_read, lut_address);
    pio_sm_exec_wait_blocking(vidPIO, sm_pixels_read, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(vidPIO, sm_pixels_read, pio_encode_mov(pio_y, pio_osr));


    //pio_6bpp_color_read_pixel_program_init(vidPIO, sm_pixels_read, offset_pixels_read, RGB_IN_START_PIN);
    //pio_6bpp_color_read_program_init(vidPIO, sm_pixels, offset_pixels, RGB_IN_START_PIN);

    //gpio_set_input_enabled(RED_NOT_ENABLE_PIN, true);
    //gpio_set_input_enabled(GREEN_NOT_ENABLE_PIN, true);
    //gpio_set_input_enabled(BLUE_NOT_ENABLE_PIN, true);
    //pio_gpio_init(vidPIO, RED_NOT_ENABLE_PIN); 
    //pio_gpio_init(vidPIO, GREEN_NOT_ENABLE_PIN); 
    //pio_gpio_init(vidPIO, BLUE_NOT_ENABLE_PIN); 

    //pio_sm_set_consecutive_pindirs(vidPIO, sm_pixels_read, RED_NOT_ENABLE_PIN, 3, true);


    
	


    pio_6bpp_color_read_pixel_program_init(vidPIO, sm_pixels_read, offset_pixels_read, RED_ADC_IN_PIN);
    pio_6bpp_color_read_program_init(vidPIO, sm_pixels, offset_pixels, RED_ADC_IN_PIN);

    vsync_detector_program_init(vidPIO, sm_sync, offset_sync, SYNC_IN_PIN);


    //PIO adcPIO = pio1;
    //uint offset_flashadc = pio_add_program(adcPIO, &flash_adc_program);

 /*   uint sm_flashadc_blue = pio_claim_unused_sm(adcPIO, true);

    flash_adc_program_init(adcPIO, sm_flashadc_blue, offset_flashadc, BLUE_ADC_IN_PIN, BLUE_ADC_OUT_PIN);
    gpio_set_input_enabled(BLUE_ADC_IN_PIN, true);
    gpio_set_input_enabled(BLUE_ADC_IN_PIN+1, true);
    gpio_set_input_enabled(BLUE_ADC_IN_PIN+2, true);

    uint sm_flashadc_grn = pio_claim_unused_sm(adcPIO, true);
    flash_adc_program_init(adcPIO, sm_flashadc_grn, offset_flashadc, GREEN_ADC_IN_PIN, GREEN_ADC_OUT_PIN);
    gpio_set_input_enabled(GREEN_ADC_IN_PIN, true);
    gpio_set_input_enabled(GREEN_ADC_IN_PIN+1, true);
    gpio_set_input_enabled(GREEN_ADC_IN_PIN+2, true);*/
    
    /*pio_sm_claim(adcPIO, sm_flashadc_red);
    flash_adc_program_init(adcPIO, sm_flashadc_red, offset_flashadc, RED_ADC_IN_PIN+1, RED_ADC_OUT_PIN);
    gpio_set_input_enabled(RED_ADC_IN_PIN, true);
    gpio_set_input_enabled(RED_ADC_IN_PIN+1, true);
    gpio_set_input_enabled(RED_ADC_IN_PIN+2, true);*/

}




volatile uint32_t * write_chan_write_address_pointer;

void claim_dma()
{
	for(uint32_t c = 0; c < 12; c++) {
		dma_channel_cleanup(c);
    	dma_channel_unclaim(c);
	}

	//DVI code uses claim_unused_channel, so we must as well, instead of explicitly picking DMA channels
	//DVI audio also claims 2 dma channels
    dma_chan_lookup_capture = dma_claim_unused_channel(true);
    dma_chan_write_capture = dma_claim_unused_channel(true);
    dma_chan_lookup_reset = dma_claim_unused_channel(true);
    dma_chan_write_reset = dma_claim_unused_channel(true);
    
    printf("%s: dma_chan_lookup_capture: %d\n", __func__, dma_chan_lookup_capture);
    printf("%s: dma_chan_write_capture: %d\n", __func__, dma_chan_write_capture);
    printf("%s: dma_chan_lookup_reset: %d\n", __func__, dma_chan_lookup_reset);    
    printf("%s: dma_chan_write_reset: %d\n", __func__, dma_chan_write_reset);
}

void config_dma()
{
    // Get a free dma channel, panic() if there are none
    //dma_chan_capture = dma_claim_unused_channel(true);
    //dma_chan_reset = dma_claim_unused_channel(true);
    
    
    write_chan_write_address_pointer = &(dma_hw->ch[dma_chan_write_capture].al3_read_addr_trig);

    dma_channel_config lookup_config = dma_channel_get_default_config(dma_chan_lookup_capture);
    channel_config_set_transfer_data_size(&lookup_config, DMA_SIZE_32);
    channel_config_set_read_increment(&lookup_config, false); // We will pull from the RX FIFO, so don't move read ptr
    channel_config_set_write_increment(&lookup_config, false);
    channel_config_set_dreq(&lookup_config, pio_get_dreq(vidPIO, sm_pixels_read, false));
    channel_config_set_chain_to(&lookup_config, dma_chan_lookup_reset);
    channel_config_set_enable(&lookup_config, true);
    dma_channel_configure(
        dma_chan_lookup_capture,                                        // Channel to be configured
        &lookup_config,                                                 // The configuration we just created
        write_chan_write_address_pointer,                               // The initial write address
        &vidPIO->rxf[sm_pixels_read],                                        // The initial read address
        IMAGE_SIZE_PIXELS,                                              // Number of transfers.
        false                                                           // Do not start immediately.
    );

    
    dma_channel_config write_config = dma_channel_get_default_config(dma_chan_write_capture);
    channel_config_set_transfer_data_size(&write_config, DMA_SIZE_8);
    channel_config_set_read_increment(&write_config, true); // We will pull from the RX FIFO, so don't move read ptr
    channel_config_set_write_increment(&write_config, true);
    //channel_config_set_dreq(&write_config, DREQ_FORCE);
    channel_config_set_enable(&write_config, true);
    dma_channel_configure(
        dma_chan_write_capture,                         // Channel to be configured
        &write_config,                                  // The configuration we just created
        IMAGE_DATA,                                     // The initial write address
        NULL,                                           // The initial read address
        1,                                              // Number of transfers.
        false                                           // Do not start immediately.
    );


    dma_channel_config c2 = dma_channel_get_default_config(dma_chan_lookup_reset);
    channel_config_set_transfer_data_size(&c2, DMA_SIZE_32);
    channel_config_set_read_increment(&c2, false); // We will pull from the RX FIFO, so don't move read ptr
    channel_config_set_write_increment(&c2, false);
    channel_config_set_chain_to(&c2, dma_chan_write_reset);
    channel_config_set_enable(&c2, true);
    dma_channel_configure(
        dma_chan_lookup_reset,                                      // Channel to be configured
        &c2,                                                        // The configuration we just created
        &(dma_hw->ch[dma_chan_lookup_capture].write_addr),          // The initial write address
        &write_chan_write_address_pointer,                          // The initial read address
        1,                                          // Number of transfers.
        false                                                       // Do not start immediately.
    );

    dma_channel_config c3 = dma_channel_get_default_config(dma_chan_write_reset);
    channel_config_set_transfer_data_size(&c3, DMA_SIZE_32);
    channel_config_set_read_increment(&c3, false); // We will pull from the RX FIFO, so don't move read ptr
    channel_config_set_write_increment(&c3, false);
    channel_config_set_chain_to(&c3, dma_chan_lookup_capture);
    channel_config_set_enable(&c3, true);
    dma_channel_configure(
        dma_chan_write_reset,                                 // Channel to be configured
        &c3,                                             // The configuration we just created
        &(dma_hw->ch[dma_chan_write_capture].write_addr),     // The initial write address
        &IMAGE_DATA_ADDRESS,                            // The initial read address
        1,                                              // Number of transfers.
        false                                           // Do not start immediately.
    );


/*
    dma_channel_config c = dma_channel_get_default_config(dma_chan_capture);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false); // We will pull from the RX FIFO, so don't move read ptr
    channel_config_set_write_increment(&c, true);
    //channel_config_set_dreq(&c, pio_get_dreq(vidPIO, sm_pixels, false));
    channel_config_set_dreq(&c, pio_get_dreq(vidPIO, sm_pixels_read, false));
    channel_config_set_chain_to(&c, dma_chan_reset);
    channel_config_set_enable(&c, true);
    dma_channel_configure(
        dma_chan_capture,                           // Channel to be configured
        &c,                                 // The configuration we just created
        IMAGE_DATA,                         // The initial write address
        &vidPIO->rxf[sm_pixels_read],            // The initial read address
        IMAGE_SIZE_PIXELS,                   // Number of transfers.
        false                               // Do not start immediately.
    );


    dma_channel_config c2 = dma_channel_get_default_config(dma_chan_reset);
    channel_config_set_transfer_data_size(&c2, DMA_SIZE_32);
    channel_config_set_read_increment(&c2, false); // We will pull from the RX FIFO, so don't move read ptr
    channel_config_set_write_increment(&c2, false);
    channel_config_set_chain_to(&c2, dma_chan_capture);
    channel_config_set_enable(&c2, true);
    dma_channel_configure(
        dma_chan_reset,                                 // Channel to be configured
        &c2,                                             // The configuration we just created
        &(dma_hw->ch[dma_chan_capture].write_addr),     // The initial write address
        &IMAGE_DATA_ADDRESS,                            // The initial read address
        1,                                              // Number of transfers.
        false                                           // Do not start immediately.
    );

    
    dma_channel_start(dma_chan_capture);
*/
}


/*
// FLASH stuff lifted from pico examples
// This function will be called when it's safe to call flash_range_erase
static void call_flash_range_erase(void *param) {
    uint32_t offset = (uint32_t)param;
    
    //printf("erasing %08X\n", offset);
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
}

// This function will be called when it's safe to call flash_range_program
static void call_flash_range_program(void *param) {
    uint32_t offset = ((uintptr_t*)param)[0];
    const uint8_t *data = (const uint8_t *)((uintptr_t*)param)[1];
    
    //printf("programming %08X\n", offset);
    flash_range_program(offset, data, FLASH_PAGE_SIZE);
}

void read_flash()
{
    memcpy(config, nvdata, FLASH_PAGE_SIZE);
    //printf("read flash\n");
}

void write_flash()
{
    //printf("writing flash\n");

    uint32_t ints = save_and_disable_interrupts();

    int rc; 
    rc = flash_safe_execute(call_flash_range_erase, (void*)FLASH_TARGET_OFFSET, UINT32_MAX);
    hard_assert(rc == PICO_OK);
    
    //printf("erased flash\n");

    uintptr_t params[] = { FLASH_TARGET_OFFSET, (uintptr_t)config};
    rc = flash_safe_execute(call_flash_range_program, params, UINT32_MAX);
    hard_assert(rc == PICO_OK);

    //printf("programmed flash\n");
    
    restore_interrupts(ints);
}
*/

struct erase_params {
    uint32_t offset;
    uint32_t len;
};

struct program_params {
    uint32_t offset;
    //const uint8_t *data;
    void *data;
    uint32_t len;
};

static void safe_erase_callback(void * param)
{
    struct erase_params *e = (struct erase_params*) param;
    //printf("Safetly erasing %u bytes from 0x%08X\n", e->len, e->offset);
    flash_range_erase(e->offset, e->len);
}

static void safe_program_callback(void * param)
{
    struct program_params *p = (struct program_params *) param;

    //printf("Safetly copying %u bytes from 0x%08X\n", p->len, p->offset);
    flash_range_program(p->offset, p->data, p->len);
}


Config current_config;

bool save_config()
{
    //CartConfig cfg = {0};
    Config* cfg = &current_config;

    printf("%s: Saving Config\n", __func__);
    
    uint32_t ints = save_and_disable_interrupts();
    
    uint32_t erase_bytes = (sizeof(Config) + 4095) & ~4095UL;

    printf("%s: Erasing %d config bytes at %08X\n", __func__, erase_bytes, FLASH_CONFIG_OFFSET);
    struct erase_params e = {FLASH_CONFIG_OFFSET, erase_bytes};
    int rc = flash_safe_execute(safe_erase_callback, &e, 1000); //1000ms timeout

    if(rc != PICO_OK){
        //printf("%s: safe_erase_callback failed: %d\n", __func__, rc);
        printf("failed: %d\n", rc);
        restore_interrupts(ints);
        return false;
    }
    
    printf("%s: Erased ok\n", __func__);
    
    struct program_params p = {FLASH_CONFIG_OFFSET, cfg, sizeof(Config)};

    rc = flash_safe_execute(safe_program_callback, &p, 2000); //1000ms timeout
    
    if(rc != PICO_OK){
        printf("%s: safe_program_callback failed: %d offset: %08X\n", __func__, rc, FLASH_CONFIG_OFFSET);
        restore_interrupts(ints);
        return false;
    }

    if(rc == PICO_OK)
    {
        printf("%s: Config Saved:\n ntsc_pal_toggle: %s\n", __func__, cfg->ntsc_pal_toggle);
    }
    else 
    {  
        printf("%s: Failed to save config: %d\n", __func__, rc);
    }

    restore_interrupts(ints);
    
}


void load_config()
{
    //printf("%s: Loading Config...\n", __func__);
    memcpy(&current_config, (const void*)(XIP_BASE + FLASH_CONFIG_OFFSET), sizeof(Config));

    if(current_config.identifier != CONFIG_IDENTIFIER
        || current_config.version != CONFIG_VERSION)
    {
        printf("%s: Old or invalid config detected - resetting\n", __func__);
        memset(&current_config, 0, sizeof(Config));

        current_config.identifier = (uint32_t)CONFIG_IDENTIFIER;
        current_config.version = (uint32_t)CONFIG_VERSION;
        
        current_config.ntsc_pal_toggle = 0;

        sleep_ms(10);

        save_config();
    }
    else
    {        
        printf("%s: Loaded Config: Id: %08X Version: %d\n ntsc_pal_toggle: %d\n",__func__, current_config.identifier, current_config.version, current_config.ntsc_pal_toggle);
    }

}

 
void dma_start()
{
    dma_channel_start(dma_chan_lookup_capture);
}

void dma_stop()
{
    dma_channel_abort(dma_chan_lookup_capture);
    dma_channel_abort(dma_chan_write_capture);
    dma_channel_abort(dma_chan_lookup_reset);
    dma_channel_abort(dma_chan_write_reset);

}

void pio_start()
{
    pio_sm_set_enabled(vidPIO, sm_pixels_read, true);
    pio_sm_set_enabled(vidPIO, sm_pixels, true);
    pio_sm_set_enabled(vidPIO, sm_sync, true);
}

void pio_stop()
{

	//stop and reset pios
	pio_set_sm_mask_enabled(vidPIO, 0b1111, false);

	pio_clear_instruction_memory(vidPIO); 
    
	pio_interrupt_clear(vidPIO, 0);
	pio_interrupt_clear(vidPIO, 1);
	pio_interrupt_clear(vidPIO, 2);
	pio_interrupt_clear(vidPIO, 3);
	pio_interrupt_clear(vidPIO, 4);
	pio_interrupt_clear(vidPIO, 5);
	pio_interrupt_clear(vidPIO, 6);	
	pio_interrupt_clear(vidPIO, 7);

    
	pio_sm_clear_fifos(vidPIO, sm_pixels_read);
	pio_sm_clear_fifos(vidPIO, sm_pixels);
	pio_sm_clear_fifos(vidPIO, sm_sync);

	pio_remove_program(vidPIO, &pio_6bpp_color_read_pixel_program, offset_pixels_read);
	pio_remove_program(vidPIO, &pio_6bpp_color_read_program, offset_pixels);
	pio_remove_program(vidPIO, &vsync_detector_program, offset_sync);

}


bool current_ntsc = false;

void toggle_ntsc_pal_mode()
{
    current_ntsc = !current_ntsc;

    //config[0] = current_ntsc;
    current_config.ntsc_pal_toggle = current_ntsc;
    printf("%s: Switched NTSC/PAL! %d \n",__func__, current_ntsc);

    if(current_ntsc)
    {
        gpio_put(NTSC_PAL_PIN, VDP_NTSC_MODE);
    }
    else
    {
        gpio_put(NTSC_PAL_PIN, VDP_PAL_MODE);
    }

    save_config();

/*  
    pio_stop();
    dma_stop();

    config_pio();
    config_dma();

    dma_start();
    pio_start();
*/
} 

 const uint LED_PIN = PICO_DEFAULT_LED_PIN;



int main()
{
    
	vreg_set_voltage(VREG_VSEL);
	sleep_ms(10);
	set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);


	//stdio_init_all();
    //stdio_usb_init();
    
	sleep_ms(1000);
	printf("------------\n");
	printf("PICO SMS RGB\n");
	printf("------------\n");
	sleep_ms(10);

    load_config();

	sleep_ms(10);


    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);


    gpio_init(NTSC_PAL_PIN);
    gpio_set_dir(NTSC_PAL_PIN, GPIO_OUT);

    current_ntsc = current_config.ntsc_pal_toggle;
    
    printf("%s: current_ntsc = %d \n", __func__, current_ntsc);
   // gpio_put(LED_PIN, current_ntsc);

    if(current_ntsc)
    {
        gpio_put(NTSC_PAL_PIN, VDP_NTSC_MODE);
    }
    else
    {
        gpio_put(NTSC_PAL_PIN, VDP_PAL_MODE);
    }


    gpio_init(PAUSE_PIN);
    gpio_set_dir(PAUSE_PIN, GPIO_IN);
    //gpio_set_pulls(PAUSE_PIN, false, false);

    gpio_init(USER_BTN_PIN);
    gpio_set_dir(USER_BTN_PIN, GPIO_IN);
    gpio_set_pulls(USER_BTN_PIN, true, false);

	sleep_ms(10);

    generate_lut();

    fill_framebuffer_with_test_pattern();

	sleep_ms(10);


    claim_pio_sm();
    config_pio();

    claim_dma();
    config_dma();

	sleep_ms(10);

    dma_start();
    pio_start();

    //pio_sm_set_enabled(adcPIO, sm_flashadc_red, true);

    //core1_main();

	sleep_ms(10);
    
	multicore_reset_core1();

	multicore_launch_core1(core1_main); //libdvi core

    frameReady = false;

    bool pause_pressed = false;
    bool pause_down = false;
    bool pause_up = false;
    bool pause_was_pressed = false;

    uint8_t pause_count = 0;
    uint32_t pause_timeout = 0;
    uint32_t time_now = 0;

    frameReady = true;
    gpio_put(LED_PIN, frameReady);
    uint32_t led_timer = 0;

    while (1) {
        //gpio_put(LED_PIN, true);
        //processNextFrame();
        //busy_wait_at_least_cycles(133);
        //busy_wait_at_least_cycles(300);
        //printf("test\n");


        time_now = time_us_32();

	    //sleep_ms(200);
        if(time_now > led_timer)
        {
            frameReady = !frameReady;
            gpio_put(LED_PIN, frameReady);
            led_timer = time_now + (200 * 1000); //200ms
        }
        //sleep_ms(2);

        
        pause_pressed = !gpio_get(PAUSE_PIN);
        //pause_pressed = !gpio_get(PAUSE_PIN) || !gpio_get(USER_BTN_PIN);
        //pause_pressed = !gpio_get(USER_BTN_PIN);

        if(pause_pressed)
        {
            
            pause_down = !pause_was_pressed;
            pause_was_pressed = true;

            pause_up = false;

            //printf("Pause Pressed?\n");

            /*if(pause_down)
            {
                //frameReady = !frameReady;
                //gpio_put(LED_PIN, frameReady);

                if(pause_timeout > 0 && pause_timeout < time_now) 
                {
                    pause_count = 0;
                    pause_timeout = 0;
                }
            
                //pause_timeout = 0;
                
                //printf("Pause Timeout!\n");
                
                if(pause_timeout == 0 || pause_timeout > time_now)
                {
                    pause_count++;

                    //printf("%s: Pause Down! %d \n",__func__, pause_count);
                    
                    if(pause_count == 1)
                    {
                        pause_timeout = time_now + (2 * 1000 * 1000);
                    } 
                    
                    if(pause_count == 2)
                    {
                        pause_count = 0;
                        pause_timeout = 0;

                        toggle_ntsc_pal_mode();
                        //gpio_put(LED_PIN, current_ntsc);
                    }
                }
            }*/

        }
        else
        {
            pause_up = pause_was_pressed;
            pause_was_pressed = false;
            
            pause_down = false;

            //printf("Pause Not Pressed?\n");

            if(pause_up)
            {

                if(pause_timeout > 0 && pause_timeout < time_now) 
                {
                    pause_count = 0;
                    pause_timeout = 0;
                }
                if(pause_timeout == 0 || pause_timeout > time_now)
                {
                    pause_count++;

                    //printf("%s: Pause Up! %d \n",__func__, pause_count);
                    
                    if(pause_count == 1)
                    {
                        pause_timeout = time_now + (2 * 1000 * 1000);
                    } 
                    
                    if(pause_count == 4)
                    {
                        pause_count = 0;
                        pause_timeout = 0;

                        toggle_ntsc_pal_mode();
                        //gpio_put(LED_PIN, current_ntsc);
                    }
                }
            }



        }

        /*else if(pause_timeout > 0 && pause_timeout < time_now)
        {
            pause_timeout = 0;
            pause_count = 0;
            
            //printf("Pause Timeout!\n");
        }*/

        

        sleep_ms(1);
    
    }

    return 0;
}
