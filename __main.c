/**
 * main.c - EGB240 Digital Voice Recorder Skeleton Code
 *
 * This code provides a skeleton implementation of a digital voice 
 * recorder using the Teensy micro controller and QUT TensyBOBv2 
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
 * Version:				v1.3
 *    Date:				28/05/2017
 *  Author:				Mark Broadmeadow
 *  E-mail:				mark.broadmeadow@qut.edu.au
 *	Edited:				Group 420
 *	GitHub Repository:	https://github.com/SmugglerSMR/EGB240_Recorder/
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
#define BIT_IS_SET(byte, bit) (byte & (1 << bit))  // check to see if 
												   //     the bit is set or not

#define TOP 255									   // Init 0xFF 
#define pageSize 512							   // Init Size of the Page

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
volatile uint16_t newPage = 0;		// Flag that indicates a new page 
									//					is available for read/write
volatile uint8_t stop = 0;			// Flag that indicates playback/recording
									//						 is complete

// Personal global variables for playback
volatile uint32_t data_amount = 0;	// Amount of samples used to play
volatile uint8_t played = 1;		// Flag used in interrupt for Ini
volatile uint8_t first_que = 0;		// First sample
volatile uint8_t second_que = 0;	// Next sample
volatile uint8_t first_played = 0;	// Flag indicates if first sample was played
volatile uint8_t second_played = 0;	// Flag indicates if second sample was played
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
	PLLFRQ = 0x6A;				// PLL = 96 MHz, USB = 48 MHz, TIM4 = 64 MHz
}

// Configure system clock for 16 MHz
void clock_init() {
	CLKPR = 0x80;				// Prescaler change enable
	CLKPR = 0x00;				// Prescaler /1, 16 MHz
}

// Initialize buttons and LEDs
void hardware_setup (){
	// clear the bit and set the button pins as input (1= output && 0=input)		
	DDRD |= 0b11110000;			// LEDS
	DDRF &= ~0b11110000;		// Buttons
}

// Initialize PWM state. Sets Prescaler to 8
void set_pwm() {	
	OCR4C = TOP;				// Set top to 0xFF (255)	
	TCCR4B = 0x04;				// Prescaler 8, ~32.5kHz	
	TCCR4A = 0x20;				// Set control register for 0C4B Off
	OCR4B = 0x80;				// Initialize to 50% duty cycle
	TIMSK4 = 0x00;				// Disable interrupt	
	DDRB |= 0b01000000;			// Set pin B6 to output(JOUT)
	TCNT4 = 0x00;				// Reset counter
}
void start_pwm(){
	TIMSK4 = 0x04;				// Enable interrupt
	TCCR4A = 0x21;				// Set control register for 0C4B On
}
void stop_pwm(){
	TIMSK4 = 0x00;				// Disable interrupt
	TCCR4A = 0x20;				// Set control register for 0C4B Off
}
// Initialize DVR subsystems and enable interrupts
void init() {
	cli();						// Disable interrupts
	clock_init();				// Configure clocks
	pll_init();					// Configure PLL (used by Timer4 and USB serial)
	serial_init();				// Initialize USB serial interface (debug)
	timer_init();				// Initialize timer (used by FatFs library)
	hardware_setup();			// Initialize Button with LEDs
	set_pwm();
	buffer_init(pageFull,
				   pageEmpty);  // Initialize circular buffer 
								//				(must specify callback functions)
	adc_init();					// Initialize ADC
	sei();						// Enable interrupts
	
	// Must be called after interrupts are enabled
	wave_init();				// Initialize WAVE file interface
}
/************************************************************************/
/* CALLBACK FUNCTIONS FOR CIRCULAR BUFFER                               */
/************************************************************************/

// CALLED FROM BUFFER MODULE WHEN A PAGE IS FILLED WITH RECORDED SAMPLES
void pageFull() {
	if(!(--pageCount)) {
		// If all pages have been read
		adc_stop();				// Stop recording (disable new ADC conversions)
		stop = 1;				// Flag recording complete
	} else {
		newPage = 1;			// Flag new page is ready to write to SD card
	}
}

// CALLED FROM BUFFER MODULE WHEN A NEW PAGE HAS BEEN EMPTIED
void pageEmpty() {
	if (data_amount > (4*pageSize)) {	// If Data reached final 2 page
		newPage = 1;
	}	
}

/************************************************************************/
/* RECORD/PLAYBACK ROUTINES                                             */
/************************************************************************/

// Initiates a record cycle
void dvr_record() {
	buffer_reset();				// Reset buffer state
	
	pageCount = 305;			// Maximum record time of 10 sec
	newPage = 0;				// Clear new page flag
	
	wave_create();				// Create new wave file on the SD card
	adc_start();				// Begin sampling

	SET_BIT (PORTD, PD1);		// turn on the first led
	PORTD &= 0b00001111;		// turn other LEDs off
}

