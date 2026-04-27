#include <stdio.h>
#include <string.h>

#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"

#include "vsync_detector.pio.h"
#include "pio_6bpp_color_read.pio.h"
#include "flash_adc.pio.h"

#include "libdvi/dvi.h"
#include "libdvi/dvi_serialiser.h"
#include "libdvi/dvi_timing.h"

#include "ProjectName.h"

/*
#define ADBUS0_PIN 0
#define ADBUS1_PIN 1
#define ADBUS2_PIN 2
#define ADBUS3_PIN 3
#define ADBUS4_PIN 4
#define ADBUS5_PIN 5
#define ADBUS6_PIN 6
#define ADBUS7_PIN 7

// ACBUS0 tied low for Chip Select
#define ACBUS1_PIN 8

// tied read hi
// #define ACBUS2_PIN 12

#define ACBUS3_PIN 22
// ACBUS4 tied high for SIWU not used (Sleep Immediate Wake up?)

// tied reset hi
// #define RESET_PIN 15

#define TXE ACBUS1_PIN
// #define RD ACBUS2_PIN // could actually just be tied hi for a dumb transmitter.
#define WR ACBUS3_PIN

#define BUSMASK ((1 << ADBUS0_PIN) | (1 << ADBUS1_PIN) | (1 << ADBUS2_PIN) | (1 << ADBUS3_PIN) | (1 << ADBUS4_PIN) | (1 << ADBUS5_PIN) | (1 << ADBUS6_PIN) | (1 << ADBUS7_PIN))

*/

//#define IMAGE_SIZE_PIXELS (sms_pixel_width*sms_pixel_height)
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



//------------- prototypes -------------//
static void processNextFrame(void);
//static inline void write_byte(uint8_t b);
//static void pipeImage(void* _unsafe_image_ptr_plz);

PIO vidPIO = pio0;
uint sm_sync = 1;
uint sm_pixels = 2;
uint sm_pixels_read = 3;


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

PIO adcPIO = pio0; //pio1;
uint sm_flashadc_red = 0;

// sync reading
// const uint SYNC_OUT_PIN = 22; // this will have to be disabled with FTDI, not enough pins
const uint SYNC_IN_PIN = 3; //15;

// pixel reading
// const uint RGB_IN_STATUS_OUT_PIN = 28;  // this will have to be disabled with FTDI, not enough pins
const uint RGB_IN_START_PIN = 6; //9; // 9 10 b, 11 12 g, 13 14 r

const uint BLUE_ADC_IN_PIN = 27; //16; // 16, 17, 18
const uint BLUE_ADC_OUT_PIN = 6; //9;

const uint GREEN_ADC_IN_PIN = 20; //19; // 19, 20, 21
const uint GREEN_ADC_OUT_PIN = 8; //11;

const uint RED_ADC_IN_PIN = 0; //26; // 26, 27, 28
const uint RED_ADC_OUT_PIN = 10; //13;


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



//
//secondary dvi output loop
//
void core1_main() 
{
	
	//
	//DVI INIT
	//
	dvi0.timing = &DVI_TIMING;
	dvi0.ser_cfg = pico_sms_rgb_conf;
	dvi0.vertical_repeat = DVI_VERTICAL_REPEAT_DEFAULT;
	dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
	
	//DVI AUDIO
/*
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
*/

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
			printf("starting DVI in SMS mode\n");
			dvi0.vertical_repeat = DVI_VERTICAL_REPEAT_SMS;
			dvi_register_irqs_this_core(&dvi0, DVI_DMA_IRQ);
			dvi_start(&dvi0);
			//dvi_scanbuf_main_12bpp_noqueue_sms(&dvi0, framebuffer, framebuffer2, dma_chan_fb1_write, dma_chan_fb2_write);
            dvi_scanbuf_main_12bpp_noqueue_sms(&dvi0, IMAGE_DATA);
			printf("stopping DVI in SMS mode\n");
		}
		dvi_stop(&dvi0);
	}

}

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
    sm_config_set_clkdiv(&c, (DVI_TIMING.bit_clk_khz / sms_clock_pal_khz));
    //sm_config_set_clkdiv(&c, 1);
    
    // Initialize and enable the state machine.
    pio_sm_init(pio, sm, offset, &c);

    pio_sm_set_jmp_pin(pio, sm, syncInPin);
    
    //pio_sm_set_enabled(pio, sm, true);
}

static inline void pio_6bpp_color_read_program_init(PIO pio, uint sm, uint offset, uint startPin) {
    pio_sm_config c = pio_6bpp_color_read_program_get_default_config(offset);
    sm_config_set_in_pins(&c, startPin);
    pio_sm_set_consecutive_pindirs(pio, sm, startPin, 6, false);
    sm_config_set_in_shift(&c, false, true, 6);

    sm_config_set_clkdiv(&c, (DVI_TIMING.bit_clk_khz / sms_clock_pal_khz));

    // Initialize and enable the state machine.
    pio_sm_init(pio, sm, offset, &c);
}

