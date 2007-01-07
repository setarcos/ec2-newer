/** EC2 Driver Library
  *
  *
  *   Copyright (C) 2005 by Ricky White
  *   rickyw@neatstuff.co.nz
  *
  *   This program is free software; you can redistribute it and/or modify
  *   it under the terms of the GNU General Public License as published by
  *   the Free Software Foundation; either version 2 of the License, or
  *   (at your option) any later version.
  *
  *   This program is distributed in the hope that it will be useful,
  *   but WITHOUT ANY WARRANTY; without even the implied warranty of
  *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  *   GNU General Public License for more details.
  *
  *   You should have received a copy of the GNU General Public License
  *   along with this program; if not, write to the
  *   Free Software Foundation, Inc.,
  *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
  */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>			// UNIX standard function definitions
#include <fcntl.h>			// File control definitions
#include <errno.h>			// Error number definitions
#include <termios.h>		// POSIX terminal control definitions
#include <sys/ioctl.h>
#include "ec2drv.h"
#include "config.h"
#include "boot.h"
#include "c2_mode.h"
#include "jtag_mode.h"

#include <usb.h>			// Libusb header
#include <sys/ioctl.h>

#define MAJOR_VER 0
#define MINOR_VER 4
#define NEW_FLASH

#define MIN_EC2_VER 0x13	///< Minimum usable EC2 Firmware version
#define MAX_EC2_VER 0x13	///< Highest tested EC2 Firmware version, will try and run with newer versions
#define MIN_EC3_VER 0x07	///< Minimum usable EC3 Firmware version
#define MAX_EC3_VER 0x0a	///< Highest tested EC3 Firmware version, will try and run with newer versions




#define SFR_PAGE_REG	0x84		///< SFR Page selection register




/** Retrieve the ec2drv library version
  * \returns the version.  upper byte is major version, lower byte is minor
  */
uint16_t ec2drv_version()
{
	return (MAJOR_VER<<8) | MINOR_VER;
}

typedef struct
{
	char *tx;
	int tlen;
	char *rx;
	int rlen;
} EC2BLOCK;

// Internal functions
static void	init_ec2( EC2DRV *obj );
static BOOL	txblock( EC2DRV *obj, EC2BLOCK *blk );
//static BOOL	trx( EC2DRV *obj, char *txbuf, int txlen, char *rxexpect, int rxlen );
static void	print_buf( char *buf, int len );
static int	getNextBPIdx( EC2DRV *obj );
static int	getBP( EC2DRV *obj, uint32_t addr );
static BOOL	setBpMask( EC2DRV *obj, int bp, BOOL active );

inline static void update_progress( EC2DRV *obj, uint8_t percent );
static uint8_t sfr_fixup( uint8_t addr );

BOOL ec2_write_flash_jtag( EC2DRV *obj, char *buf,
						   uint32_t start_addr, uint32_t len );
uint16_t device_id( EC2DRV *obj );

static BOOL check_flash_range( EC2DRV *obj, uint32_t addr, uint32_t len );
static BOOL check_scratchpad_range( EC2DRV *obj, uint32_t addr, uint32_t len );


/** Suspend the target core.
	\param obj			Object to act on.
*/
void ec2_core_suspend( EC2DRV *obj )
{
	if( obj->mode==JTAG )
		jtag_core_suspend(obj);
	else if( obj->mode==C2 )
		c2_core_suspend(obj);
}

// PORT support
static BOOL open_port( EC2DRV *obj, const char *port );
static void rx_flush( EC2DRV *obj );
static void tx_flush( EC2DRV *obj );
static void close_port( EC2DRV *obj );
static void DTR( EC2DRV *obj, BOOL on );
static void RTS( EC2DRV *obj, BOOL on );


static BOOL open_ec3( EC2DRV *obj, const char *port );
static void close_ec3( EC2DRV *obj );
static BOOL write_usb( EC2DRV *obj, char *buf, int len );
static BOOL write_usb_ch( EC2DRV *obj, char ch );
static BOOL read_usb( EC2DRV *obj, char *buf, int len );
static int read_usb_ch( EC2DRV *obj );
void close_ec3( EC2DRV *obj );


/** Connect to the EC2/EC3 device.
  *
  * This will perform any initialisation required to bring the device into
  * an active state.
  * this function must be called before any other operation
  *
  * \param port name of the linux device the EC2 is connected to, eg "/dev/ttyS0"
  *				or "/dev/ttyUSB0" for an EC2 on a USB-serial converter
  *				or USB for an EC3, or to specify an exact device USB::XXXXXXXX
  *				where XXXXXXXX is the device serial number.
  * \returns TRUE on success
  */
BOOL ec2_connect( EC2DRV *obj, const char *port )
{
	DUMP_FUNC();
	int ec2_sw_ver;
	const char *lport = port;
	uint16_t idrev;
	strncpy( obj->port, port, sizeof(obj->port) );
	
	if( obj->mode == AUTO )
	{
		printf(	"*********************************************************************\n"
				"* WARNING: Auto detection of mode may cause initialisation sequence *\n"
				"* to differ significantly from the SiLabs IDE.                      *\n"
				"* In the case of problems specify --mode=C2 or --mode=JTAG          *\n"
				"*********************************************************************\n\n");
	}
	
	obj->progress = 0;
	obj->progress_cbk = 0;
	if( strncmp(lport,"USB",3)==0 )
	{
		// USB mode, EC3
		obj->dbg_adaptor = EC3;
		if( lport[3]==':' )
		{
			lport = lport+4;	// point to the remainder ( hopefully the serial number of the adaptor )
		}
		else if( strlen(lport)==3 )
		{
			lport = 0;
		}
		else
			return FALSE;
	}
	else
		obj->dbg_adaptor = EC2;
	
	
	if( !open_port( obj, lport) )
	{
		printf("Coulden't connect to %s\n", obj->dbg_adaptor==EC2 ? "EC2" : "EC3");
		return FALSE;
	}
	obj->connected=TRUE;
	// call new jtag init
	if(obj->mode==JTAG)
		return ec2_connect_jtag( obj, port );
	
	ec2_reset( obj );
	if( obj->dbg_adaptor==EC2 )
	{
		if( !trx( obj,"\x55",1,"\x5A",1 ) )
			return FALSE;
		boot_get_version( obj );
		boot_select_flash_page(obj,0x03);
	} 
	else if( obj->dbg_adaptor==EC3 )
	{
		boot_get_version(obj);
		boot_select_flash_page(obj,0x0c);
	}
	
	ec2_sw_ver = boot_run_app(obj);
	
	if( obj->dbg_adaptor==EC2 )
	{
		printf("EC2 firmware version = 0x%02x\n",ec2_sw_ver);
		//if( ec2_sw_ver != 0x12  && ec2_sw_ver != 0x13 && ec2_sw_ver != 0x14)
		//{
		//	printf("Incompatible EC2 firmware version, version 0x12 required\n");
		//	return FALSE;
		//}
		if( ec2_sw_ver < MIN_EC2_VER )
		{
			printf("Incompatible EC2 firmware version,\n"
					"Versions between 0x%02x and 0x%02x inclusive are reccomended\n"
					"Newer vesrions may also be tried and will just output a warning that they are untested\n", MIN_EC2_VER, MAX_EC2_VER);
			exit(-1);
		}
		else if( ec2_sw_ver > MAX_EC2_VER )
		{
			printf("Warning: this version is newer than the versions tested by the developers,\n");
			printf("Please report success / failure and version via ec2drv.sf.net\n");
		}
	}
	else if( obj->dbg_adaptor==EC3 )
	{
		printf("EC3 firmware version = 0x%02x\n",ec2_sw_ver);
		if( ec2_sw_ver < MIN_EC3_VER )
		{
			printf("Incompatible EC3 firmware version,\n"
					"Versions between 0x%02x and 0x%02x inclusive are reccomended\n"
					"Newer vesrions may also be tried and will just output a warning that they are untested\n", MIN_EC2_VER, MAX_EC2_VER);
		}
		else if( ec2_sw_ver > MAX_EC3_VER )
		{
			printf("Warning: this version is newer than the versions tested by the developers,\n");
			printf("Please report success / failure and version via ec2drv.sf.net\n");
		}
	}
	
	if( obj->mode==AUTO )
	{
		// try and figure out what communication the connected device uses
		// JTAG or C2
		idrev=0;
		obj->mode=C2;
		c2_connect_target(obj);
		idrev = device_id( obj );
		if( idrev==0xffff )
		{
			obj->mode = JTAG;			// device most probably a JTAG device
			// On EC3 the simplistic mode change dosen't work,
			// we take the slower approach and restart the entire connection.
			// This seems the most reliable method.
			// If you find it too slow just specify the mode rather than using auto.
//			if( obj->dbg_adaptor==EC3 )
//			{
				printf("NOT C2, Trying JTAG\n");
				ec2_disconnect( obj );
				ec2_connect( obj, obj->port );
				return TRUE;
//			}
//			trx(obj, "\x04",1,"\x0D",1);	// select JTAG mode
//			idrev = device_id( obj );
//			ec2_connect_jtag( obj, obj->port );
#if 0
			if( idrev==0xFF00 )
			{
				printf("ERROR :- Debug adaptor Not connected to a microprocessor\n");
				ec2_disconnect( obj );
				exit(-1);
			}
#endif
		}
	}
	else
	{
		if(obj->mode==JTAG)
			jtag_connect_target(obj);
		else if(obj->mode==C2)
			c2_connect_target(obj);
		
		idrev = device_id( obj );
		if( idrev==0xFF00 || idrev==0xFFFF )
		{
			printf("ERROR :- Debug adaptor Not connected to a microprocessor\n");
			ec2_disconnect( obj );
			exit(-1);
		}
		obj->dev = getDevice( idrev>>8, idrev&0xFF );
		obj->dev = getDeviceUnique( unique_device_id(obj), 0);
		ec2_target_reset( obj );
		return TRUE;
	}
	obj->dev = getDevice( idrev>>8, idrev&0xFF );
	obj->dev = getDeviceUnique( unique_device_id(obj), 0);
//	init_ec2(); Does slightly more than ec2_target_reset() but dosen't seem necessary
	ec2_target_reset( obj );
	return TRUE;
}


