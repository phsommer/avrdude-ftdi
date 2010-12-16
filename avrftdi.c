/*
 * avrftdi - extension for avrdude
 * Copyright (C) 2007 Hannes Weisbach
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * Interface to the MPSSE Engine of FTDI Chips using libftdi.
 */
#include "ac_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>

#include "avrdude.h"
#include "avr.h"
#include "pgm.h"
#include "avrftdi.h"

#ifdef HAVE_LIBUSB
#ifdef HAVE_LIBFTDI

#include <ftdi.h>
#include <usb.h>


static struct ftdi_context ftdic;
static uint16_t pin_value, pin_direction, pin_inversion;

static void buf_dump(unsigned char *buf, int len, char *desc, int offset, int width)
{
	int i;
	fprintf(stdout, "%s begin:\n", desc);
	for (i = 0; i < offset; i++)
		fprintf(stdout, "%02x ", buf[i]);
	fprintf(stdout, "\n");
	for (i++; i <= len; i++) {
		fprintf(stdout, "%02x ", buf[i-1]);
		if((i-offset) != 0 && (i-offset)%width == 0) fprintf(stdout, "\n");
	}
	fprintf(stdout, "%s end\n", desc);
}

static int avrftdi_get_command(AVRPART *p, AVRMEM *m, unsigned char cmd, unsigned char *ret)
{
	if (m->op[cmd] == NULL) {
		fprintf(stderr, "%s failure: %s command not defined for %s\n", progname, &cmd, p->desc);
		exit(1);
	} else
		avr_set_bits(m->op[cmd], ret);
	return 0;
}

static int set_frequency(uint32_t freq)
{
	uint32_t divisor;
	uint8_t buf[3];

	/* divisor on 6000000 / freq - 1 */
	divisor = (6000000 / freq) - 1;
	if (divisor < 0) {
		fprintf(stderr, "%s failure: Frequency too high (%u > 6 MHz)\n", progname, freq);
		fprintf(stderr, "resetting Frequency to 6MHz\n");
		divisor = 0;
	}

	if (divisor > 65535) {
		fprintf(stderr, "%s failure: Frequency too low (%u < 91.553 Hz)\n", progname, freq);
		fprintf(stderr, "resetting Frequency to 91.553Hz\n");
		divisor = 65535;
	}

	if(verbose)
		fprintf(stderr, "%s info: clock divisor: 0x%04x\n", progname, divisor);

	buf[0] = 0x86;
	buf[1] = (uint8_t)(divisor & 0xff);
	buf[2] = (uint8_t)((divisor >> 8) & 0xff);

	E(ftdi_write_data(&ftdic, buf, 3) < 0);

	return 0;
}

/*   Add a single pin (by pin number) to the pin masks (or to pins), update pinmask[pinfunc]   */

static int add_pin(PROGRAMMER *pgm, int pinfunc)
{
	int pin, inversion_mask;
	
	pin = pgm->pinno[pinfunc];
	if (verbose > 2)
		fprintf(stdout, "add_pin: %d: %d\n", pinfunc, pgm->pinno[pinfunc]);

	/* non-existent definitions, go away */
	if (pin == 0)
		return 0;
	
	/* see if pin should be inverted */
	if(pin & 0x80) {
		pin &= 0x7f;
		inversion_mask = 1 << (pin - 1);
	} else {
		inversion_mask = 0;
	}

	/* check that the pin number is in range */
	if (pin > 11) {
		fprintf(stderr, "%s failure: invalid pin definition (pin no > 11) in config file\n", progname);
		fprintf(stderr, "pin function no %d, pin no: 0x%x\n", pinfunc, pin);
		return -1;
	}

	/* create the mask and check that the pin is available */
	
	
	if (pin_direction & (1 << (pin -1)) ) {
		fprintf(stderr, "%s failure: pin %d has two definitions in config file\n", progname, pin);
		return -1;
	} else {
		pin_direction |= (1 << (pin - 1));
		pin_inversion |= inversion_mask; 
	}

	return 0;
}



/*   Add pins by pin mask    */

