/***********************************************************************
 * Backlight daemon for Zipit Z2 revision 2012-02-20
 *
 * 2010-04-26 Tal Stokes
 * 2012-02-20 Joe Honold <mozzwald@mozzwald.com>
 * 2012-03-08 mcmajeres  <mark@engine12.com> -- added seperate timer for keyboard -- works in conjunction with ebindkeys
 * 2012-03-23 mcmajeres      -- changed blanking control to the framebuffer and added posix timers -- still have a polling loop :(
 *
 * Manages screen and keyboard backlights:
 * -turns them off when lid is closed and on when lid is opened
 * -dims them when on battery and brightens them when on AC (or reverse)
 * -respects changes made to brightness while running
 *
 * Thanks to Russell K. Davis for lid switch and AC
 * plug detection routines, and help
 *
 ***********************************************************************/
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h> /* close */


#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>

#define MAP_SIZE 4096UL

#define GPIO 98	/* lid switch */
#define GPIO_BASE 0x40E00000 /* PXA270 GPIO Register Base */

typedef unsigned long u32;

int regoffset(int gpio) {
	if (gpio < 32) return 0;
	if (gpio < 64) return 4;
	if (gpio < 96) return 8;
	return 0x100;
}

int gpio_read(void *map_base, int gpio) {
	volatile u32 *reg = (u32*)((u32)map_base + regoffset(gpio));
	return (*reg >> (gpio&31)) & 1;
}

#define LID_CLOSED  0
#define LID_OPEN    1
#define LID_UNKNOWN 255
int lidstate() {
	int fd;
	int retval;
	void *map_base;

	fd = open("/dev/mem", O_RDONLY | O_SYNC);
   	if (fd < 0) {printf("Please run as root"); exit(1);}

    	map_base = mmap(0, MAP_SIZE, PROT_READ, MAP_SHARED, fd, GPIO_BASE);
	if(map_base == (void *) -1) exit(255);

	switch(gpio_read(map_base,98))
	{
		case 0: /* lid is closed */
			retval = LID_CLOSED;
			break;

		case 1: /* lid is open */
			retval = LID_OPEN;
			break;

		default:
			retval = LID_UNKNOWN;

	}

	if(munmap(map_base, MAP_SIZE) == -1) exit(255) ;
	close(fd);
	return retval;
}

#define PWR_BATTERY 0
#define PWR_AC_CORD 1
#define PWR_UNKNOWN 255
int powerstate() {
        int fd;
        int retval;
        void *map_base;

        fd = open("/dev/mem", O_RDONLY | O_SYNC);
        if (fd < 0) {printf("Please run as root"); exit(1);}

        map_base = mmap(0, MAP_SIZE, PROT_READ, MAP_SHARED, fd, GPIO_BASE);
        if(map_base == (void *) -1) exit(255);

        switch(gpio_read(map_base,0))
        {
                case 0: /* battery */
                        retval = PWR_BATTERY;
			break;

                case 1: /* mains */
                        retval = PWR_AC_CORD;
                        break;

                default:
			retval = PWR_UNKNOWN;
        }

        if(munmap(map_base, MAP_SIZE) == -1) exit(255) ;
        close(fd);
        return retval;
}

//on --> 1  off --> 0
int lightswitch(int onoroff) {	//turns backlight power on or off
	static const char screenfile[] = "/sys/class/backlight/pwm-backlight.0/bl_power";
	static const char keyfile[] = "/sys/class/backlight/pwm-backlight.1/bl_power";
	FILE *scr = fopen(screenfile, "w");
	FILE *key = fopen(keyfile, "w");
	int success;
	if (scr != NULL && key != NULL) {
		char buf [5];
		sprintf(buf, "%i", (onoroff == 0?1:0));	
		fputs(buf, scr);
		fputs(buf, key);
		fclose(scr);
		fclose(key);
		success = 1;
	} else {
	success = 0;
	}
	return success;
}

int lcdb(int scrbr) {	//set screen to given brightness
	static const char screenfile[] = "/sys/class/backlight/pwm-backlight.0/brightness";
	FILE *scr = fopen(screenfile, "w");

	int success;
	if (scr != NULL) {
		char scrbuf [5];
//		itoa(scrbr, scrbuf, 10);
		sprintf(scrbuf, "%i", scrbr);
		fputs(scrbuf, scr);
		fclose(scr);
		success = 1;
	} else {
	success = 0;
	}
	return success;
}