BOOL ec2_connect_fw_update( EC2DRV *obj, char *port )
{
	DUMP_FUNC();

	obj->progress = 0;
	obj->progress_cbk = 0;
	if( strncmp(port,"USB",3)==0 )
	{
		// USB mode, EC3
		obj->dbg_adaptor = EC3;
		if( port[3]==':' )
		{
			// get the rest of the string
			port = port+4;	// point to the remainder ( hopefully the serial number of the adaptor )
		}
		else if( strlen(port)==3 )
		{
			port = 0;
		}
		else
			return FALSE;
	}
	else
		obj->dbg_adaptor = EC2;
	
	
	if( !open_port( obj, port) )
	{
		printf("Coulden't connect to %s\n", obj->dbg_adaptor==EC2 ? "EC2" : "EC3");
		return FALSE;
	}
	DUMP_FUNC_END();
	return TRUE;
}


// identify the device, id = upper 8 bites, rev = lower 9 bits
uint16_t device_id( EC2DRV *obj )
{
	DUMP_FUNC();
	char buf[6];
	uint16_t idrev = 0;
	
	if( obj->mode==C2 )
		idrev = c2_device_id(obj);
	else if( obj->mode==JTAG )
		idrev = jtag_device_id(obj);
	return idrev;
}


// identify the device, id = upper 8 bites, rev = lower 9 bits
uint16_t unique_device_id( EC2DRV *obj )
{
	DUMP_FUNC();
	char buf[40];
	uint16_t unique_id=0xffff;	// invalid
	if( obj->mode==C2 )
		unique_id = c2_unique_device_id(obj);
	else if( obj->mode==JTAG )
		unique_id = jtag_unique_device_id(obj);
	
	DUMP_FUNC_END();
	return unique_id;
}

/** Disconnect from the EC2/EC3 releasing the serial port.
	This must be called before the program using ec2drv exits, especially for
	the EC3 as exiting will leave the device in an indeterminant state where
	it may not respond correctly to the next application that tries to use it.
	software retries or replugging the device may bring it back but it is 
	definitly prefered that this function be called.
*/
void ec2_disconnect( EC2DRV *obj )
{
	DUMP_FUNC();
	
	if( obj->connected==TRUE )
	{
		obj->connected = FALSE;
		if( obj->dbg_adaptor==EC3)
		{
			char buf[255];
			int r;
	
			c2_disconnect_target(obj);
			
			usb_control_msg( obj->ec3, USB_TYPE_CLASS + USB_RECIP_INTERFACE, 0x9, 0x340, 0,"\x40\x02\x0d\x0d", 4, 1000);
			r = usb_interrupt_read(obj->ec3, 0x00000081, buf, 0x0000040, 1000);
			
			r = usb_release_interface(obj->ec3, 0);
			assert(r == 0);
			usb_reset( obj->ec3);
			r = usb_close( obj->ec3);
			assert(r == 0);
			DUMP_FUNC_END();
			return;
		}
		else if( obj->dbg_adaptor==EC2)
		{
			DTR( obj, FALSE );
		}
		close_port( obj );
	}
	DUMP_FUNC_END();
}


/** Translates certain special SFR addresses for read and write 
	reading or writing the sfr address as per datasheet returns incorrect
	information.
	These mappings seem necessary due to the way the hardware is implemented.
	The access is the same byte sequence as a normal SFR but the address is
	much lower starting arround 0x20.

	\param addr		Stanbdard SFR address.
	\returns		Translated SFR to address required to access it in HW.
*/
static uint8_t sfr_fixup( uint8_t addr )
{
	DUMP_FUNC();
	switch( addr )
	{
		case 0xD0:	return 0x23;	// PSW
		case 0xE0:	return 0x22;	// ACC
		default:	return addr;
	}
}


/** SFR read command							<br>
  * T 02 02 addr len							<br>
  * len <= 0x0C									<br>
  * addr = SFR address 0x80 - 0xFF				<br>
  *
  * \param buf	Buffer to store the read data
  * \param addr Address to read from, must be in SFR area, eg 0x80 - 0xFF
  */
void ec2_read_sfr( EC2DRV *obj, char *buf, uint8_t addr )
{
	DUMP_FUNC();
	assert( addr >= 0x80 );
	ec2_read_ram_sfr( obj, buf, sfr_fixup( addr ), 1, TRUE );
	DUMP_FUNC_END();
}

/** Write to an SFR (Special Function Register)
	NOTE some SFR's appear to accept writes but do not take any action on the
	heardware.  This seems to be the same SFRs that the SI labs IDE can't make
	change either.

	One possible work arroud is to place a couple of byte program in the top of
	flash and then the CPU state can be saved (via EC2) and then values poked 
	into regs and this code stepped through.  This would mean we could change 
	any sfr provided the user application can spare a few bytes of code memory
	The SFR's that don't write correctly are a subset of the bit addressable ones
	for some of them the SI labs IDE uses a different command.
	This function will add support for knowen alternative access methods as found.

	\param obj	EC2 object to operate on
	\param buf	Buffer containing data to write
	\param addr	Sfr addr to begin writing at, must be in SFR area, eg 0x80 - 0xFF
	\param len	Number of bytes to write.
*/
void ec2_write_sfr( EC2DRV *obj, uint8_t value, uint8_t addr )
{
	DUMP_FUNC();
	assert( addr >= 0x80 );

	if( obj->mode==JTAG )
		jtag_write_sfr( obj, value, sfr_fixup( addr ) );
	else if( obj->mode==C2 )
		c2_write_sfr( obj, value, sfr_fixup( addr ) );
	DUMP_FUNC_END();
}



////////////////////////////////////////////////////////////////////////////////
// Paged SFR Support
////////////////////////////////////////////////////////////////////////////////

