/**
 * main.c - EGB240 Digital Voice Recorder Skeleton Code
 *
 * This code provides a skeleton implementation of a digital voice 
 * recorder using the Teensy microcontroller and QUT TensyBOBv2 
 * development boards. This skeleton code demonstrates usage of
 * the EGB240DVR library, which provides functions for recording
 * audio samples from the ADC, storing samples temporarily in a 
 * circular buffer, and reading/writing samples to/from flash
 * memory on an SD card (using the FAT file system and WAVE file
 * format. 
 *
 * This skeleton code provides a recording implementation which 
 * samples CH0 of the ADC at 8-bit, 15.625kHz. Samples are stored 
 * in flash memory on an SD card in the WAVE file format. The 
 * filename is set to "EGB240.WAV". The SD card must be formatted 
 * with the FAT file system. Recorded WAVE files are playable on 
 * a computer.
 * 
 * LED4 on the TeensyBOBv2 is configured to flash as an 
 * indicator that the programme is running; a 1 Hz, 50 % duty
 * cycle flash should be observed under normal operation.
 *
 * A serial USB interface is provided as a secondary control and
 * debugging interface. Errors will be printed to this interface.
 *
 * Version: v1.2
 *    Date: 28/05/2017
 *  Author: Mark Broadmeadow
 *  E-mail: mark.broadmeadow@qut.edu.au
 *	Edited: Group 420
 */  

 /************************************************************************/
 /* INCLUDED LIBRARIES/HEADER FILES                                      */
 /************************************************************************/
#include <avr/io.h>
#include <avr/interrupt.h>

#include <stdio.h>

#include "serial.h"
#include "timer.h"
#include "wave.h"
#include "buffer.h"
#include "adc.h"

/************************************************************************/
/* MACROS to use in the code	                                        */
/************************************************************************/
#define SET_BIT(byte, bit) (byte |= (1 << bit))

//Macros for if conditions//
#define BIT_IS_SET(byte, bit) (byte & (1 << bit))           // check to see if the bit is set or not

#define TOP 255

/************************************************************************/
/* ENUM DEFINITIONS                                                     */
/************************************************************************/
enum {
	DVR_STOPPED,
	DVR_RECORDING,
	DVR_PLAYING
};

/************************************************************************/
/* GLOBAL VARIABLES                                                     */
/************************************************************************/
volatile uint16_t pageCount = 0;	// Page counter - used to terminate recording
volatile uint16_t newPage = 0;		// Flag that indicates a new page is available for read/write
volatile uint8_t stop = 0;			// Flag that indicates playback/recording is complete

// Personal global variables
volatile uint32_t data_amount = 0;
volatile uint8_t played = 1;
volatile uint8_t first_que = 0;
volatile uint8_t second_que = 0;
volatile uint8_t first_played = 0;
volatile uint8_t second_played = 0;
/************************************************************************/
/* FUNCTION PROTOTYPES                                                  */
/************************************************************************/
void pageFull();
void pageEmpty();

/************************************************************************/
/* INITIALISATION FUNCTIONS                                             */
/************************************************************************/

// Initialize PLL (required by USB serial interface, PWM)
void pll_init() {
	PLLFRQ = 0x6A; // PLL = 96 MHz, USB = 48 MHz, TIM4 = 64 MHz
}

// Configure system clock for 16 MHz
void clock_init() {
	CLKPR = 0x80;	// Prescaler change enable
	CLKPR = 0x00;	// Prescaler /1, 16 MHz
}

// Initialize buttons and LEDs
void hardware_setup (){
	// clear the bit and set the button pins as input (1= output && 0=input)		
	DDRD |= 0b11110000; // LEDS
	DDRF &= ~0b11110000; // Buttons
}

void set_pwm() {	
	OCR4C = TOP;			// set top to 0xFF (255)
	
	TCCR4B = 0x04;				//Prescaler 8, ~32.5kHz	
	TCCR4A = 0x20;				//set control register for 0C4B
	OCR4B = 0x80;				// initialize to 50% duty cycle
	TIMSK4 = 0x00;				// Disable interrupt	
	DDRB |= 0b01000000;			// set pin B6 to output(JOUT)
	TCNT4 = 0x00;  // reset counter
}
void start_pwm(){
	TIMSK4 = 0x04;				// Enable interrupt
	TCCR4A = 0x21;				//set control register for 0C4B
}
void stop_pwm(){
	TIMSK4 = 0x00;				// Disable interrupt
	TCCR4A = 0x20;				//set control register for 0C4B
}
// Initialize DVR subsystems and enable interrupts
void init() {
	cli();			// Disable interrupts
	clock_init();	// Configure clocks
	pll_init();     // Configure PLL (used by Timer4 and USB serial)
	serial_init();	// Initialize USB serial interface (debug)
	timer_init();	// Initialize timer (used by FatFs library)
	hardware_setup();  // Initialize Button with LEDs
	set_pwm();
	buffer_init(pageFull, pageEmpty);  // Initialize circular buffer (must specify callback functions)
	adc_init();		// Initialize ADC
	sei();			// Enable interrupts
	
	// Must be called after interrupts are enabled
	wave_init();	// Initialize WAVE file interface
}
/************************************************************************/
/* CALLBACK FUNCTIONS FOR CIRCULAR BUFFER                               */
/************************************************************************/

