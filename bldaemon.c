/***********************************************************************
 * Backlight daemon for Zipit Z2 revision 2012-02-20
 *
 * 2010-04-26 Tal Stokes
 * 2012-02-20 Joe Honold <mozzwald@mozzwald.com>
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
		case 0:
			/* lid is closed */
			retval = 0;
			break;
		case 1:
			/* lid is open */
			retval = 1;
			break;
		default:
			/* will never reach here unless something has gone terribly wrong */
			retval = 255;
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
        if (fd < 0) {printf("Please run as root"); exit(1);}

        map_base = mmap(0, MAP_SIZE, PROT_READ, MAP_SHARED, fd, GPIO_BASE);
        if(map_base == (void *) -1) exit(255);

        switch(gpio_read(map_base,0))
        {
                case 0: /* battery */
                        retval = 0;
                        break;
                case 1:
                        /* mains */
                        retval = 1;
                        break;
                default:
                        /* will never reach here unless something has gone terribly wrong */
                        retval = 255;
        }

        if(munmap(map_base, MAP_SIZE) == -1) exit(255) ;
        close(fd);
        return retval;
}

int lightswitch(int onoroff) {	//turns backlight power on or off
	static const char screenfile[] = "/sys/class/backlight/pwm-backlight.0/bl_power";
	static const char keyfile[] = "/sys/class/backlight/pwm-backlight.1/bl_power";
	FILE *scr = fopen(screenfile, "w");
	FILE *key = fopen(keyfile, "w");
	int success;
	if (scr != NULL && key != NULL) {
		char buf [5];
		sprintf(buf, "%i", onoroff);	//WARNING: opposite of what you might expect - 1 is off and 0 is on
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

int bright(int scrbr, int keybr) {	//set screen and keyboard to given brightness
	static const char screenfile[] = "/sys/class/backlight/pwm-backlight.0/brightness";
	static const char keyfile[] = "/sys/class/backlight/pwm-backlight.1/brightness";
	FILE *scr = fopen(screenfile, "w");
	FILE *key = fopen(keyfile, "w");
	int success;
	if (scr != NULL && key != NULL) {
		char scrbuf [5];
		char keybuf [5];
//		itoa(scrbr, scrbuf, 10);
//		itoa(keybr, keybuf, 10);
		sprintf(scrbuf, "%i", scrbr);
		sprintf(keybuf, "%i", keybr);
		fputs(scrbuf, scr);
		fputs(keybuf, key);
		fclose(scr);
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


int main(int argc, char **argv) {
	int lid = lidstate();		//init to sane values
	int power = powerstate();
	int oldlid;
	int oldpower;
	int brightscr=1023;	//screen brightness on AC (default at start)
	int brightkeyb=512;	//keyboard brightness on AC
	int dimscr=512;		//screen brightness on battery
	int dimkeyb=0;	//keyboard brightness on battery

	if(power){	//set values based on AC before looping
		system("setterm -blank 0 >/dev/tty0");     //set screen blank to never
		dimscr = getscr();      //store current brightness  as dim values
		dimkeyb = getkeyb();
		bright(brightscr, brightkeyb);  //and brighten lights
	}else{
		system("setterm -blank 5 >/dev/tty0");     //set screen blank to 5
		brightscr = getscr();   //store current brightness as bright
		brightkeyb = getkeyb();
		bright(dimscr, dimkeyb);        //and dim lights
	}

	while(1) {			//main loop
		oldlid = lid;		//store current values for comparison
		oldpower = power;

		while(lid == oldlid && power == oldpower) { //loop until something changes
			sleep(1);		//wait one second
			lid = lidstate();		//update lid status
			power = powerstate();	//update AC status
		}
		// Loop exited. Something has changed.

		if (lid && !oldlid) lightswitch(0);	//lid is open and wasn't, turn on
		if (!lid && oldlid) lightswitch(1); //lid is closed and wasn't, turn off
		if (power && !oldpower) {	//AC is plugged in and wasn't
			system("setterm -blank 0 >/dev/tty0");	//set screen blank to never
			dimscr = getscr();	//store current brightness as dim values
			dimkeyb = getkeyb();
			bright(brightscr, brightkeyb);	//and brighten lights
		}
		if (!power && oldpower) {		//AC isn't plugged in and was
			system("setterm -blank 5 >/dev/tty0");	//set screen blank to 5 minutes
			brightscr = getscr();	//store current brightness as bright
			brightkeyb = getkeyb();
			bright(dimscr, dimkeyb);	//and dim lights
		}

	}
}