/**	Read a paged Special function register.
	\param[in]	obj		EC2 object to operate on
	\param[in]	page	Page number the register resides on (0-255)
	\param[in]	addr	Register address (0x80-0xFF)
	\param[out]	ok		if non zero on return will indicate success/failure
	\returns			value read from the register
*/
uint8_t ec2_read_paged_sfr( EC2DRV *obj, SFRREG sfr_reg, BOOL *ok )
{
	uint8_t cur_page, value;
	
	// Save page register and set new page
	if( obj->dev->has_paged_sfr )
	{
		cur_page = ec2_read_raw_sfr( obj, SFR_PAGE_REG, 0 );
		ec2_write_raw_sfr( obj, SFR_PAGE_REG, sfr_reg.page );
	}
	
	value = ec2_read_raw_sfr( obj, sfr_reg.addr, ok );
	
	// Restore page register
	if( obj->dev->has_paged_sfr )
		ec2_write_raw_sfr( obj, SFR_PAGE_REG, cur_page );

	return value;
}

/**	Write to  a paged Special function register.
	\param[in]	obj		EC2 object to operate on
	\param[in]	page	Page number the register resides on (0-255)
	\param[in]	addr	Register address (0x80-0xFF)
	\param[in]	value	Value to write to register (0-0xff)
	\returns			TRUE on success, FALSE on addr out of range
 */
BOOL ec2_write_paged_sfr(EC2DRV *obj, SFRREG sfr_reg, uint8_t value)
{
	uint8_t cur_page;
	BOOL result;
	
	// Save page register and set new page
	if( obj->dev->has_paged_sfr )
	{
		cur_page = ec2_read_raw_sfr( obj, SFR_PAGE_REG, 0 );
		ec2_write_raw_sfr( obj, SFR_PAGE_REG, sfr_reg.page );
	}
	
	result = ec2_write_raw_sfr( obj, sfr_reg.addr, value );
	
	// Restore page register
	if( obj->dev->has_paged_sfr )
		ec2_write_raw_sfr( obj, SFR_PAGE_REG, cur_page );
	
	return result;
}


/**	Read a Special function register from the current page
	\param[in]	obj		EC2 object to operate on
	\param[in]	addr	Register address (0x80-0xFF)
	\param[out]	ok		if non zero on return will indicate success/failure
	\returns			value read from the register
 */
uint8_t ec2_read_raw_sfr( EC2DRV *obj, uint8_t addr, BOOL *ok )
{
	uint8_t value;
	if( addr>=0x80 )
	{
		ec2_read_sfr( obj, (char*)&value, addr );
		if(ok)	*ok = TRUE;
	}
	else
	{
		value = 0;
		if(ok)	*ok = FALSE;
	}
	return value;
}

/**	Write to a Special function register in the current page
	\param[in]	obj		EC2 object to operate on
	\param[in]	addr	Register address (0x80-0xFF)
	\param[in]	value	Value to write to register (0-0xff)
	\returns			TRUE on success, FALSE on addr out of range
 */
BOOL ec2_write_raw_sfr( EC2DRV *obj, uint8_t addr, uint8_t value )
{
	if( addr>=0x80 )
	{
		ec2_write_sfr( obj, value, addr );
		return TRUE;
	}
	return FALSE;
}


/** Read data from the internal data memory.
	\param obj			EC2 object to operate on
	\param buf			Buffer to store the read data
	\param start_addr	Address to begin reading from, 0x00 - 0xFF
	\param len			Number of bytes to read, 0x00 - 0xFF
*/
void ec2_read_ram( EC2DRV *obj, char *buf, int start_addr, int len )
{
	DUMP_FUNC();

	// special case here and call 
	if( obj->mode == JTAG )
		jtag_read_ram( obj, buf, start_addr, len );
	else if( obj->mode==C2 )
		c2_read_ram( obj, buf, start_addr, len );
	
	DUMP_FUNC_END();
}


/** Read ram or sfr
	Read data from the internal data memory or from the SFR region

	\param obj			EC2 object to operate on
	\param buf			Buffer to store the read data
	\param start_addr	Address to begin reading from, 0x00 - 0xFF
	\param len			Number of bytes to read, 0x00 - 0xFF
	\param sfr 			TRUE to read a special function reg, FALSE to read RAM
  */
void ec2_read_ram_sfr( EC2DRV *obj, char *buf, int start_addr, int len, BOOL sfr )
{
	DUMP_FUNC();
	assert( (int)start_addr+len-1 <= 0xFF ); // -1 to allow reading 1 byte at 0xFF

	if( obj->mode == JTAG )
		jtag_read_ram_sfr( obj, buf, start_addr, len, sfr );
	else if( obj->mode == C2 )
		c2_read_ram_sfr( obj, buf, start_addr, len, sfr );
	
	DUMP_FUNC_END();
}


/** Write data into the micros DATA RAM.
	\param obj			Object to act on.	
	\param buf			Buffer containing data to write to data ram
	\param start_addr	Address to begin writing at, 0x00 - 0xFF
	\param len			Number of bytes to write, 0x00 - 0xFF
	
	\returns 			TRUE on success, otherwise FALSE
 */
BOOL ec2_write_ram( EC2DRV *obj, char *buf, int start_addr, int len )
{
	DUMP_FUNC();
	BOOL r = FALSE;
	
	assert( start_addr>=0 && start_addr<=0xFF );
	// printf("start addr = 0x%02x\n",start_addr);
	if( obj->mode == JTAG )
		r = jtag_write_ram( obj, buf, start_addr, len );
	else if( obj->mode == C2 )
		r = c2_write_ram( obj, buf, start_addr, len );
	else
		r = FALSE;
	DUMP_FUNC_END();
	return r;
}


/** write to targets XDATA address space.

	\param obj			Object to act on.
	\param buf buffer containing data to write to XDATA
	\param start_addr address to begin writing at, 0x00 - 0xFFFF
	\param len Number of bytes to write, 0x00 - 0xFFFF
	
	\returns TRUE on success, otherwise FALSE
*/
BOOL ec2_write_xdata( EC2DRV *obj, char *buf, int start_addr, int len )
{ 
	DUMP_FUNC();
	BOOL r = FALSE;
	if( obj->mode==JTAG )
		r = jtag_write_xdata( obj, buf, start_addr, len );
	else if( obj->mode==C2 )
		r = c2_write_xdata( obj, buf, start_addr, len );
	else
		r = FALSE;
	DUMP_FUNC_END();
	return r;
}


/** Read len bytes of data from the target
	starting at start_addr into buf

	\param obj			Object to act on.
	\param buf buffer to recieve data read from XDATA
	\param start_addr address to begin reading from, 0x00 - 0xFFFF
	\param len Number of bytes to read, 0x00 - 0xFFFF
	
	\returns TRUE on success, otherwise FALSE
*/
void ec2_read_xdata( EC2DRV *obj, char *buf, int start_addr, int len )
{
	DUMP_FUNC();
	if( obj->mode==JTAG )
		jtag_read_xdata( obj, buf, start_addr, len );
	else if( obj->mode==C2 )
		c2_read_xdata( obj, buf, start_addr, len );
}



////////////////////////////////////////////////////////////////////////////////
// Flash Access routines
////////////////////////////////////////////////////////////////////////////////


/** Read from Flash memory (CODE memory)
	@NOTE This function no longer supports high virtual addresses for the
		  scratchpad, use the scratchpad functions.

	\param obj			Object to act on.
	\param buf			Buffer to recieve data read from CODE memory
	\param start_addr	Address to begin reading from, 0 - 0x1FFFF
	\param len			Number of bytes to read, 0 - 0x1FFFF
	\returns			TRUE on success, otherwise FALSE
*/
BOOL ec2_read_flash( EC2DRV *obj, uint8_t *buf, uint32_t start_addr, int len )
{
	DUMP_FUNC();
	BOOL r = FALSE;
	
	if(!check_flash_range( obj, start_addr, len )) return FALSE;
	
	if( obj->mode==JTAG )
		r = jtag_read_flash( obj, buf, start_addr, len, FALSE );
	else if( obj->mode==C2 )
		r = c2_read_flash( obj, buf, start_addr, len );
	DUMP_FUNC_END();
	return r;
}