static int add_pins(PROGRAMMER *pgm, int pinfunc)
{
	int i;
	uint32_t mask;

	if (verbose > 2)
		fprintf(stdout, "add_pins: %d: %04x\n", pinfunc, pgm->pinno[pinfunc]);

	mask = pgm->pinno[pinfunc];
	if (mask >= 1 << 12) {
		fprintf(stderr, "%s failure: pin list has pins out of range: ", progname);
		mask &= ~(1 << 12) - 1;
	}
	else if (mask & pin_direction) {
		fprintf(stderr, "%s failure: conflicting pins in pin list: ", progname);
		mask &= pin_direction;
	}
	else {
		pin_direction |= (uint16_t)mask;
		return 0;
	}

	/* print the list of pins, if needed */
	i = 0;
	while (mask > 1) {
		if (mask & 1)
			fprintf(stderr, "%d, ", i);
		mask >>= 1;
		i++;
	}
	if (mask > 0)
		fprintf(stderr, "%d\n", i); 
	return -1;
}

static int set_pin(int pin, int value)
{
	unsigned char buf[6];

	if(pin_inversion & (1 << (pin -1))) {
		value = !value;
	}
	
	/* set bits depending on value */
	pin_value ^= (-value ^ pin_value) & (1 << (pin - 1)); 

	if(verbose)
		fprintf(stdout, "%s info: direction: 0x%04x, value: 0x%04x, inversion: 0x%04x\n", progname, pin_direction, pin_value, pin_inversion);

	buf[0] = 0x80;
	buf[1] = pin_value & 0xff;
	buf[2] = pin_direction & 0xff;
	buf[3] = 0x82;
	buf[4] = (pin_value >> 8) & 0xff;
	buf[5] = (pin_direction >> 8) & 0xff;

	E(ftdi_write_data(&ftdic, buf, 6) != 6);

	if (verbose > 4)
		printf("%02x %02x %02x %02x %02x %02x\n", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
	
	return 0;
}
static int set_led_pgm(struct programmer_t * pgm, int value)
{
	return set_pin(pgm->pinno[PIN_LED_PGM], value);
}

static int set_led_rdy(struct programmer_t * pgm, int value)
{
	return set_pin(pgm->pinno[PIN_LED_RDY], value);
}

static int set_led_err(struct programmer_t * pgm, int value)
{
	return set_pin(pgm->pinno[PIN_LED_ERR], value);
}

static int set_led_vfy(struct programmer_t * pgm, int value)
{
	return set_pin(pgm->pinno[PIN_LED_VFY], value);
}

static int avrftdi_transmit(unsigned char mode, unsigned char *cmd, unsigned char *data, int buf_size)
{
	int k = 0;
	int n;
	unsigned char buf[4 + buf_size];

	if (mode & TX) {
		buf[0] = mode;
		buf[1] = ((buf_size - 1) & 0xff);
		buf[2] = (((buf_size - 1) >> 8) & 0xff);

		memcpy(buf + 3, cmd, buf_size);
		buf[buf_size + 3] = 0x87;

		if(verbose >= 3) {
			buf_dump(buf, buf_size + 4, "\ntransmit o_buf", 3, 16);
		}
		
		E(ftdi_write_data(&ftdic, buf, buf_size + 4) != buf_size + 4);
	}
	
	if (mode & RX) {
		memset(buf, 0, sizeof(buf));
		do {
			n = ftdi_read_data(&ftdic, buf + k, buf_size - k);
			E(n < 0);
			k += n;
			if(verbose >= 3 && n != 0)
				buf_dump(buf, k, "\ntransmit i_buf", 0, 16);
		} while (k < buf_size);
		
		memcpy(data, buf, buf_size);
	}

	return k;
}

static int avrftdi_open(PROGRAMMER * pgm, char *port)
{
	int vid, pid;

	/* use vid/pid in following priority: config, defaults. cmd-line is currently not supported */

	if(pgm->usbvid)
		vid = pgm->usbvid;
	else
		vid = 0x0403;

	if(pgm->usbpid)
		pid = pgm->usbpid;
	else
		pid = 0x6010;

	if (SCK != (1 << (pgm->pinno[PIN_AVR_SCK] - 1))
		|| PDO != (1 << (pgm->pinno[PIN_AVR_MISO] - 1))
		|| PDI != (1 << (pgm->pinno[PIN_AVR_MOSI] - 1))) {
		fprintf(stderr, "%s failure: pinning for FTDI MPSSE must be:\n"
			"\tSCK: 1, PDI: 2, PDO: 3(is: %d,%d,%d)\n",
			progname,
			pgm->pinno[PIN_AVR_SCK],
			pgm->pinno[PIN_AVR_MOSI],
			pgm->pinno[PIN_AVR_MISO]);
		fprintf(stderr, "Setting pins accordingly ...\n");
			pgm->pinno[PIN_AVR_SCK] = 1;
			pgm->pinno[PIN_AVR_MOSI] = 2;
			pgm->pinno[PIN_AVR_MISO] = 3;
		
	}

	if (verbose)
		fprintf(stdout, "%s info: reset pin value: %x\n", progname, pgm->pinno[PIN_AVR_RESET]);
	if (pgm->pinno[PIN_AVR_RESET] < 4 || pgm->pinno[PIN_AVR_RESET] == 0) {
		fprintf(stderr, "%s failure: RESET pin clashes with data pin or is not set.\n",
			progname);
		fprintf(stderr, "Setting to default-value 4\n");
		pgm->pinno[PIN_AVR_RESET] = 4;
	}

	pin_direction = (0x3 | (1 << (pgm->pinno[PIN_AVR_RESET] - 1)));

	/* gather the rest of the pins */
	if (add_pins(pgm, PPI_AVR_VCC)) return -1;
	if (add_pins(pgm, PPI_AVR_BUFF)) return -1;
	if (add_pin(pgm, PIN_LED_ERR)) return -1;
	if (add_pin(pgm, PIN_LED_RDY)) return -1;
	if (add_pin(pgm, PIN_LED_PGM)) return -1;
	if (add_pin(pgm, PIN_LED_VFY)) return -1;

	if (verbose) {
		fprintf(stdout, "pin direction mask: %04x\n", pin_direction);
		fprintf(stdout, "pin value mask: %04x\n", pin_value);
	}


	E(ftdi_init(&ftdic) < 0);
	E(ftdi_set_interface(&ftdic, INTERFACE_A) < 0);		/*must be A for mpsse */
	E(ftdi_usb_open(&ftdic, vid, pid) < 0);
	E(ftdi_set_bitmode(&ftdic, pin_direction & 0xff, BITMODE_MPSSE) < 0);	/*set SPI */

	set_frequency(pgm->baudrate ? pgm->baudrate : 150000);

	return 0;
}

static void avrftdi_close(PROGRAMMER * pgm)
{
	unsigned int i, *a = &i;
	
	//Check wether &ftdic was initialized, if not ftdi_deinit() crashes
	E(ftdi_write_data_get_chunksize(&ftdic, a) < 0);

	if( i != 0 ) {
		set_pin(pgm->pinno[PIN_AVR_RESET], ON);
		E(ftdi_usb_close(&ftdic));
		ftdi_deinit(&ftdic);
	}
}


static int avrftdi_initialize(PROGRAMMER * pgm, AVRPART * p)
{
	set_pin(pgm->pinno[PIN_AVR_RESET], OFF);
	set_pin(pgm->pinno[PIN_AVR_SCK], OFF);
	/*use speed optimization with CAUTION*/
	usleep(20 * 1000);

	/*giving rst-pulse of at least 2 avr-clock-cycles, for security (2us @ 1MHz) */
	set_pin(pgm->pinno[PIN_AVR_RESET], ON);
	usleep(20 * 1000);

	/*setting rst back to 0 */
	set_pin(pgm->pinno[PIN_AVR_RESET], OFF);
	/*wait at least 20ms bevor issuing spi commands to avr */
	usleep(20 * 1000);

	return pgm->program_enable(pgm, p);
}

static void avrftdi_disable(PROGRAMMER * pgm)
{
	//if(BUFF)
	//	set_pin(BUFF, OFF);
}

static void avrftdi_enable(PROGRAMMER * pgm)
{
	//if(BUFF)
	//	set_pin(BUFF, ON);
	return;
}

static void avrftdi_display(PROGRAMMER * pgm, const char *p)
{
	return;
}


static int avrftdi_cmd(PROGRAMMER * pgm, unsigned char cmd[4], unsigned char res[4])
{
	return avrftdi_transmit(TRX, cmd, res, sizeof(cmd));
}


static int avrftdi_program_enable(PROGRAMMER * pgm, AVRPART * p)
{
	int i;
	unsigned char buf[4];

	memset(buf, 0, sizeof(buf));

	if (p->op[AVR_OP_PGM_ENABLE] == NULL) {
		fprintf(stderr, "%s failure: Program Enable (PGM_ENABLE) command not defined for %s\n", progname, p->desc);
		exit(1);
	}
	
	avr_set_bits(p->op[AVR_OP_PGM_ENABLE], buf);

	for(i = 0; i < 4; i++) {
		pgm->cmd(pgm, buf, buf);
		if(buf[p->pollindex-1] != p->pollvalue) {
			//try resetting
			set_pin(pgm->pinno[PIN_AVR_RESET], ON);
			usleep(20);
			set_pin(pgm->pinno[PIN_AVR_RESET], OFF);
			avr_set_bits(p->op[AVR_OP_PGM_ENABLE], buf);
		} else
			return 0;
	}
	
	return -1;

}


static int avrftdi_chip_erase(PROGRAMMER * pgm, AVRPART * p)
{
	unsigned char cmd[4];
	unsigned char res[4];

	if (p->op[AVR_OP_CHIP_ERASE] == NULL) {
		fprintf(stderr, "%s failure Chip Erase (CHIP_ERASE) command not defined for %s\n", progname, p->desc);
		return -1;
	}

	memset(cmd, 0, sizeof(cmd));

	avr_set_bits(p->op[AVR_OP_CHIP_ERASE], cmd);
	pgm->cmd(pgm, cmd, res);
	usleep(p->chip_erase_delay);
	pgm->initialize(pgm, p);

	return 0;
}



void avrftdi_initpgm(PROGRAMMER * pgm)
{
	strcpy(pgm->type, "avrftdi");

	/*
	 * mandatory functions
	 */

	pgm->initialize = avrftdi_initialize;
	pgm->display = avrftdi_display;
	pgm->enable = avrftdi_enable;
	pgm->disable = avrftdi_disable;
	pgm->program_enable = avrftdi_program_enable;
	pgm->chip_erase = avrftdi_chip_erase;
	pgm->cmd = avrftdi_cmd;
	pgm->open = avrftdi_open;
	pgm->close = avrftdi_close;
	pgm->read_byte = avr_read_byte_default;
	pgm->write_byte = avr_write_byte_default;

	/*
	 * optional functions
	 */
	
	pgm->rdy_led = set_led_rdy;
	pgm->err_led = set_led_err;
	pgm->pgm_led = set_led_pgm;
	pgm->vfy_led = set_led_vfy;

	
}

#else /*HAVE_LIBFTDI*/

static int avrftdi_noftdi_open (struct programmer_t *pgm, char * name)
{
	fprintf(stderr, "%s: error: no libftdi support. please compile again with libftdi installed.\n",
	  progname);

	exit(1);
}

void usbasp_initpgm(PROGRAMMER * pgm)
{
	strcpy(pgm->type, "avrftdi");
	pgm->open = avrftdi_noftdi_open;
}

#endif  /* HAVE_LIBFTDI */

#else /*HAVE_LIBUSB*/

static int avrftdi_nousb_open (struct programmer_t *pgm, char * name)
{
	fprintf(stderr, "%s: error: no usb support. please compile again with libsusb installed.\n",
	  progname);
	
	exit(1);
}

void avrftdi_initpgm(PROGRAMMER * pgm)
{
	strcpy(pgm->type, "avrftdi");
	pgm->open = avrftdi_noftdi_open;
}

#endif /*HAVE_LIBUSB*/