// Debounce button
void debounce(int p){
	_delay_ms(50);
	while(PINF >> p & 0b1);
	_delay_ms(50);
}

/************************************************************************/
/* MAIN LOOP (CODE ENTRY)                                               */
/************************************************************************/
int main(void) {
	uint8_t state = DVR_STOPPED;// Start DVR in stopped state	
	// Initialization
	init();	
	PORTD &= 0b00001111;		// turn other LEDs off
	// Loop forever (state machine)
	stop_pwm();
    for(;;) {		
		// Switch depending on state
		switch (state) {
			case DVR_STOPPED:				
				if ( BIT_IS_SET (~PINF, PF5 ) ) {			// -----STARTING THE RECORDING----
					PORTD |= 0b10000000;					// Turn LED2 on				
					
					printf("Recording started...");			// Output status to console
					dvr_record();							// Initiate recording
					state = DVR_RECORDING;					// Transition to "recording" state
				 }											// -------------------------------
				 if ( BIT_IS_SET (~PINF, PF4 ) ) {			// -------STARTING PLAYBACK-------
				 	 PORTD &= 0b00001111;					// Turn all LEDs off
					 PORTD |= 0b01000000;					// turn LED3 on
					 
					 printf("Preparing file\n");			// Output status to console
					 buffer_reset();
					 newPage = 0;
					 data_amount = wave_open ()*2+1;		// Open the file to read not VOID function
					 
					 wave_read (buffer_writePage(),
											   pageSize);   // Feel first page with samples
					 wave_read (buffer_writePage(),
											   pageSize);   // Feels second page with samples
					 start_pwm();							// Start PWM					 
					 state = DVR_PLAYING;					// Transition to "Playing" state
				 }											// ----------------------------------
				break;
			case DVR_RECORDING:
				PORTD |= 0b10000000;						// Keeps LED2 turn on
				if ( BIT_IS_SET (~PINF, PF6) ) {			// --- STOP REcording on Button Press--
					PORTD &= 0b00001111;					// Turn all LEDs off
					PORTD |= 0b00010000;					// Turn LED1 on					
					pageCount = 1;							// Finish recording last page									
				}											// ----------------------------------
			
				if (newPage) {								// ---Write samples to SD card when buffer page is full---
					newPage = 0;							// Acknowledge new page flag
					wave_write(buffer_readPage(), pageSize);
				} else if (stop) {							// ---Stop is flagged when the last page has been recorded---
					stop = 0;								// Acknowledge stop flag
					wave_write(buffer_readPage(),
												 pageSize);	// Write final page
					wave_close();							// Finalize WAVE file 
					printf("Recording COMPLETE!\n");		// Print status to console
					debunce(PF5);				//===========Test debouncing=======================
					state = DVR_STOPPED;					// Transition to stopped state
				}											// --------------------------------------------------------
				break;
			case DVR_PLAYING:
				PORTB |= 0b01000000;						// Keeps LED3 turn on
				
				if ( BIT_IS_SET (~PINF, PF6) ) {			// ---- Stops PLayback------
					PORTD &= 0b00001111;					// turn other LEDs off
					PORTD |= 0b00010000;					// turn LED1 on
					
					stop = 1;								// Sets stop flag
					newPage = 0;							// Finalize page
					stop_pwm();								// Stops PWM
				}											// --------------------------				
				if(newPage){								// ------Page is reeded
					newPage = 0;					
					wave_read (buffer_writePage(), 
												pageSize);  // Writes next page
				}											//---------------------------
				else if(stop) {								//---- Finalize Playback------
					stop = 0;					
					wave_close ();							// close the file after reading
					printf("DONE!");
					debunce(PF4);				//===========Test debouncing=======================
					state = DVR_STOPPED;					// Transition to stopped state
				}											//-----------------------------
				
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
 * Creates an average value to fill space. (var1+var2)/2 RUns per 3 sample
 */
ISR(TIMER4_OVF_vect) {
	if(--data_amount > 0){								// -----Runs until all samples were played
		if(played){										// True if 2 samples has not been played yet.		
			first_que = buffer_dequeue();				// Stores first value
			second_que = buffer_dequeue();				// Stores second value
			first_played = 0;							// Sets flag for first
			second_played = 0;							// Sets flag for second
			played = 0;									// Values are stored, start play
		} else {										// ------Plays samples of creates one--------
			if(!first_played) {							// ------Play first sample-------------------
				OCR4B = first_que;
				first_played = 1;
			} else if(!second_played) {					// ------Play average sample-----------------
				OCR4B = (first_que+second_que)/2.0;
				second_played = 1;
			} else {									// ------Play second sample-------------------
				OCR4B = second_que;				
				played = 1;
			}											// --------------------------------------------
		}												// --------------------------------------------
	} else {											// ----- File has been played------------------
		newPage = 0;									// Empties the page
		stop = 1;										// Stops playback run
		stop_pwm();										// Stops PWM
	} // END data_amount								// --------------------------------------------
	
} // END Interrupt