static inline void pio_6bpp_color_read_pixel_program_init(PIO pio, uint sm, uint offset, uint startPin) {
    pio_sm_config c = pio_6bpp_color_read_pixel_program_get_default_config(offset);
    sm_config_set_in_pins(&c, startPin);
    pio_sm_set_consecutive_pindirs(pio, sm, startPin, 3, false);
    sm_config_set_in_shift(&c, false, true, 32);

    //sm_config_set_clkdiv(&c, (DVI_TIMING.bit_clk_khz / sms_clock_pal_khz));
    sm_config_set_clkdiv(&c, 1);

    // Initialize and enable the state machine.
    pio_sm_init(pio, sm, offset, &c);
}

volatile uint32_t * write_chan_write_address_pointer;

int main()
{
    
	vreg_set_voltage(VREG_VSEL);
	sleep_ms(10);
	set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);
	stdio_init_all();
    
	sleep_ms(100);
	printf("PICO SMS RGB\n");
	sleep_ms(100);

    // Init FTDI Pins.
    //gpio_init_mask(BUSMASK);
    //gpio_set_dir_out_masked(BUSMASK);

    //gpio_init(TXE);
    //gpio_set_dir(TXE, false);
    //gpio_init(WR);
    //gpio_set_dir(WR, true);

    //gpio_put(WR, true);

    fill_framebuffer_with_test_pattern();

    
    // Init image capture PIO stuff
    // gpio_set_pulls(BLUE_ADC_IN_PIN, false, true);
    // gpio_set_pulls(BLUE_ADC_IN_PIN+1, false, true);
    // gpio_set_pulls(BLUE_ADC_IN_PIN+2, false, true);
    // gpio_set_pulls(GREEN_ADC_IN_PIN, false, true);
    // gpio_set_pulls(GREEN_ADC_IN_PIN+1, false, true);
    // gpio_set_pulls(GREEN_ADC_IN_PIN+2, false, true);
    // gpio_set_pulls(RED_ADC_IN_PIN, false, true);
    // gpio_set_pulls(RED_ADC_IN_PIN+1, false, true);
    // gpio_set_pulls(RED_ADC_IN_PIN+2, false, true);



    // image termination sequence
    // 0b00000011 00000011 00000011 00000011
    // would be
    // 000000 110000 001100 000011 000000 11
    // black  red    green  blue   black
    // unlikely sequence of colors to happen by chance
    //IMAGE_DATA[IMAGE_SIZE_WORDS] = 0b00000011000000110000001100000011;


    uint offset_sync = pio_add_program(vidPIO, &vsync_detector_program);
    //uint sm_sync = pio_claim_unused_sm(vidPIO, true);
    pio_sm_claim(vidPIO, sm_sync);

    gpio_set_input_enabled(SYNC_IN_PIN, true);

    // PIO pixelReadPIO = pio1;
    offset_pixels = pio_add_program(vidPIO, &pio_6bpp_color_read_program);
    pio_sm_claim(vidPIO, sm_pixels);

    //offset_pixels_read = pio_add_program(vidPIO, &pio_6bpp_color_read_pixel_program);
    //pio_sm_claim(vidPIO, sm_pixels_read);

    for (uint i=0; i<6; i++) {
        gpio_set_input_enabled(RGB_IN_START_PIN+i, true);
    }

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