// These registers for the F120
SFRREG SFR_SFRPAGE	= { 0x0, 0x84 };
SFRREG SFR_FLSCL	= { 0x0, 0xb7 };
SFRREG SFR_CCH0LC	= { 0xf, 0xa3 };
SFRREG SFR_OSCICN	= { 0xf, 0x8a };
SFRREG SFR_CLKSEL	= { 0xf, 0x97 };		// in F120, not present in F020	
SFRREG SFR_CCH0CN	= { 0xf, 0xa1 };
// The F020 has different registers and no paged sfrs, for F040 has CLKSEL at same number but no pagesand also pas pages (clksel not present on F020 but ther on F040 and F120
/// @FIXME: We need some way of knowing about the SFR's on the various devices
///			Mabye parse the provided header files but they don't include pages
///			We only need a few of the registers so maybe just add them to the 
///			device_table spreadsheet and add them to the structure.



/** Write to flash memory
	This function assumes the specified area of flash is already erased
	to 0xFF before it is called.

	Writes to a location that already contains data will only be successfull
	in changing 1's to 0's.

	\param obj			Object to act on.
	\param buf			Buffer containing data to write to CODE
	\param start_addr	Address to begin writing at, 0x00 - 0x1FFFF
	\param len			Number of bytes to write, 0x00 - 0xFFFF

	\returns			TRUE on success, otherwise FALSE
*/
BOOL ec2_write_flash( EC2DRV *obj, uint8_t *buf, uint32_t start_addr, int len )
{
	DUMP_FUNC();
	BOOL r;
	if(!check_flash_range( obj, start_addr, len )) return FALSE;
	
	if( obj->mode==C2 )
		r = c2_write_flash( obj, buf, start_addr, len );
	else
		r = jtag_write_flash( obj, buf, start_addr, len );
	DUMP_FUNC_END();
	return r;
}


/** This variant of writing to flash memory (CODE space) will erase sectors
	before writing.

	\param obj			Object to act on.
	\param buf			Buffer containing data to write to CODE
	\param start_addr	Address to begin writing at, 0x00 - 0x1FFFF
	\param len			Number of bytes to write, 0x00 - 0xFFFF

	\returns			TRUE on success, otherwise FALSE
*/
BOOL ec2_write_flash_auto_erase( EC2DRV *obj, uint8_t *buf,
								 uint32_t start_addr, int len )
{
	DUMP_FUNC();
	if(!check_flash_range( obj, start_addr, len )) return FALSE;
	
	if( obj->mode==JTAG )
		return jtag_write_flash_block( obj, start_addr, buf, len, FALSE,FALSE);
	
	uint16_t first_sector = start_addr / obj->dev->flash_sector_size;
	uint32_t end_addr = start_addr + len - 1;
	uint16_t last_sector = end_addr / obj->dev->flash_sector_size;
	uint16_t sector_cnt = last_sector - first_sector + 1;
	int i;

	// Erase sectors involved
	for( i=first_sector; i<=last_sector; i++ )
	{
		ec2_erase_flash_sector( obj, i*obj->dev->flash_sector_size  );
	}
	ec2_write_flash( obj, buf, start_addr, len );	// @FIXME why is this broken? is this comment meeningful any more?
	
	return TRUE;	///< @TODO check to successful erase
}


/** This variant of writing to flash memory (CODE space) will read all sector
	content before erasing and will merge changes over the existing data
	before writing.
	This is slower than the other methods in that it requires a read of the
	sector first.  also blank sectors will not be erased again

	JTAG mode does this by default so not a big loss.	

	\param obj			Object to act on.
	\param buf			Buffer containing data to write to CODE
	\param start_addr	Address to begin writing at, 0 - 0x1FFFF
	\param len			Number of bytes to write, 0 - 0x1FFFF

	\returns			TRUE on success, otherwise FALSE
*/
BOOL ec2_write_flash_auto_keep( EC2DRV *obj, uint8_t *buf,
								uint32_t start_addr, int len )
{
	DUMP_FUNC();
	BOOL ok;
	if(!check_flash_range( obj, start_addr, len )) return FALSE;
	
	if( obj->mode==JTAG )
	{
		ok = jtag_write_flash_block( obj, start_addr, buf, len, TRUE, FALSE );
	}
	else
	{
		int first_sector = start_addr/obj->dev->flash_sector_size;
		int first_sec_addr = first_sector*obj->dev->flash_sector_size;
		int end_addr = start_addr + len - 1;
		int last_sector = end_addr/obj->dev->flash_sector_size;
		int sector_cnt = last_sector - first_sector + 1;
		int i,j;
	
		uint8_t *tbuf = malloc(obj->dev->flash_size);
		if(tbuf==0)
			return FALSE;
		
		// read in all sectors that are affected
		ec2_read_flash( obj, tbuf, first_sec_addr,
						sector_cnt*obj->dev->flash_sector_size );
	
		// erase nonblank sectors
		for( i=0; i<sector_cnt; i++)
		{
			j = 0;
			while( j<0x200 )
			{
				if( (unsigned char)tbuf[i*0x200+j] != 0xFF )
				{
					// not blank, erase it
					ec2_erase_flash_sector( obj,
						first_sec_addr + i * obj->dev->flash_sector_size );
					break;
				}
				j++;
			}
		}
	
		// merge data then write
		memcpy( tbuf + ( start_addr - first_sec_addr ), buf, len );
		
		ok = ec2_write_flash( obj, tbuf, first_sec_addr,
							sector_cnt*obj->dev->flash_sector_size );
		free(tbuf);
	}
	return ok;
}


/** Erase all User CODE memory (Flash) in the device.

	\param obj			Object to act on.
  */
void ec2_erase_flash( EC2DRV *obj )
{
	DUMP_FUNC();
	if( obj->mode==C2 )
		c2_erase_flash(obj);
	else if( obj->mode==JTAG )
		jtag_erase_flash(obj);
	DUMP_FUNC_END();
}

/** Erase a single sector of flash memory.

	\param obj			Object to act on.
	\param sect_addr	Base address of sector to erase.  
						Does not necessarilly have to be the base address but
						any	address within the sector is equally valid.
*/
void ec2_erase_flash_sector( EC2DRV *obj, uint32_t sect_addr )
{
	DUMP_FUNC();
	if(!check_flash_range( obj, sect_addr, obj->dev->flash_sector_size) )
		return;	// failure
	if( obj->mode == JTAG )
	{
		jtag_erase_flash_sector( obj, sect_addr, FALSE );
	}	// end JTAG
	else if( obj->mode == C2 )
	{
		c2_erase_flash_sector( obj, sect_addr, FALSE );
	}	// End C2
}

/** Read from the scratchpad area in flash.

	\param obj			Object to act on.
	\param buf			Buffer to recieve data read from scratchpad
	\param start_addr	Address to begin reading from (scratchpad starts at 0)
	\param len			Number of bytes to read
	\returns			TRUE on success, otherwise FALSE
*/
BOOL ec2_read_flash_scratchpad( EC2DRV *obj, uint8_t *buf,
								uint32_t start_addr, int len )
{
	DUMP_FUNC();
	if( check_scratchpad_range( obj, start_addr, len ) )
	{
		if( obj->mode==JTAG )
			return jtag_read_flash( obj, buf, start_addr, len,TRUE );
	}
	return FALSE;
}

/** Write to the flash scratchpad.
	The locations being modified must have been erased first or be 
	having their values burn't down.
	
	\param obj			Object to act on.
	\param buf			Buffer containing data to write.
	\param start_addr	Address to begin writing (scratchpad addrs start at 0)
	\param len			Number of bytes to write
	\returns			TRUE on success, FALSE otherwise
*/
BOOL ec2_write_flash_scratchpad( EC2DRV *obj, uint8_t *buf, 
								 uint32_t start_addr, int len )
{
	DUMP_FUNC();
	if(!check_scratchpad_range( obj, start_addr, len ))
		return FALSE;
	if( obj->mode==JTAG )
		return jtag_write_flash_block( obj, start_addr, buf, len, TRUE, TRUE );
}

