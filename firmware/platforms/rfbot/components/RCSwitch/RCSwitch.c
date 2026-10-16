/*
	RCSwitch - Arduino libary for remote control outlet switches
	Copyright (c) 2011 Suat Ozgur.	All right reserved.
	
	Contributors:
	- Andre Koehler / info(at)tomate-online(dot)de
	- Gordeev Andrey Vladimirovich / gordeev(at)openpyro(dot)com
	- Skineffect / http://forum.ardumote.com/viewtopic.php?f=2&t=46
	- Dominik Fischer / dom_fischer(at)web(dot)de
	- Frank Oltmanns / <first name>.<last name>(at)gmail(dot)com
	- Andreas Steinel / A.<lastname>(at)gmail(dot)com
	- Max Horn / max(at)quendi(dot)de
	- Robert ter Vehn / <first name>.<last name>(at)gmail(dot)com
	- Johann Richard / <first name>.<last name>(at)gmail(dot)com
	- Vlad Gheorghe / <first name>.<last name>(at)gmail(dot)com https://github.com/vgheo
	- Matias Cuenca-Acuna 
	
	Project home: https://github.com/sui77/rc-switch/
	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Lesser General Public
	License as published by the Free Software Foundation; either
	version 2.1 of the License, or (at your option) any later version.
	This library is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the GNU
	Lesser General Public License for more details.
	You should have received a copy of the GNU Lesser General Public
	License along with this library; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h" // for esp-idf v5
#include "esp_attr.h"

#include "RCSwitch.h"

#define proto rf_protocols
#define numProto RF_PROTOCOL_COUNT

void initSwitch(RCSWITCH_t * RCSwitch) {
	RCSwitch->nReceivedValue = 0;
	RCSwitch->nReceivedBitlength = 0;
	RCSwitch->nReceivedDelay = 0;
	RCSwitch->nReceivedProtocol = 0;
	RCSwitch->nReceiveTolerance = 80;
	RCSwitch->nSeparationLimit = 1500;

	RCSwitch->nTransmitterPin = -1;
	setRepeatTransmit(RCSwitch, 10);
	setProtocol(RCSwitch, 1);
	RCSwitch->nReceiverInterrupt = -1;
	setReceiveTolerance(RCSwitch, 80);
	RCSwitch->nReceivedValue = 0;
}

/**
	* Sets the protocol to send, from a list of predefined protocols
	*/
void setProtocol(RCSWITCH_t * RCSwitch, int nProtocol) {
	if (nProtocol < 1 || nProtocol > numProto) {
	nProtocol = 1;	// TODO: trigger an error, e.g. "bad protocol" ???
	}
	RCSwitch->protocol = proto[nProtocol-1];
}

/**
	* Sets the protocol to send with pulse length in microseconds.
	*/
void setProtocolPulseLength(RCSWITCH_t * RCSwitch, int nProtocol, int nPulseLength) {
	setProtocol(RCSwitch, nProtocol);
	setPulseLength(RCSwitch, nPulseLength);
}

/**
	* Sets pulse length in microseconds
	*/
void setPulseLength(RCSWITCH_t * RCSwitch, int nPulseLength) {
	RCSwitch->protocol.pulseLength = nPulseLength;
}

/**
 * Sets Repeat Transmits
 */
void setRepeatTransmit(RCSWITCH_t * RCSwitch, int nRepeatTransmit) {
	RCSwitch->nRepeatTransmit = nRepeatTransmit;
}

/**
 * Set Receiving Tolerance
 */
void setReceiveTolerance(RCSWITCH_t * RCSwitch, int nPercent) {
	RCSwitch->nReceiveTolerance = nPercent;
}

/**
 * Enable transmissions
 *
 * @param nTransmitterPin Arduino Pin to which the sender is connected to
 */