// CALLED FROM BUFFER MODULE WHEN A PAGE IS FILLED WITH RECORDED SAMPLES
void pageFull() {
	if(!(--pageCount)) {
		// If all pages have been read
		adc_stop();		// Stop recording (disable new ADC conversions)
		stop = 1;		// Flag recording complete
	} else {
		newPage = 1;	// Flag new page is ready to write to SD card
	}
}

// CALLED FROM BUFFER MODULE WHEN A NEW PAGE HAS BEEN EMPTIED
void pageEmpty() {
	if (data_amount > 2048) {
		newPage = 1;
	}	
}

/************************************************************************/
/* RECORD/PLAYBACK ROUTINES                                             */
/************************************************************************/

// Initiates a record cycle
void dvr_record() {
	buffer_reset();		// Reset buffer state
	
	pageCount = 305;	// Maximum record time of 10 sec
	newPage = 0;		// Clear new page flag
	
	wave_create();		// Create new wave file on the SD card
	adc_start();		// Begin sampling

	SET_BIT (PORTD, PD1); // turn on the first led
	PORTD &= 0b00001111;  // turn other LEDs off
}

/************************************************************************/
/* MAIN LOOP (CODE ENTRY)                                               */
/************************************************************************/
int main(void) {
	uint8_t state = DVR_STOPPED;	// Start DVR in stopped state	
	// Initialization
	init();	
	PORTD &= 0b00001111;  // turn other LEDs off
	// Loop forever (state machine)
	stop_pwm();
    for(;;) {		
		// Switch depending on state
		switch (state) {
			case DVR_STOPPED:				
				if ( BIT_IS_SET (~PINF, PF5 ) ) {
				//if(~PINF & (1 << 5 ))	  {
					PORTD |= 0b10000000;  // turn LED2 on				
					
					printf("Recording started...");	// Output status to console
					dvr_record();			// Initiate recording
					state = DVR_RECORDING;	// Transition to "recording" state
				 }
				 if ( BIT_IS_SET (~PINF, PF4 ) ) {
				 //if(~PINF & (1 << 4 ))	  {
					 PORTD &= 0b00001111;  // turn all LEDs off
					 PORTD |= 0b01000000;  // turn LED3 on
					 
					 printf("Preparing file\n");	// Output status to console
					 buffer_reset();
					 newPage = 0;
					 data_amount = wave_open ()*2+1;  //Open the file to read not VOID function
					 
					 wave_read (buffer_writePage(), 512); //2 is the number of counts
					 wave_read (buffer_writePage(), 512); //2 is the number of counts
					 start_pwm();					 
					 state = DVR_PLAYING;	// Transition to "recording" state
				 }
				break;
			case DVR_RECORDING:				
				if ( BIT_IS_SET (~PINF, PF6) ) {
					PORTD &= 0b00001111;  // turn all LEDs off
					PORTD |= 0b00010000;  // turn LED1 on					
					pageCount = 1;	// Finish recording last page									
				}
			
				// Write samples to SD card when buffer page is full
				if (newPage) {
					newPage = 0;	// Acknowledge new page flag
					wave_write(buffer_readPage(), 512);
				} else if (stop) {
					// Stop is flagged when the last page has been recorded
					stop = 0;							// Acknowledge stop flag
					wave_write(buffer_readPage(), 512);	// Write final page
					wave_close();						// Finalize WAVE file 
					printf("Recording COMPLETE!\n");	// Print status to console
					state = DVR_STOPPED;				// Transition to stopped state
				}
				break;
			case DVR_PLAYING:
				PORTB |= 0b01000000;
				
				if ( BIT_IS_SET (~PINF, PF6) ) {
					PORTD &= 0b00001111;  // turn other LEDs off
					PORTD |= 0b00010000;  // turn LED1 on
					
					stop = 1;
					newPage = 0;
					stop_pwm();
				}								
				if(newPage){
					newPage = 0;					
					wave_read (buffer_writePage(), 512); //2 is the number of counts
				}
				else if(stop) {
					stop = 0;					
					wave_close (); // close the file after reading
					printf("DONE!");
					state = DVR_STOPPED;				// Transition to stopped state
				}
				
				break;
			default:
				// Invalid state, return to valid idle state (stopped)
				printf("ERROR: State machine in main entered invalid state!\n");
				state = DVR_STOPPED;
				break;

		} // END switch(state)
			
	} // END for(;;)

}

/**
 * ISR: PWM conversion complete
 * 
 */
ISR(TIMER4_OVF_vect) {
	// 	
	if(--data_amount > 0){		
		if(played){
			first_que = buffer_dequeue();
			second_que = buffer_dequeue();				
			first_played = 0;
			second_played = 0;
			played = 0;
		} else {
			if(!first_played) {
				OCR4B = first_que;
				first_played = 1;
			} else if(!second_played) {
				OCR4B = (first_que+second_que)/2.0;
				second_played = 1;
			}
			else {
				OCR4B = second_que;				
				played = 1;
			}
		}	
	} else {
		newPage = 0;
		stop = 1;
		stop_pwm();			
	}
}