int keyb(int keybr) {	//set keyboard to given brightness
	static const char keyfile[] = "/sys/class/backlight/pwm-backlight.1/brightness";
	FILE *key = fopen(keyfile, "w");
	int success;
	if (key != NULL) {
		char keybuf [5];
//		itoa(keybr, keybuf, 10);
		sprintf(keybuf, "%i", keybr);
		fputs(keybuf, key);
		fclose(key);
		success = 1;
	} else {
	success = 0;
	}
	return success;
}


int getscr(void) {	//return current brightness of screen
	static const char screenfile[] = "/sys/class/backlight/pwm-backlight.0/actual_brightness";
	FILE *scr = fopen(screenfile, "r");
	int scrbr;
	if (scr != NULL) {
		char buf [5];
		scrbr = atoi(fgets(buf, sizeof buf, scr));
		fclose(scr);
	}
	return scrbr;
}

int getkeyb(void) {	//return current brightness of keyboard
	static const char keyfile[] = "/sys/class/backlight/pwm-backlight.1/actual_brightness";
	FILE *key = fopen(keyfile, "r");
	int keybr;
	if (key != NULL) {
		char buf [5];
		keybr = atoi(fgets(buf, sizeof buf, key));
		fclose(key);
	}
	return keybr;
}

#define CLOCKID CLOCK_MONOTONIC
#define SIG SIGALRM
#define KEYS_TIMER 101
#define LCD_TIMER  201
#define KEYS_TIMEOUT 500  //5secs
#define LCD_TIMEOUT  3000 //30 secs  

//on --> 0  off --> 1
#define KEYS_ON  0
#define KEYS_OFF 1
void keysOn() {	//turns backlight power on or off

	sigset_t mask;
	/* Block timer signal temporarily */
	sigemptyset(&mask);
	sigaddset(&mask, SIG);
	if (sigprocmask(SIG_SETMASK, &mask, NULL) == -1)
	   perror("sigprocmask");

	FILE *key = fopen("/sys/class/backlight/pwm-backlight.1/bl_power", "w");

	if (key != NULL) {
		char buf [5];
		sprintf(buf, "%i", KEYS_ON);	
		fputs(buf, key);
		fclose(key);
	}	

   /* Unlock the timer signal, so that timer notification nan be delivered */
   if (sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1)
	   perror("sigprocmask");
}

inline void keysOff() {	//turns backlight power on or off

	FILE *key = fopen("/sys/class/backlight/pwm-backlight.1/bl_power", "w");

	if (key != NULL) {
		char buf [5];
		sprintf(buf, "%i", KEYS_OFF);	
		fputs(buf, key);
		fclose(key);
	}	
}

int GetKeyPressed(void) {	//return value of /tmp/keypress
	static const char keyfile[] = "/tmp/keypressed";
	FILE *key = fopen(keyfile, "r+");
	int keybr;
	if (key != NULL) {
		char buf [5];
		keybr = atoi(fgets(buf, sizeof buf, key));
		//reset the value to zero		
		if(keybr != 0) fputs("0", key);
		
		fclose(key);
	}
	return keybr;
}




static timer_t keys_timerid = 0;
static timer_t lcd_timerid = 0;
static unsigned int bScreenOff = 0;

inline void screenOn(){	
	sigset_t mask;
	/* Block timer signal temporarily */
	sigemptyset(&mask);
	sigaddset(&mask, SIG);
	if (sigprocmask(SIG_SETMASK, &mask, NULL) == -1)
	   perror("sigprocmask");
	
	if(bScreenOff){//turn it back on
		system("echo 0 >/sys/class/graphics/fb0/blank");
		bScreenOff = 0;
	}

   /* Unlock the timer signal, so that timer notification nan be delivered */
   if (sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1)
	   perror("sigprocmask");
}
	
inline void screenOff(){	
	system("echo 1 >/sys/class/graphics/fb0/blank");
	bScreenOff = 1;
}