void enableTransmit(RCSWITCH_t * RCSwitch, int nTransmitterPin) {
	RCSwitch->nTransmitterPin = nTransmitterPin;
	//pinMode(this->nTransmitterPin, OUTPUT);
	//gpio_pad_select_gpio( RCSwitch->nTransmitterPin );
	gpio_reset_pin( RCSwitch->nTransmitterPin );
	gpio_set_direction( RCSwitch->nTransmitterPin, GPIO_MODE_OUTPUT );
}

/**
	* Disable transmissions
	*/
void disableTransmit(RCSWITCH_t * RCSwitch) {
	RCSwitch->nTransmitterPin = -1;
}

/**
 * Returns a char[13], representing the code word to be send.
 *
 */
char* getCodeWordA(const char* sGroup, const char* sDevice, bool bStatus) {
	static char sReturn[13];
	int nReturnPos = 0;

	for (int i = 0; i < 5; i++) {
		sReturn[nReturnPos++] = (sGroup[i] == '0') ? 'F' : '0';
	}

	for (int i = 0; i < 5; i++) {
		sReturn[nReturnPos++] = (sDevice[i] == '0') ? 'F' : '0';
	}

	sReturn[nReturnPos++] = bStatus ? '0' : 'F';
	sReturn[nReturnPos++] = bStatus ? 'F' : '0';

	sReturn[nReturnPos] = '\0';
	return sReturn;
}

/**
 * Encoding for type B switches with two rotary/sliding switches.
 *
 * The code word is a tristate word and with following bit pattern:
 *
 * +-----------------------------+-----------------------------+----------+------------+
 * | 4 bits address							 | 4 bits address							 | 3 bits		| 1 bit			 |
 * | switch group								 | switch number							 | not used | on / off	 |
 * | 1=0FFF 2=F0FF 3=FF0F 4=FFF0 | 1=0FFF 2=F0FF 3=FF0F 4=FFF0 | FFF			| on=F off=0 |
 * +-----------------------------+-----------------------------+----------+------------+
 *
 * @param nAddressCode	Number of the switch group (1..4)
 * @param nChannelCode	Number of the switch itself (1..4)
 * @param bStatus				Whether to switch on (true) or off (false)
 *
 * @return char[13], representing a tristate code word of length 12
 */
char* getCodeWordB(int nAddressCode, int nChannelCode, bool bStatus) {
	static char sReturn[13];
	int nReturnPos = 0;

	if (nAddressCode < 1 || nAddressCode > 4 || nChannelCode < 1 || nChannelCode > 4) {
		return 0;
	}

	for (int i = 1; i <= 4; i++) {
		sReturn[nReturnPos++] = (nAddressCode == i) ? '0' : 'F';
	}

	for (int i = 1; i <= 4; i++) {
		sReturn[nReturnPos++] = (nChannelCode == i) ? '0' : 'F';
	}

	sReturn[nReturnPos++] = 'F';
	sReturn[nReturnPos++] = 'F';
	sReturn[nReturnPos++] = 'F';

	sReturn[nReturnPos++] = bStatus ? 'F' : '0';

	sReturn[nReturnPos] = '\0';
	return sReturn;
}

/**
 * Like getCodeWord (Type C = Intertechno)
 */
char* getCodeWordC(char sFamily, int nGroup, int nDevice, bool bStatus) {
	static char sReturn[13];
	int nReturnPos = 0;

	int nFamily = (int)sFamily - 'a';
	if ( nFamily < 0 || nFamily > 15 || nGroup < 1 || nGroup > 4 || nDevice < 1 || nDevice > 4) {
		return 0;
	}
	
	// encode the family into four bits
	sReturn[nReturnPos++] = (nFamily & 1) ? 'F' : '0';
	sReturn[nReturnPos++] = (nFamily & 2) ? 'F' : '0';
	sReturn[nReturnPos++] = (nFamily & 4) ? 'F' : '0';
	sReturn[nReturnPos++] = (nFamily & 8) ? 'F' : '0';

	// encode the device and group
	sReturn[nReturnPos++] = ((nDevice-1) & 1) ? 'F' : '0';
	sReturn[nReturnPos++] = ((nDevice-1) & 2) ? 'F' : '0';
	sReturn[nReturnPos++] = ((nGroup-1) & 1) ? 'F' : '0';
	sReturn[nReturnPos++] = ((nGroup-1) & 2) ? 'F' : '0';

	// encode the status code
	sReturn[nReturnPos++] = '0';
	sReturn[nReturnPos++] = 'F';
	sReturn[nReturnPos++] = 'F';
	sReturn[nReturnPos++] = bStatus ? 'F' : '0';

	sReturn[nReturnPos] = '\0';
	return sReturn;
}