/** Write to the flash scratchpad with merge.
	Write th the scratchpad a block of bytes while preserving all other bytes
	in the page.  This function will wrewrite the entire page if necessary.

	\param obj			Object to act on.
	\param buf			Buffer containing data to write.
	\param start_addr	Address to begin writing (scratchpad addrs start at 0)
	\param len			Number of bytes to write
	\returns			TRUE on success, FALSE otherwise
 */
BOOL ec2_write_flash_scratchpad_merge( EC2DRV *obj, uint8_t *buf,
                                       uint32_t start_addr, int len )
{
	DUMP_FUNC();
	BOOL result;
	
	uint8_t *mbuf = malloc(obj->dev->scratchpad_len);
	if(!mbuf)
		return FALSE;	
	if( obj->mode==JTAG )
	{
		result = jtag_write_flash_block(obj,start_addr,buf,len,TRUE,TRUE);
		goto done;
	}
	else
	{
		/// @todo	add erase only when necessary checks
		update_progress( obj, 0 );
		ec2_read_flash_scratchpad( obj, mbuf, 0, obj->dev->scratchpad_len );
		memcpy( &mbuf[start_addr], buf, len );	// merge in changes
		update_progress( obj, 45 );
		ec2_erase_flash_scratchpad( obj );
		update_progress( obj, 55 );
		ec2_write_flash_scratchpad( obj, mbuf, 0, obj->dev->scratchpad_len );
		update_progress( obj, 100 );
	}
done:
	free(mbuf);
	return result;
}

/** Erase all scratchpad sectors.

	\param obj			Object to act on.
	\returns			TRUE on success, FALSE on failure
*/
BOOL ec2_erase_flash_scratchpad( EC2DRV *obj )
{
	DUMP_FUNC();
	uint8_t num_sec = obj->dev->scratchpad_len/obj->dev->scratchpad_sector_size;
	int i;
	BOOL ok = TRUE;
	for( i=0; i<num_sec; i++ )
	{
		ok &= ec2_erase_flash_scratchpad_sector( obj,
			obj->dev->scratchpad_start + i*obj->dev->scratchpad_sector_size );
	};
	DUMP_FUNC_END();
	return ok;
}


/** Erase a single scratchpad sector.

	\param obj			Object to act on.
	\param sector_addr	Start address of sector to erase
	\returns			TRUE on success, FALSE on failure
*/
BOOL ec2_erase_flash_scratchpad_sector( EC2DRV *obj, uint32_t sector_addr )
{
//	printf("erasing scratchpad sector at addr=0x%05x\n",sector_addr);
	return jtag_erase_flash_sector( obj, sector_addr, TRUE );
}



////////////////////////////////////////////////////////////////////////////////
// 8051 register accesses
////////////////////////////////////////////////////////////////////////////////


/** Read the currently active set of R0-R7
  * the first returned value is R0
  * \note This needs more testing, seems to corrupt R0
  * \param buf buffer to reciere results, must be at least 8 bytes only
  */
void read_active_regs( EC2DRV *obj, char *buf )
{
	DUMP_FUNC();
	char psw;
	int addr;
	// read PSW
	ec2_read_sfr( obj, &psw, 0xD0 );
	printf( "PSW = 0x%02x\n",psw );

	// determine correct address
	addr = ((psw&0x18)>>3) * 8;
	printf("address = 0x%02x\n",addr);
	ec2_read_ram( obj, buf, addr, 8 );

	// R0-R1
	write_port( obj, "\x02\x02\x24\x02", 4 );
	read_port( obj, &buf[0], 2 );
}

/** Read the targets program counter
  *
  * \returns current address of program counter (16-bits)
  */
uint16_t ec2_read_pc( EC2DRV *obj )
{
	DUMP_FUNC();
	unsigned char buf[2];

	if( obj->mode==JTAG )
	{
		write_port( obj, "\x02\x02\x20\x02", 4 );
		read_port(  obj, (char*)buf, 2 );
	}
	else if( obj->mode==C2 )
	{
		write_port( obj, "\x28\x20\x02", 3 );
		read_port(  obj, (char*)buf, 2 );
	}
	return ((buf[1]<<8) | buf[0]);
}

void ec2_set_pc( EC2DRV *obj, uint16_t addr )
{
	DUMP_FUNC();
	char cmd[4];
	if( obj->mode==JTAG )
	{
		cmd[0] = 0x03;
		cmd[1] = 0x02;
		cmd[2] = 0x20;
		cmd[3] = addr&0xFF;
		trx( obj, cmd, 4, "\x0D", 1 );
		cmd[2] = 0x21;
		cmd[3] = (addr>>8)&0xFF;
		trx( obj, cmd, 4, "\x0D", 1 );
	}
	else if( obj->mode==C2 )
	{
		cmd[0] = 0x29;
		cmd[1] = 0x20;
		cmd[2] = 0x01;					// len
		cmd[3] = addr & 0xff;			// low byte addr
		trx( obj, cmd, 4,"\x0d", 1 );
		cmd[1] = 0x21;
		cmd[3] = addr>>8;				// high byte
		trx( obj, cmd, 4, "\x0d", 1 );
	}
}


/** Cause the processor to step forward one instruction
  * The program counter must be setup to point to valid code before this is
  * called. Once that is done this function can be called repeatedly to step
  * through code.
  * It is likely that in most cases the debugger will request register dumps
  * etc between each step but this function provides just the raw step
  * interface.
  * 
  * \returns instruction address after the step operation
  */
uint16_t ec2_step( EC2DRV *obj )
{
	DUMP_FUNC();
	char buf[2];
	
	if( obj->mode==JTAG )
	{
		trx( obj, "\x09\x00", 2, "\x0d", 1 );
		trx( obj, "\x13\x00", 2, "\x01", 1 );		// very similar to 1/2 a target_halt command,  test to see if stopped...
		
		write_port( obj, "\x02\x02\x20\x02", 4 );
		read_port(  obj, buf, 2 );
		return buf[0] | (buf[1]<<8);
	}
	else if( obj->mode==C2 )
	{
		trx( obj, "\x26", 1, "\x0d", 1 );
		return ec2_read_pc(obj);
	}
	return 0;	// Invalid mode
}

/** Start the target processor running from the current PC location
  *
  * \returns TRUE on success, FALSE otherwise
  */
BOOL ec2_target_go( EC2DRV *obj )
{
	DUMP_FUNC();
	BOOL r = FALSE;
	if( obj->mode==JTAG )
	{
		r = jtag_target_go(obj);
	}
	else if( obj->mode==C2 )
		r = c2_target_go(obj);
	
	DUMP_FUNC_END();
	return r;
}

/** Poll the target to determine if the processor has halted.
	The halt may be caused by a breakpoint or the ec2_target_halt() command.
	
	For run to breakpoint it is necessary to call this function regularly to
	determine when the processor has actually come accross a breakpoint and
	stopped.

	Recommended polling rate every 250ms.

	\param obj			Object to act on.
	\returns 			TRUE if processor has halted, FALSE otherwise.
*/
BOOL ec2_target_halt_poll( EC2DRV *obj )
{
	DUMP_FUNC();
	BOOL r = FALSE;
	
	if( obj->mode==JTAG )
		r = jtag_target_halt_poll(obj);
	else if( obj->mode==C2 )
		r = c2_target_halt_poll(obj);
	
	DUMP_FUNC_END();
	return r;
}

/** Cause target to run until the next breakpoint is hit.
  * \note this function will not return until a breakpoint it hit.
  * 
  * \returns Adderess of breakpoint at which the target stopped
  */
uint16_t ec2_target_run_bp( EC2DRV *obj, BOOL *bRunning )
{
	DUMP_FUNC();
	int i;
	ec2_target_go( obj );
	if( obj->dbg_adaptor )		// @FIXME: which debug adapter?
	{
		trx( obj, "\x0C\x02\xA0\x10", 4, "\x00\x01\x00", 3 );
		trx( obj, "\x0C\x02\xA1\x10", 4, "\x00\x00\x00", 3 );
		trx( obj, "\x0C\x02\xB0\x09", 4, "\x00\x00\x01", 3 );
		trx( obj, "\x0C\x02\xB1\x09", 4, "\x00\x00\x01", 3 );
		trx( obj, "\x0C\x02\xB2\x0B", 4," \x00\x00\x20", 3 );
	}
	
	// dump current breakpoints for debugging
	for( i=0; i<4;i++)
	{
		if( getBP( obj, obj->bpaddr[i] )!=-1 )
			printf("bpaddr[%i] = 0x%05x\n",i,(unsigned int)obj->bpaddr[i]);
	}
	
	while( !ec2_target_halt_poll( obj )&&(*bRunning) )
		usleep(250);
	return ec2_read_pc( obj );
}