static void onTimer(int sig, siginfo_t *si, void *uc)
{
	switch(si->si_int){
		case KEYS_TIMER:
			keysOff();	
			break;
	
		case LCD_TIMER:
			screenOff();
			break;

		default:
			break;
	}
}
   
     
#define errExit(msg)    do { perror(msg); return 0; \
                        } while (0)

timer_t create_timer(int timerName, unsigned int freq_msecs)
{
    struct itimerspec 	its;
						its.it_value.tv_sec = freq_msecs / 100;
						its.it_value.tv_nsec = 0;
						its.it_interval.tv_sec = 0;
						its.it_interval.tv_nsec = 0;
						
	struct sigevent 	sev;
						sev.sigev_notify = SIGEV_SIGNAL;
						sev.sigev_signo = SIG;
						sev.sigev_value.sival_int = timerName;

	struct sigaction 	sa;
						sa.sa_flags = SA_SIGINFO;
						sa.sa_sigaction = onTimer;

	timer_t timerid=0;

	/* Establish handler for timer signal */
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIG, &sa, NULL) == -1)
		errExit("sigaction");

	/* Create the timer */
	if (timer_create(CLOCKID, &sev, &timerid) == -1)
		errExit("timer_create");
	
   /* Start the timer */
   if (timer_settime(timerid, 0, &its, NULL) == -1)
         errExit("timer_settime");
   
   return timerid;
}
							
int set_timer(timer_t timerid, unsigned int freq_msecs)
{
    struct itimerspec 	its;
						its.it_value.tv_sec = freq_msecs / 100;
						its.it_value.tv_nsec = 0;
						its.it_interval.tv_sec = 0;
						its.it_interval.tv_nsec = 0;
	
   /* Start the timer */
   if (timer_settime(timerid, 0, &its, NULL) == -1)
         errExit("timer_settime");
   
   return 1;
}
   
int main(int argc, char **argv) {
	int lid = LID_UNKNOWN; 
	int power = PWR_UNKNOWN;
	int brightscr=1023;	//screen brightness on AC (default at start)
	int brightkeyb=512;	//keyboard brightness on AC
	int dimscr=512;		//screen brightness on battery
	int dimkeyb=300;		//keyboard brightness on battery
	int keyTimer = 0;

	system("echo -ne \"\\033[9;0]\" >/dev/tty0");	//set screen blank to never -- it doesn't blank the frame buffer so don't use it
					
	//intialize the keyboard lights	
	keyb(powerstate() == PWR_AC_CORD?brightkeyb:dimkeyb);			

	keys_timerid = create_timer(KEYS_TIMER, KEYS_TIMEOUT);
	lcd_timerid = create_timer(LCD_TIMER, LCD_TIMEOUT);

	while(1) {		//main loop
			
		if(!power && GetKeyPressed() == 1) //if there is power the timers are not active, so nothing needs to be done
		{	
			//a key has been pressed -- reset the timers
			set_timer(keys_timerid, KEYS_TIMEOUT);
			set_timer(lcd_timerid, LCD_TIMEOUT);
			
			screenOn();
		}


		if(lid != lidstate()) 
		{	
			lid = lidstate();
			lightswitch(lid);
		}
		
		if(power != powerstate())
		{	//there has been a change in the powerstate
			keyTimer = 0;
			power = powerstate();
			if (power) {	//AC is plugged in
		//		system("echo -ne \"\\033[9;0]\" >/dev/tty0");	//set screen blank to never
				screenOn();
				
				set_timer(keys_timerid, 0);
				set_timer(lcd_timerid, 0);	
			
				dimscr = getscr();      //store current brightness  as dim values
				dimkeyb = getkeyb();
				keysOn();
				lcdb(brightscr);
				keyb(brightkeyb);	//and brighten lights
			}
			else{		//AC is unplugged
		//		system("echo -ne \"\\033[9;1]\" >/dev/tty0");	//set screen blank to 1 minutes

				set_timer(keys_timerid, KEYS_TIMEOUT);
				set_timer(lcd_timerid, LCD_TIMEOUT);	
			
				brightscr = getscr();   //store current brightness as bright
				brightkeyb = getkeyb();
				lcdb(dimscr);
				keyb(dimkeyb);	//and dim lights
			}
		}
			
		sleep(1);		//wait one second
	}
}









