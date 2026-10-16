#ifndef MAIN_RCSWITCH_H_
#define MAIN_RCSWITCH_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "rf_codec.h"

#define	LOW  0
#define HIGH 1

// Number of maximum high/Low changes per packet.
// We can handle up to (unsigned long) => 32 bit * 2 H/L changes per bit + 2 for sync
#define RCSWITCH_MAX_CHANGES 120

typedef struct {
	volatile unsigned long nReceivedValue;
	volatile unsigned int nReceivedBitlength;
	volatile unsigned int nReceivedDelay;
	volatile unsigned int nReceivedProtocol;
	int nReceiveTolerance;
	unsigned nSeparationLimit;
	/*
	 * timings[0] contains sync timing, followed by a number of bits
	 */
	volatile unsigned int timings[RCSWITCH_MAX_CHANGES];
	int nReceiverInterrupt;

	int nTransmitterPin;
	int nRepeatTransmit;

	Protocol protocol;
} RCSWITCH_t;


	void initSwitch(RCSWITCH_t * RCSwitch);
#if 0
	void switchOn(int nGroupNumber, int nSwitchNumber);
	void switchOff(int nGroupNumber, int nSwitchNumber);
	void switchOn(const char* sGroup, int nSwitchNumber);
	void switchOff(const char* sGroup, int nSwitchNumber);
	void switchOn(char sFamily, int nGroup, int nDevice);
	void switchOff(char sFamily, int nGroup, int nDevice);
	void switchOn(const char* sGroup, const char* sDevice);
	void switchOff(const char* sGroup, const char* sDevice);
	void switchOn(char sGroup, int nDevice);
	void switchOff(char sGroup, int nDevice);

	void sendTriState(const char* sCodeWord);
	void send(unsigned long code, unsigned int length);
	void send(const char* sCodeWord);
#endif

	esp_err_t enableReceive(RCSWITCH_t * RCSwitch, int interrupt);
	bool available(RCSWITCH_t * RCSwitch);
	void resetAvailable(RCSWITCH_t * RCSwitch);

	unsigned long getReceivedValue(RCSWITCH_t * RCSwitch);
	unsigned int getReceivedBitlength(RCSWITCH_t * RCSwitch);
	unsigned int getReceivedDelay(RCSWITCH_t * RCSwitch);
	unsigned int getReceivedProtocol(RCSWITCH_t * RCSwitch);

	bool receiveCode(RCSWITCH_t *rc, TickType_t timeout);
	void rf_rmt_tx_init(int pin);

	void setProtocol(RCSWITCH_t * RCSwitch, int nProtocol);
	void setProtocolPulseLength(RCSWITCH_t * RCSwitch, int nProtocol, int nPulseLength);
	void setPulseLength(RCSWITCH_t * RCSwitch, int nPulseLength);
	void setRepeatTransmit(RCSWITCH_t * RCSwitch, int nRepeatTransmit);
	void setReceiveTolerance(RCSWITCH_t * RCSwitch, int nPercent);

	void enableTransmit(RCSWITCH_t * RCSwitch, int nTransmitterPin);
	void disableTransmit(RCSWITCH_t * RCSwitch);

	char* getCodeWordA(const char* sGroup, const char* sDevice, bool bStatus);
	char* getCodeWordB(int nAddressCode, int nChannelCode, bool bStatus);
	char* getCodeWordC(char sFamily, int nGroup, int nDevice, bool bStatus);
	char* getCodeWordD(char group, int nDevice, bool bStatus);
	void sendTriState(RCSWITCH_t * RCSwitch, const char* sCodeWord);
	esp_err_t sendCode(RCSWITCH_t * RCSwitch, unsigned long code, unsigned int length);


#endif /* MAIN_RXB6_RECEIVER_H_ */
