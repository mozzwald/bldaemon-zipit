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
//#define DEBUG
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h> /* close */
#include <linux/input.h>
#include <string.h>
#include <pthread.h>
#include "confuse.h"

#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>

#define MAP_SIZE 4096UL
#define GPIO_BASE 0x40E00000 /* PXA270 GPIO Register Base */
#define LID_CLOSED  0
#define LID_OPEN    1
#define LID_UNKNOWN 255
#define PWR_BATTERY 0
#define PWR_AC_CORD 1
#define PWR_UNKNOWN 255

typedef unsigned long u32;

/* Stuff for watching keyboard events */
pthread_t get_keypressed;
int wasKeyPressed = 0;
pthread_mutex_t lock;
static int evfd;

/* Default settings in case we can't parse config file */
long int brightscr=8;			//screen brightness on AC (default at start)
long int brightkeyb=4;			//keyboard brightness on AC
long int dimscr=4;				//screen brightness on battery
long int dimkeyb=1;				//keyboard brightness on battery
long int lcdtimeout = 6000;		//screen blank timeout on Battery
long int keytimeout = 500;		//keys off timeout on Battery
int keyTimer = 0;
char keybuf[99];
char scrbuf[99];
static char *scrbfile = NULL;	//path to screen backlight
static char *keybfile = NULL;	//path to keyboard backlight
static char *evdev = NULL;		//path to keyboard input device

/* Config file variables */
cfg_opt_t opts[] = {
	CFG_SIMPLE_INT("brightkeyb", &brightkeyb),
	CFG_SIMPLE_INT("dimkeyb", &dimkeyb),
	CFG_SIMPLE_INT("brightscr", &brightscr),
	CFG_SIMPLE_INT("dimscr", &dimscr),
	CFG_SIMPLE_INT("lcdtimeout", &lcdtimeout),
	CFG_SIMPLE_INT("keytimeout", &keytimeout),
	CFG_SIMPLE_STR("scrbfile", &scrbfile),
	CFG_SIMPLE_STR("keybfile", &keybfile),
	CFG_SIMPLE_STR("evdev", &evdev),
	CFG_END()
};
cfg_t *cfg;

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