/*
    uint32_t lut_address = ((uint32_t)TWO_BIT_LUT) >> 3;
    pio_sm_put_blocking(vidPIO, sm_pixels_read, lut_address);
    pio_sm_exec_wait_blocking(vidPIO, sm_pixels_read, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(vidPIO, sm_pixels_read, pio_encode_mov(pio_y, pio_osr));

    pio_6bpp_color_read_pixel_program_init(vidPIO, sm_pixels_read, offset_pixels_read, RED_ADC_IN_PIN);
*/

    pio_6bpp_color_read_program_init(vidPIO, sm_pixels, offset_pixels, RGB_IN_START_PIN);

    vsync_detector_program_init(vidPIO, sm_sync, offset_sync, SYNC_IN_PIN);

    //PIO adcPIO = pio1;
    uint offset_flashadc = pio_add_program(adcPIO, &flash_adc_program);

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
    
    //uint sm_flashadc_red = pio_claim_unused_sm(adcPIO, true);
    pio_sm_claim(adcPIO, sm_flashadc_red);
    flash_adc_program_init(adcPIO, sm_flashadc_red, offset_flashadc, RED_ADC_IN_PIN+1, RED_ADC_OUT_PIN);
    gpio_set_input_enabled(RED_ADC_IN_PIN, true);
    gpio_set_input_enabled(RED_ADC_IN_PIN+1, true);
    gpio_set_input_enabled(RED_ADC_IN_PIN+2, true);


	for(uint32_t c = 0; c < 12; c++) {
		dma_channel_cleanup(c);
    	dma_channel_unclaim(c);
	}


    // Get a free dma channel, panic() if there are none
    dma_chan_capture = dma_claim_unused_channel(true);
    dma_chan_reset = dma_claim_unused_channel(true);
    
    /*dma_chan_lookup_capture = dma_claim_unused_channel(true);
    dma_chan_write_capture = dma_claim_unused_channel(true);
    dma_chan_lookup_reset = dma_claim_unused_channel(true);
    dma_chan_write_reset = dma_claim_unused_channel(true);
    
    
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
        &(dma_hw->ch[dma_chan_write_capture].al3_read_addr_trig),                               // The initial write address
        &vidPIO->rxf[sm_pixels],                                        // The initial read address
        IMAGE_SIZE_PIXELS,                                              // Number of transfers.
        false                                                           // Do not start immediately.
    );

    
    dma_channel_config write_config = dma_channel_get_default_config(dma_chan_write_capture);
    channel_config_set_transfer_data_size(&write_config, DMA_SIZE_8);
    channel_config_set_read_increment(&write_config, false); // We will pull from the RX FIFO, so don't move read ptr
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
*/


    dma_channel_config c = dma_channel_get_default_config(dma_chan_capture);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false); // We will pull from the RX FIFO, so don't move read ptr
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(vidPIO, sm_pixels, false));
    channel_config_set_chain_to(&c, dma_chan_reset);
    channel_config_set_enable(&c, true);
    dma_channel_configure(
        dma_chan_capture,                           // Channel to be configured
        &c,                                 // The configuration we just created
        IMAGE_DATA,                         // The initial write address
        &vidPIO->rxf[sm_pixels],            // The initial read address
        IMAGE_SIZE_PIXELS,                   // Number of transfers.
        false                               // Do not start immediately.
    );


    dma_channel_config c2 = dma_channel_get_default_config(dma_chan_reset);
    channel_config_set_transfer_data_size(&c2, DMA_SIZE_32);
    channel_config_set_read_increment(&c2, false); // We will pull from the RX FIFO, so don't move read ptr
    channel_config_set_write_increment(&c2, false);
    //channel_config_set_chain_to(&c2, dma_chan_capture);
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

    frameReady = false;

    const uint LED_PIN = PICO_DEFAULT_LED_PIN;
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    dma_channel_start(dma_chan_capture);
    //dma_channel_start(dma_chan_lookup_capture);

    //pio_sm_set_enabled(vidPIO, sm_pixels_read, true);
    pio_sm_set_enabled(vidPIO, sm_pixels, true);
    pio_sm_set_enabled(vidPIO, sm_sync, true);

	multicore_reset_core1();

	multicore_launch_core1(core1_main); //libdvi core

    //pio_sm_set_enabled(adcPIO, sm_flashadc_red, true);

    //core1_main();

    while (1) {
        //gpio_put(LED_PIN, true);
        //processNextFrame();
        gpio_put(LED_PIN, frameReady);
        //busy_wait_at_least_cycles(133);
        //busy_wait_at_least_cycles(300);
        //printf("test\n");
	    sleep_ms(100);
        frameReady = true;
        gpio_put(LED_PIN, frameReady);
        frameReady = false;
	    sleep_ms(500);
        
    }

    return 0;
}

/*
static void processNextFrame(void) {
    if (!frameReady) {
        // tud_cdc_write_clear();
        pio_sm_clear_fifos(vidPIO,sm_pixels);
        
        // point the DMA dest to IMAGE_DATA, set xfer size, start the DMA.
        dma_channel_set_write_addr(dma_chan_capture, &IMAGE_DATA[0], true);
        dma_channel_start(dma_chan_capture);

        // then
        // clear vsync flag IRQ4 to indicate  we're ready for a frame
        //vidPIO->irq = 0b00010000;
        //pio_interrupt_clear(vidPIO, 4);

        dma_channel_wait_for_finish_blocking(dma_chan_capture);

        frameReady = true;
        return;
    }

    //pipeImage(IMAGE_DATA);
}
*/
/*
static void pipeImage(void* _unsafe_image_ptr_plz) {
    // omg the FTDI version is so much simpler lol
    uint8_t *bytePtr = (uint8_t*)_unsafe_image_ptr_plz;
    for (uint i=0; i<IMAGE_SIZE_BYTES; i++) {
        write_byte(bytePtr[i]);
    }

    for (uint i=0; i<4; i++) {
        write_byte(0b00000011);
    }

    frameReady = false;
}


static inline void write_byte(uint8_t b) {
    // based on 133MHz clock or
    // 133 MHz = 7.518ns per cycle
    // let's try actually using TXE as a read
    gpio_put(WR, true);
    while (gpio_get(TXE)) { // TXE hi means you gotta wait.
        busy_wait_at_least_cycles(1);
    }
    busy_wait_at_least_cycles(5);
    gpio_put_masked(BUSMASK, b);
    busy_wait_at_least_cycles(1);  // DATA to WR# active setup time: 5ns
    gpio_put(WR, false); // WR active is LOW.
    busy_wait_at_least_cycles(5); // WR# active pulse width: 30ns
    gpio_put(WR, true);
    // busy_wait_at_least_cycles(7); // TXE# inactive after WR# cycle: 49 ns
    // actually this last wait cycle is probably irrelevant, since we're waiting for TXE to go low
}
*/