/** Request the target processor to stop
  * the polling is necessary to determine when it has actually stopped
  */
BOOL ec2_target_halt( EC2DRV *obj )
{
	DUMP_FUNC();
	int i;
	BOOL r = FALSE;
	
	if( obj->mode==JTAG )
		r = jtag_target_halt(obj);
	else if( obj->mode==C2 )
		r = c2_target_halt(obj);
	return TRUE;
	// loop allows upto 8 retries 
	// returns 0x01 of successful stop, 0x00 otherwise suchas already stopped	
	for( i=0; i<8; i++ )
	{
		if( ec2_target_halt_poll( obj ) )
			return TRUE;	// success
	}
	printf("ERROR: target would not stop after halt!\n");
	return r;
}


/** Rest the target processor
  * This reset is a cut down form of the one used by the IDE which seems to 
  * read 2 64byte blocks from flash as well.
  * \todo investigate if the additional reads are necessary
  */
BOOL ec2_target_reset( EC2DRV *obj )
{
	DUMP_FUNC();
	BOOL r = FALSE;

	if( obj->mode == JTAG )
		r = jtag_target_reset(obj);
	else if( obj->mode==C2 )
		r = c2_target_reset(obj);
	
	DUMP_FUNC_END();
	return r;
}

/** Read the lock byte on single lock devices such as the F310.
	\returns read lock byte of devices with 1 lock byte
*/
uint8_t flash_lock_byte( EC2DRV *obj )
{
	DUMP_FUNC();
	if( obj->dev->lock_type==FLT_SINGLE || obj->dev->lock_type==FLT_SINGLE_ALT)
	{
		return 0;	/// @TODO implement
	}
	else
		return 0;	// oops device dosen't have a single lock byte
}

/** Read the flash read lock byte
	\returns read lock byte of devices with 2 lock bytes
*/
uint8_t flash_read_lock( EC2DRV *obj )
{
	DUMP_FUNC();
	if( obj->dev->lock_type==FLT_RW || obj->dev->lock_type==FLT_RW_ALT )
	{
		return 0;	/// @TODO implement
	}
	return 0;
}

/** Read the flash write/erase lock
	\returns the write/erase lock byte
*/
uint8_t flash_write_erase_lock( EC2DRV *obj )
{
	DUMP_FUNC();
	if( obj->dev->lock_type==FLT_RW || obj->dev->lock_type==FLT_RW_ALT )
	{
		/// @TODO implement
		
	}
	return 0;
}



/** Checks if all addresses in the range specified are valid.
	the reserved flash area is considered invalid.
	\param obj		EC2DRV object to test address range for.
	\param addr		Address block to test starts at
	\param len		number of bytes long the block to test is.
	\returns		TRUE if all addresses in range are valid, FALSE otherwise.
 */
static BOOL check_flash_range( EC2DRV *obj, uint32_t addr, uint32_t len )
{
	uint32_t bottom, top;
	bottom = addr;
	top = bottom+len-1;
	
	// is block outside flash area for this device? device flash area ?
	if( !((bottom<(obj->dev->flash_size-1)) &&
			  (top<(obj->dev->flash_size-1) )) )
		return FALSE;
	
	// I'm tired so simple scan for test,
	// There is a better way.
	uint32_t i;
	for( i=bottom; i<=top; i++)
	{
		if( i>=obj->dev->flash_reserved_bottom &&
				  i<=obj->dev->flash_reserved_top )
		{
			printf("ERROR: attempt to write to reserved flash area!\n");
			return FALSE;	// in reserved area
		}
	}
	return TRUE;
}

/** Checks if all addresses in the range specified are valid for scratchpad.
	\param obj		EC2DRV object to test address range for.
	\param addr		Address block to test starts at
	\param len		number of bytes long the block to test is.
	\returns		TRUE if all addresses in range are valid, FALSE otherwise.
 */
static BOOL check_scratchpad_range( EC2DRV *obj, uint32_t addr, uint32_t len )
{
	uint32_t top = addr+len-1;
	uint32_t scratchpad_top = /*obj->dev->scratchpad_start*/ +
							  obj->dev->scratchpad_len - 1;
	
	if(	( obj->dev->has_scratchpad	&&
		( addr >= 0 ))				&&
	    ( addr <= scratchpad_top )	&&
		( top <= scratchpad_top ) ) 
	{
		return TRUE;
	}
	printf("ERROR: attempt to access non exsistant scratchpad area\n");
	return FALSE;	// in reserved area
}


///////////////////////////////////////////////////////////////////////////////
// Breakpoint support                                                        //
///////////////////////////////////////////////////////////////////////////////

void dump_bp( EC2DRV *obj )
{
	DUMP_FUNC();
	int bp;
	printf("BP Dump:\n");
	for( bp=0; bp<4; bp++ )
	{
		printf(	"\t%i\t0x%05x\t%s\n",
				bp,obj->bpaddr[bp],
				((obj->bp_flags>>bp)&0x01) ? "Active" : "inactive" );
	}
}

/** Clear all breakpoints in the local table and also in the hardware.
*/
void ec2_clear_all_bp( EC2DRV *obj )
{
	DUMP_FUNC();
	int bp;
	for( bp=0; bp<4; bp++ )
		setBpMask( obj, bp, FALSE );
	if(obj->debug)
		dump_bp(obj);
}

/** Determine if there is a free breakpoint and then returning its index
  * \returns the next available breakpoint index, -1 on failure
 */
static int getNextBPIdx( EC2DRV *obj )
{
	DUMP_FUNC();
	int i;
	
	for( i=0; i<4; i++ )
	{
		if( !( (obj->bp_flags)>>i)&0x01 )
			return i;				// not used, well take it
	}
	return -1;						// no more available
}

/** Get the index of the breakpoint for the specified address
  * \returns index of breakpoint matching supplied address or -1 if not found
  */
static int getBP( EC2DRV *obj, uint32_t addr )
{
	DUMP_FUNC();
	int i;

	for( i=0; i<4; i++ )
		if( ( obj->bpaddr[i]==addr) && ((obj->bp_flags>>i)&0x01) )
			return i;

	return -1;	// No active breakpoints with this address
}

// Modify the bp mask approprieatly and update EC2
/** Update both our local and the EC2 bp mask byte
  * \param bp		breakpoint number to update
  * \param active	TRUE set that bp active, FALSE to disable
  * \returns		TRUE = success, FALSE=failure
  */
static BOOL setBpMask( EC2DRV *obj, int bp, BOOL active )
{
	DUMP_FUNC();
	char cmd[7];
//	printf("static BOOL setBpMask( EC2DRV *obj, %i, %i )\n",bp,active);
//	printf("obj->bp_flags = 0x%04x\n",obj->bp_flags);
	if( active )
		obj->bp_flags |= ( 1 << bp );
	else
		obj->bp_flags &= ~( 1 << bp );

	if( obj->mode==JTAG )
		return jtag_update_bp_enable_mask(obj);
	else if( obj->mode==C2 )
		return c2_update_bp_enable_mask(obj);
	else
		return FALSE;
}

/** check the breakpoint flags to see if the specific breakpoint is set.
*/
BOOL isBPSet( EC2DRV *obj, int bpid )
{
	DUMP_FUNC();
	return (obj->bp_flags >> bpid) & 0x01;
}


/** Add a new breakpoint using the first available breakpoint
  */