int lidstate() {
	int fd;
	int retval;
	void *map_base;

	fd = open("/dev/mem", O_RDONLY | O_SYNC);
   	if (fd < 0) {printf("Please run as root\n"); exit(1);}

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

int powerstate() {
        int fd;
        int retval;
        void *map_base;

        fd = open("/dev/mem", O_RDONLY | O_SYNC);
        if (fd < 0) {printf("Please run as root\n"); exit(1);}

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
	sprintf(scrbuf, "%s%s", cfg_getstr(cfg, "scrbfile"), "bl_power");
	FILE *scr = fopen(scrbuf, "w");
	sprintf(keybuf, "%s%s", cfg_getstr(cfg, "keybfile"), "bl_power");
	FILE *key = fopen(keybuf, "w");
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
	sprintf(scrbuf, "%s%s", cfg_getstr(cfg, "scrbfile"), "brightness");
	FILE *scr = fopen(scrbuf, "w");

	int success;
	if (scr != NULL) {
		char scrbuf [5];
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
	sprintf(keybuf, "%s%s", cfg_getstr(cfg, "keybfile"), "brightness");
	FILE *key = fopen(keybuf, "w");

	int success;
	if (key != NULL) {
		char keybuf [5];
		sprintf(keybuf, "%i", keybr);
		fputs(keybuf, key);
		fclose(key);
		success = 1;
	} else {
	success = 0;
	}
	return success;
}

int toggleLED(int bOn) {
	static const char keyfile[] = "/sys/class/leds/z2:green:wifi/brightness";
	FILE *key = fopen(keyfile, "w");
	int success;

	if (key != NULL) {
		char keybuf [5];
		sprintf(keybuf, "%i", bOn==0?0:255);
		fputs(keybuf, key);
		fclose(key);
		success = 1;
	} else {
	success = 0;
	}
	return success;
}


int getscr(void) {	//return current brightness of screen
	sprintf(scrbuf, "%s%s", cfg_getstr(cfg, "scrbfile"), "brightness");
	FILE *scr = fopen(scrbuf, "r");

	int scrbr;
	if (scr != NULL) {
		char buf [5];
		scrbr = atoi(fgets(buf, sizeof buf, scr));
		fclose(scr);
	}
	return scrbr;
}

int getkeyb(void) {	//return current brightness of keyboard
	sprintf(keybuf, "%s%s", cfg_getstr(cfg, "keybfile"), "brightness");
	FILE *key = fopen(keybuf, "r");

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
#define KEYS_TIMER 		101
#define LCD_TIMER  		201
#define POWER_TIMER  	301
#define POWER_TIMEOUT 	300 //30 secs

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

	sprintf(keybuf, "%s%s", cfg_getstr(cfg, "keybfile"), "bl_power");
	FILE *key = fopen(keybuf, "w");

	if (key != NULL) {
		char buf [5];
		sprintf(buf, "%i", KEYS_ON);
		fputs(buf, key);
		fclose(key);
	}

   /* Unlock the timer signal, so that timer notification can be delivered */
   if (sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1)
	   perror("sigprocmask");
}

static inline void keysOff() {	//turns backlight power on or off
	sprintf(keybuf, "%s%s", cfg_getstr(cfg, "keybfile"), "bl_power");
	FILE *key = fopen(keybuf, "w");

	if (key != NULL) {
		char buf [5];
		sprintf(buf, "%i", KEYS_OFF);
		fputs(buf, key);
		fclose(key);
	}
}

void* GetKeyPressed(void *arg) {
	ssize_t n;
	struct input_event ev;
	
    while (1) {
        n = read(evfd, &ev, sizeof ev);
        if (n == (ssize_t)-1) {
            if (errno == EINTR)
                continue;
            else
                break;
        } else
        if (n != sizeof ev) {
            errno = EIO;
            break;
        }
        if (ev.type == EV_KEY && ev.value >= 0 && ev.value <= 2){
			pthread_mutex_lock(&lock);
			wasKeyPressed = 1;
			pthread_mutex_unlock(&lock);
        }

    }
    fflush(stdout);
    fprintf(stderr, "%s.\n", strerror(errno));

	return NULL;
}

static timer_t keys_timerid = 0;
static timer_t lcd_timerid = 0;
static timer_t power_timerid = 0;

static unsigned int bScreenOff = 0;

static inline void screenOn(){
	sigset_t mask;
	/* Block timer signal temporarily */
	sigemptyset(&mask);
	sigaddset(&mask, SIG);
	if (sigprocmask(SIG_SETMASK, &mask, NULL) == -1)
	   perror("sigprocmask");

	if(bScreenOff){//turn it back on
		FILE *fblank = fopen("/sys/class/graphics/fb0/blank", "w");
		fputs("0", fblank);
		fclose(fblank);
		bScreenOff = 0;
	}

   /* Unlock the timer signal, so that timer notification nan be delivered */
   if (sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1)
	   perror("sigprocmask");
}
	
static inline void screenOff(){
	FILE *fblank = fopen("/sys/class/graphics/fb0/blank", "w");
	fputs("1", fblank);
	fclose(fblank);
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

		case POWER_TIMER:
			system("/usr/local/sbin/onPowerDown");
		
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
							
static int set_timer(timer_t timerid, unsigned int freq_msecs)
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

volatile static int powerDown = 0;
volatile static int suspend = 0;
volatile static int newMsg = 0;
volatile static int valkeyb = 0;
volatile static int	flashKeyBrd = 0;


void _powerDown(int sig)
{
	// SIGUSR1 handler
	powerDown = 1;
}


void _suspend(int sig)
{
	// SIGUSR2 handler
	suspend = 1;
}

void _newMsg(int sig)
{
	newMsg = 1;
	flashKeyBrd = 1;
}

				
int main(int argc, char **argv) {
	int lid = LID_UNKNOWN;
	int power = PWR_UNKNOWN;

	/* Get config file path from command line if provided */
	char *configfile;
	if(argv[1] != NULL)
		configfile = argv[1];
	else
		configfile = "/etc/bldaemon.conf";

	/* Parse config file if available */
	int p;
	cfg = cfg_init(opts, 0);
	p = cfg_parse(cfg, configfile);
	if(p == CFG_FILE_ERROR)
			printf("Unable to open config file (%s), Using defaults!\n", configfile);
	if(p == CFG_PARSE_ERROR)
			printf("Unable to parse config file (%s), Using defaults!\n", configfile);
	if(p != CFG_SUCCESS){
		scrbfile = "/sys/class/backlight/pxabus:display-backlight/";
		keybfile = "/sys/class/backlight/pxabus:keyboard-backlight/";
		evdev = "/dev/input/event0";
	}

	#ifdef DEBUG
		printf("scrbfile: %s\n", scrbfile);
		printf("keybfile: %s\n", keybfile);
		printf("evdev: %s\n", evdev);
		printf("brightkeyb: %ld\n", brightkeyb);
		printf("brightscr: %ld\n", brightscr);
		printf("dimkeyb: %ld\n", dimkeyb);
		printf("dimscr: %ld\n", dimscr);
		printf("keytimeout: %ld\n", keytimeout);
		printf("lcdtimeout: %ld\n", lcdtimeout);
	#endif

	/* open input event device */
	evfd = open(evdev, O_RDONLY);
		if (evfd == -1) {
		fprintf(stderr, "Cannot open %s: %s.\n", evdev, strerror(errno));
		exit(255);
	}

	/* setup pthread for key presses */
	pthread_mutex_init(&lock, NULL);
	pthread_create(&get_keypressed, NULL, &GetKeyPressed, NULL);

	//set screen blank to never -- it doesn't blank the frame buffer so don't use it
	system("echo -ne \"\\033[9;0]\" >/dev/tty0");
	
	//first screen blanking is always white so blank it and turn it back on once
	screenOff();
	screenOn();

	//initialize the timers
	keys_timerid = create_timer(KEYS_TIMER, keytimeout);
	lcd_timerid = create_timer(LCD_TIMER, lcdtimeout);
	power_timerid = create_timer(POWER_TIMER, 0);

	//intialize the keyboard and screen backlights
	keyb(powerstate() == PWR_AC_CORD?brightkeyb:dimkeyb);
	keysOn();

	signal(SIGQUIT, _powerDown);
	signal(SIGINT, _suspend);
	signal(SIGUSR1, _newMsg);

	while(1) {		//main loop
		//if there is power the timers are not active, so nothing needs to be done			
		if( (!power || newMsg) && wasKeyPressed )
		{
			if(!power){
				//a key has been pressed -- reset the timers
				set_timer(keys_timerid, keytimeout);
				set_timer(lcd_timerid, lcdtimeout);
				wasKeyPressed = 0;
				screenOn();
				keysOn();
			}

			if(newMsg && valkeyb){
				//stop flashing the keyboard
				keyb(valkeyb);
				toggleLED(0);
				valkeyb = 0;
				newMsg = 0;
			}
		}

		if(newMsg){
			if(!valkeyb)
				valkeyb = getkeyb();
			
			toggleLED(flashKeyBrd);

			if(flashKeyBrd){
				keyb(valkeyb);
				flashKeyBrd=0;
			}
			else{
				keyb(0);
				flashKeyBrd=1;
			}
		}
		
		if(powerDown && !suspend)
		{
			set_timer(power_timerid, POWER_TIMEOUT);
			powerDown = 0;
		}		

		if(suspend)
		{
			set_timer(power_timerid, 0);

			suspend = 0;
			powerDown = 0;
		}		

		if(power != powerstate())
		{	//there has been a change in the powerstate
			keyTimer = 0;
			power = powerstate();
			if (power) {	//AC is plugged in
				screenOn();
				
				set_timer(keys_timerid, 0);
				set_timer(lcd_timerid, 0);
			
				//dimscr = getscr();      //store current brightness  as dim values
				//dimkeyb = getkeyb();
				keysOn();
				lcdb(brightscr);
				keyb(brightkeyb);	//and brighten lights
			}
			else{		//AC is unplugged

				set_timer(keys_timerid, keytimeout);
				set_timer(lcd_timerid, lcdtimeout);
			
				//brightscr = getscr();   //store current brightness as bright
				//brightkeyb = getkeyb();
				lcdb(dimscr);
				keyb(dimkeyb);	//and dim lights
			}
		}

		if(lid != lidstate())
		{
			lid = lidstate();
			lightswitch(lid);
		}

		sleep(1);		//wait one second
	}
}