/**
 * Encoding for the REV Switch Type
 *
 * The code word is a tristate word and with following bit pattern:
 *
 * +-----------------------------+-------------------+----------+--------------+
 * | 4 bits address							 | 3 bits address		 | 3 bits		| 2 bits			 |
 * | switch group								 | device number		 | not used | on / off		 |
 * | A=1FFF B=F1FF C=FF1F D=FFF1 | 1=0FF 2=F0F 3=FF0 | 000			| on=10 off=01 |
 * +-----------------------------+-------------------+----------+--------------+
 *
 * Source: http://www.the-intruder.net/funksteckdosen-von-rev-uber-arduino-ansteuern/
 *
 * @param sGroup				Name of the switch group (A..D, resp. a..d) 
 * @param nDevice				Number of the switch itself (1..3)
 * @param bStatus				Whether to switch on (true) or off (false)
 *
 * @return char[13], representing a tristate code word of length 12
 */
char* getCodeWordD(char sGroup, int nDevice, bool bStatus) {
	static char sReturn[13];
	int nReturnPos = 0;

	// sGroup must be one of the letters in "abcdABCD"
	int nGroup = (sGroup >= 'a') ? (int)sGroup - 'a' : (int)sGroup - 'A';
	if ( nGroup < 0 || nGroup > 3 || nDevice < 1 || nDevice > 3) {
		return 0;
	}

	for (int i = 0; i < 4; i++) {
		sReturn[nReturnPos++] = (nGroup == i) ? '1' : 'F';
	}

	for (int i = 1; i <= 3; i++) {
		sReturn[nReturnPos++] = (nDevice == i) ? '1' : 'F';
	}

	sReturn[nReturnPos++] = '0';
	sReturn[nReturnPos++] = '0';
	sReturn[nReturnPos++] = '0';

	sReturn[nReturnPos++] = bStatus ? '1' : '0';
	sReturn[nReturnPos++] = bStatus ? '0' : '1';

	sReturn[nReturnPos] = '\0';
	return sReturn;
}

/**
 * @param sCodeWord		a tristate code word consisting of the letter 0, 1, F
 */
void sendTriState(RCSWITCH_t * RCSwitch, const char* sCodeWord) {
	// turn the tristate code word into the corresponding bit pattern, then send it
	unsigned long code = 0;
	unsigned int length = 0;
	for (const char* p = sCodeWord; *p; p++) {
		code <<= 2L;
		switch (*p) {
			case '0':
				// bit pattern 00
				break;
			case 'F':
				// bit pattern 01
				code |= 1L;
				break;
			case '1':
				// bit pattern 11
				code |= 3L;
				break;
		}
		length += 2;
	}
	sendCode(RCSwitch, code, length);
}

bool available(RCSWITCH_t *rc) { return rc->nReceivedBitlength != 0; }
void resetAvailable(RCSWITCH_t *rc) { rc->nReceivedBitlength = 0; }
unsigned long getReceivedValue(RCSWITCH_t *rc) { return rc->nReceivedValue; }
unsigned int getReceivedBitlength(RCSWITCH_t *rc) { return rc->nReceivedBitlength; }
unsigned int getReceivedDelay(RCSWITCH_t *rc) { return rc->nReceivedDelay; }
unsigned int getReceivedProtocol(RCSWITCH_t *rc) { return rc->nReceivedProtocol; }