BOOL ec2_addBreakpoint( EC2DRV *obj, uint32_t addr )
{
	DUMP_FUNC();
	char cmd[7];
	int bp;
//	printf("BOOL ec2_addBreakpoint( EC2DRV *obj, uint16_t addr )\n");
	if( getBP( obj, addr )==-1 )	// check address doesn't already have a BP
	{
		bp = getNextBPIdx( obj );
		if( bp!=-1 )
		{
			if( obj->mode==JTAG )
			{
				if( jtag_addBreakpoint( obj, bp, addr ) )
					return setBpMask( obj, bp, TRUE );
				else
					return FALSE;
			}
			else if( obj->mode==C2 )
			{
				if( c2_addBreakpoint( obj, bp, addr ) )
					return setBpMask( obj, bp, TRUE );
				else
					return FALSE;
			}
		}
	}
	return FALSE;
}

BOOL ec2_removeBreakpoint( EC2DRV *obj, uint32_t addr )
{
	DUMP_FUNC();
	int16_t bp = getBP( obj, addr );
	if( bp != -1 )
		return setBpMask( obj, bp, FALSE );
	else
		return FALSE;
}


/**  Write the data pointed to by image into the flash memory of the EC2
  * \param image	buffer containing the firmware image.
  * \param len		Length of the image in bytes (shoulden't ever change)
  */
BOOL ec2_write_firmware( EC2DRV *obj, char *image, uint16_t len )
{
	DUMP_FUNC();
	int i;
	char cmd[4];
	BOOL r = FALSE;
	// defines order of captured blocks...
	// 0x12 version
//	const char ec2_block_order[] = 
//	{ 
//		0x0E,0x09,0x0D,0x05,0x06,0x0A,0x08,
//		0x0C,0x0B,0x07,0x04,0x0F,0x02,0x03
//	};
	// 0x13 version. I think we should move to unscrambled firmware
	const char ec2_block_order[] = 
	{ 
		0x0f,0x0a,0x0d,0x0e,0x05,0x06,0x09,
		0x07,0x0b,0x0c,0x04,0x08,0x02,0x03
	};
	const char ec3_block_order[] = 
	{ 
		0x11,0x12,0x1b,0x1d,
		0x1c,0x18,0x19,0x1a,
		0x0b,0x16,0x17,0x15,
		0x13,0x14,0x10,0x0c,
		0x0d,0x0e,0x0f,0x0c		// note 0x0c seems to be an end marker, why c again, there is no block for it!, I think its the start secctor for execution.
	};
	
	if( obj->dbg_adaptor==EC2 )
	{
		update_progress( obj, 0 );
		ec2_reset( obj );
		trx( obj, "\x55", 1, "\x5A", 1 );
		for(i=0; i<14;i++)
		{
			boot_select_flash_page(obj,ec2_block_order[i]);
			boot_erase_flash_page(obj);
			boot_write_flash_page(obj,(uint8_t*)image+(i*0x200),FALSE);
			boot_calc_page_cksum(obj);
			update_progress( obj, (i+1)*100/14 );
		}
		boot_select_flash_page(obj,0x0c);
		ec2_reset( obj );
		r = trx( obj, "\x55", 1, "\x5a", 1 );
		ec2_reset( obj );
	}
	else if( obj->dbg_adaptor==EC3 )
	{
		update_progress( obj, 0 );
		trx( obj, "\x05\x17\xff",3,"\xff",1);
		int i;
		for( i=0; i<19; i++)
		{
			boot_select_flash_page(obj,ec3_block_order[i]);
			boot_erase_flash_page(obj);
			boot_write_flash_page(obj,(uint8_t*)image+(i*0x200),FALSE);
			boot_calc_page_cksum(obj);
			update_progress( obj, (i+1)*100/19 );
		}
		boot_select_flash_page(obj,0x0c);
		ec2_disconnect(obj);
		return TRUE;
	}
	return r;
}


///////////////////////////////////////////////////////////////////////////////
/// Internal helper functions                                               ///
///////////////////////////////////////////////////////////////////////////////

/** Update progress counter and call callback if set
  */
inline static void update_progress( EC2DRV *obj, uint8_t percent )
{
	obj->progress = percent;
	if( obj->progress_cbk )
		obj->progress_cbk( obj->progress );

}

/** Send a block of characters to the port and check for the correct reply
  */
BOOL trx( EC2DRV *obj, char *txbuf, int txlen, char *rxexpect, int rxlen )
{
	char rxbuf[256];
	write_port( obj, txbuf, txlen );
	if( read_port( obj, rxbuf, rxlen ) )
		return memcmp( rxbuf, rxexpect, rxlen )==0 ? TRUE : FALSE;
	else
		return FALSE;
}

/** Reset the EC2 by turning off DTR for a short period
  */
void ec2_reset( EC2DRV *obj )
{
	DUMP_FUNC();
	if( obj->dbg_adaptor==EC2 )
	{
		usleep(100);
		DTR( obj, FALSE );
		usleep(100);
		DTR( obj, TRUE );
		usleep(10000);	// 10ms minimum appears to be about 8ms so play it safe
	}
	else if( obj->dbg_adaptor==EC3 )
	{
		// fixme the following is unsave for some caller to ec2_reset
//		ec2_disconnect( obj );
//		ec2_connect( obj, obj->port );
		printf("ec2_reset C2\n");
	}
	DUMP_FUNC_END();
}


BOOL txblock( EC2DRV *obj, EC2BLOCK *blk )
{
	int i = 0;
	while( blk[i].tlen != -1 )
	{
		trx( obj, blk[i].tx, blk[i].tlen, blk[i].rx, blk[i].rlen );
		i++;
	}
}


///////////////////////////////////////////////////////////////////////////////
/// COM port control functions                                              ///
///////////////////////////////////////////////////////////////////////////////
static BOOL open_port( EC2DRV *obj, const char *port )
{
	if( obj->dbg_adaptor==EC3 )
	{
		return open_ec3( obj, port );
	}
	else
	{
	obj->fd = open( port, O_RDWR | O_NOCTTY | O_NDELAY );
	if( obj->fd == -1 )
	{
		/*
		* Could not open the port.
		*/
		printf("open_port: Unable to open %s\n", port );
		return FALSE;
	}
	else
	{
		fcntl( obj->fd, F_SETFL, 0 );
		struct termios options;

		// Get the current options for the port...
		tcgetattr( obj->fd, &options );
		
		// Set the baud rates to 115200
		cfsetispeed(&options, B115200);
		cfsetospeed(&options, B115200);

		// Enable the receiver and set local mode...
		options.c_cflag |= (CLOCAL | CREAD);

		// set 8N1
		options.c_cflag &= ~PARENB;
		options.c_cflag &= ~CSTOPB;
		options.c_cflag &= ~CSIZE;
		options.c_cflag |= CS8;
		
		// Disable hardware flow control
		options.c_cflag &= ~CRTSCTS;
		
		// Disable software flow control
		options.c_iflag = 0;	// raw mode, no translations, no parity checking etc.
		
		// select RAW input
		options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

		// select raw output
		options.c_oflag &= ~OPOST;
		
		options.c_cc[VMIN] = 1;
		
		// Set the new options for the port...
		tcsetattr( obj->fd, TCSANOW, &options );
	}
	RTS( obj, TRUE );
	DTR( obj, TRUE );
	return TRUE;
	}
}

BOOL write_port_ch( EC2DRV *obj, char ch )
{
	if( obj->dbg_adaptor==EC3 )
		return write_usb_ch( obj, ch );
	else
		return write_port( obj, &ch, 1 );
}

BOOL write_port( EC2DRV *obj, char *buf, int len )
{
	if( obj->dbg_adaptor==EC3 )
	{
		return write_usb( obj, buf, len );
	}
	else
	{
		tx_flush( obj );
		rx_flush( obj );
		write( obj->fd, buf, len );
		tcdrain(obj->fd);
//		usleep(10000);				// without this we get TIMEOUT errors
		if( obj->debug )
		{
			printf("TX: ");
			print_buf( buf, len );
		}
		return TRUE;
	}
}

int read_port_ch( EC2DRV *obj )
{
	if( obj->dbg_adaptor==EC3 )
	{
		return read_usb_ch( obj );
	}
	else
	{
		char ch;
		if( read_port( obj, &ch, 1 ) )
			return ch;
		else
			return -1;
	}
}

