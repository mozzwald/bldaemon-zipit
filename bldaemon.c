/***********************************************************************
 * Backlight daemon for Zipit Z2 revision 2012-02-20
 *
 * 2010-04-26 Tal Stokes
 * 2012-02-20 Joe Honold <mozzwald@mozzwald.com>
 * 2012-03-08 mcmajeres -- added seperate timer for keyboard -- works in conjunction with ebindkeys
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

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h> /* close */

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

//on --> 0  off --> 1
#define KEYS_ON  0
#define KEYS_OFF 1
int keyPower(int onoroff) {	//turns backlight power on or off
	FILE *key = fopen("/sys/class/backlight/pwm-backlight.1/bl_power", "w");

	if (key != NULL) {
		char buf [5];
		sprintf(buf, "%i", onoroff);	
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

#define TIMEOUT 5
int main(int argc, char **argv) {
	int lid = LID_UNKNOWN; 
	int power = PWR_UNKNOWN;
	int brightscr=1023;	//screen brightness on AC (default at start)
	int brightkeyb=512;	//keyboard brightness on AC
	int dimscr=512;		//screen brightness on battery
	int dimkeyb=300;		//keyboard brightness on battery
	int keyTimer = 0;

	//intialize the keyboard lights	
	keyb(powerstate() == PWR_AC_CORD?brightkeyb:dimkeyb);			

	while(1) {		//main loop
			
		if(GetKeyPressed() == 1)//a key has been pressed -- reset the timer
			keyTimer = 0;
							
		else if(keyTimer == TIMEOUT && !power)
			keyPower(KEYS_OFF);			
		
		//don't let the timer roll
		if(keyTimer <= TIMEOUT)									
			keyTimer++;
		

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
				system("echo -ne \"\\033[9;0]\" >/dev/tty0");	//set screen blank to never
		dimscr = getscr();      //store current brightness  as dim values
		dimkeyb = getkeyb();
				keyPower(KEYS_ON);
				lcdb(brightscr);
				keyb(brightkeyb);	//and brighten lights
			}
			else{		//AC is unplugged
				system("echo -ne \"\\033[9;1]\" >/dev/tty0");	//set screen blank to 1 minutes
		brightscr = getscr();   //store current brightness as bright
		brightkeyb = getkeyb();
				lcdb(dimscr);
				keyb(dimkeyb);	//and dim lights
			}
	}

			sleep(1);		//wait one second
	}
}