BOOL read_port( EC2DRV *obj, char *buf, int len )
{
	if( obj->dbg_adaptor==EC3 )
	{
		return read_usb( obj, buf, len );
	}
	else
	{
		fd_set			input;
		struct timeval	timeout;
		int cnt=0, r, n;
		while(TRUE)
		{
//			r = read( obj->fd, cur_ptr, len-cnt );
//			if( obj->debug )
			// Initialize the input set
			FD_ZERO( &input );
			FD_SET( obj->fd, &input );
			//			fcntl(obj->fd, F_SETFL, 0);	// block if not enough		characters available

			// Initialize the timeout structure
			timeout.tv_sec  = 5;		// n seconds timeout
			timeout.tv_usec = 0;
			
			// Do the select
			n = select( obj->fd+1, &input, NULL, NULL,&timeout );
			if (n < 0)
			{
//				printf("RX: ");
//				print_buf( buf, len );
				perror("select failed");
				exit(-1);
				return FALSE;
			}
			else if (n == 0)
			{
				puts("TIMEOUT");
				return FALSE;
			}
			else
			{
				r = read( obj->fd, buf+cnt, len-cnt );
				if (r < 1)
				{
					printf ("Problem !!! This shouldn't happenen.\n");
					return FALSE;
				}
				cnt += r;
				if( obj->debug )
				{
//					printf("RX: ");
//					print_buf( buf, len );
				}
				if (cnt == len)
				{
					if( obj->debug)
					{
						printf("RX: ");
						print_buf( buf, len );
					}
					return TRUE;
				}
			}
		}
	}
}


static void rx_flush( EC2DRV *obj )
{
	tcflush( obj->fd, TCIFLUSH );
}

static void tx_flush( EC2DRV *obj )
{
	tcflush( obj->fd, TCOFLUSH );
}

static void close_port( EC2DRV *obj )
{
	if( obj->dbg_adaptor==EC3 )
		close_ec3( obj );
	else
		close( obj->fd );
}

static void DTR( EC2DRV *obj, BOOL on )
{
	if( obj->dbg_adaptor==EC2 )
	{
		int status;
		ioctl( obj->fd, TIOCMGET, &status );
		if( on )
			status |= TIOCM_DTR;
		else
			status &= ~TIOCM_DTR;
		ioctl( obj->fd, TIOCMSET, &status );
	}
}

static void RTS( EC2DRV *obj, BOOL on )
{
	if( obj->dbg_adaptor==EC2 )
	{
		int status;
		ioctl( obj->fd, TIOCMGET, &status );
		if( on )
			status |= TIOCM_RTS;
		else
			status &= ~TIOCM_RTS;
		ioctl( obj->fd, TIOCMSET, &status );
	}
}

static void print_buf( char *buf, int len )
{
	while( len-- !=0 )
		printf("%02x ",(unsigned char)*buf++);
	printf("\n");
}


///////////////////////////////////////////////////////////////////////////////
/// EC3, USB control functions                                              ///
///////////////////////////////////////////////////////////////////////////////
#define EC3_OUT_ENDPOINT	0x02
#define EC3_IN_ENDPOINT		0x81
#define EC3_PRODUCT_ID		0x8044
#define EC3_VENDOR_ID		0x10c4
extern int usb_debug;		///< control libusb debugging

/* write a complete command to the EC3.
  adds length byte
*/
static BOOL write_usb( EC2DRV *obj, char *buf, int len )
{
	int r;
	char *txbuf = malloc( len + 1 );
	txbuf[0] = len;
	memcpy( txbuf+1, buf, len );
	if( obj->debug )
	{
		printf("TX: ");
		print_buf(txbuf,len+1);
	}
	r = usb_interrupt_write( obj->ec3, EC3_OUT_ENDPOINT, txbuf, len + 1, 1000 );
	free( txbuf );
	return r > 0;
}


/** write a single byte to the EC3 using USB.
	This should only be used for writes that have exactly 1 byte of data and 1 length byte.
 */
static BOOL write_usb_ch( EC2DRV *obj, char ch )
{
	return write_usb( obj, &ch, 1 );
}

/* read a complete result from the EC3.
  strips off length byte
*/
static BOOL read_usb( EC2DRV *obj, char *buf, int len )
{
	int r;
	char *rxbuf = malloc( len + 1 );
	r = usb_interrupt_read( obj->ec3, EC3_IN_ENDPOINT, rxbuf, len+1, 1000 );
	if( obj->debug )
	{
		printf("RX: ");
		print_buf(rxbuf,len+1);
	}
	memcpy( buf, rxbuf+1, len );
	free( rxbuf );
	return r > 0;
}

/** read a single byte from the EC3 using USB.
	This should only be used for replies that have exactly 1 byte of data and 1 length byte.
*/
static int read_usb_ch( EC2DRV *obj )
{
	char ch;
	if( read_usb( obj, &ch, 1 ) )
		return ch;
	else
		return -1;
}


/** Initialise communications with an EC3.
	Search for an EC3 then initialise communications with it.
*/
BOOL open_ec3( EC2DRV *obj, const char *port )
{
	struct usb_bus *busses;
	struct usb_bus *bus;
	struct usb_device_descriptor *ec3descr=0;
	struct usb_device *ec3dev;
	char s[255];
	BOOL match = FALSE;
	
	//usb_debug = 4;	// enable libusb debugging
	usb_init();
	usb_find_busses();
	usb_find_devices();
	busses = usb_get_busses(); 

	ec3dev = 0;
	for (bus = busses; bus; bus = bus->next)
	{
		struct usb_device *dev;
		for (dev = bus->devices; dev; dev = dev->next)
		{
			if( (dev->descriptor.idVendor==EC3_VENDOR_ID) &&
				(dev->descriptor.idProduct==EC3_PRODUCT_ID) )
			{
				if( port==0 )
				{
					ec3descr = &dev->descriptor;
					ec3dev = dev;
					match = TRUE;
					break;
				}
				else
				{
					obj->ec3 = usb_open(dev);
					usb_get_string_simple(obj->ec3, dev->descriptor.iSerialNumber, s, sizeof(s));
					// check for matching serial number
//					printf("s='%s'\n",s);
					usb_release_interface( obj->ec3, 0 );
					usb_close(obj->ec3);
					if( strcmp( s, port )==0 )
					{
						ec3descr = &dev->descriptor;
						ec3dev = dev;
						match = TRUE;
						break;
					}
				}
			}
		}
	}
	if( match == FALSE )
	{
		printf("MATCH FAILED, no suitable devices\n");
		return FALSE;
	}
//	printf("bMaxPacketSize0 = %i\n",ec3descr->bMaxPacketSize0);
//	printf("iManufacturer = %i\n",ec3descr->iManufacturer);
//	printf("idVendor = %04x\n",(unsigned int)ec3descr->idVendor);
//	printf("idProduct = %04x\n",(unsigned int)ec3descr->idProduct);
	obj->ec3 = usb_open(ec3dev);
//	printf("open ec3 = %i\n",obj->ec3);

//	printf("getting manufacturere string\n");
	usb_get_string_simple(obj->ec3, ec3descr->iManufacturer, s, sizeof(s));
//	printf("s='%s'\n",s);

	int r;
	usb_set_configuration( obj->ec3, 1 );
	
#ifdef HAVE_USB_DETACH_KERNEL_DRIVER_NP
	// On linux we force the inkernel drivers to release the device for us.	
	// can't do too much for other platforms as this function is platform specific
	usb_detach_kernel_driver_np( obj->ec3, 0);
#endif
	r = usb_claim_interface( obj->ec3, 0 );
	
#ifdef HAVE_USB_DETACH_KERNEL_DRIVER_NP
	// On linux we force the inkernel drivers to release the device for us.	
	// can't do too much for other platforms as this function is platform specific
r = usb_detach_kernel_driver_np( obj->ec3, 0);
#endif
	return TRUE;
}


void close_ec3( EC2DRV *obj )
{
	DUMP_FUNC();
	usb_detach_kernel_driver_np( obj->ec3, 0);
	usb_release_interface( obj->ec3, 0 );
	usb_close(obj->ec3);
	DUMP_FUNC_END();
}
