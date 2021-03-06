/*************************************************************
project: <type project name here>
author: <type your name here>
description: <type what this file does>
*************************************************************/

#include "ArduinoXPlaneAutopilot_main.h"

/*
TXBuffer : DATA + \0 + (long)idx + (float)*8 + (longidx + (float)*8 + ... 

RXBuffer :
[0..4] : DATA@

[5..8] : 3L (idx_speed)
[9..12] : Vind KIAS
[13..16] : Vind KEAS
[17..20] : Vtrue KTAS
[21..24] : Vtrue KTGS
[25..28] : ---
[29..32] : Vind MPH
[33..36] : Vtrue MPHAS
[37..40] : Vtrue MPGHS

[41..44] : 5L (idx_mach)
[45..48] : Mach
[49..52] : ---
[53..56] : VVI
[57..60] : ---
[61..64] : G norm
[65..68] : G axial
[69..72] : G side
[73..76] : ---

[77..80] : 17L (pitch/roll/heading)
[81..84] : pitch
[85..88] : roll
[89..92] : hding true
[93..96] : hding mag
[97..100] : ---
[101..104] : ---
[105..108] : --- 
[109..112] : ---

[113..116] : 18L (AOA)
[117..120] : alpha
[121..124] : beta
[125..128] : hpath
[129..132] : vpath
[133..136] : ---
[137..140] : ---
[141..144] : ---
[145..148] : slip

[149..152] : 20L (lat/lon/alt)
[153..156] : lat deg
[157..160] : lon deg
[161..164] : alt ftmsl
[165..168] : alt ftagl
[169..172] : on runwy
[173..176] : alt ind
[177..180] : lat south
[181..184] : lon west

[185..188] : 25L (thr)
[189..192] : thr 1
[193..196] : thr 2
*/


/*********
 * SETUP *
 ********/
void setup() {

	// Setup Serial
	Serial.begin(19200);
	while (!Serial) { ; }

	// Setup Ethernet
	if (Ethernet.begin(mac) == 0) {
		Serial.println("Failed to configure Ethernet using DHCP");
		for(;;) ;
	}
	
	// print local IP address:
	Serial.print("My IP address: ");
	for (byte thisByte = 0; thisByte < 4; thisByte++) {
		Serial.print(Ethernet.localIP()[thisByte], DEC);
		Serial.print(".");
	}
	Serial.println();
	
	// Listen to local port
	udp.begin(localPort);

	// Call setup functions (see below)
	setupBuffer();
	setupPID();

	// Setup Target
	//AoAPID_Target = 2.5f;
	speedPID_Target = 120.0f;
	rollPID_Target  = 0.0f;
	VVIPID_Target   = 0.0f;
	altPID_Target   = 5000.0f;
	hdingPID_Target = 0.0f;
	hding_vt = 320.0f;
	slipPID_Target  = 0.0f;
	elev_Target	= 0.0f;

	Serial.print("free ram (approx.) :"); Serial.println(freeRam());
}


/*****************
 * INFINITE LOOP *
 ****************/
void loop() {

	// Debug stuff
	bool sendPacket = true;
	bool printDebug = true;

	// Request packet
	int packetSize = udp.parsePacket();

	// If packet received
	if(packetSize != 0) {
	
		// Read packet
		udp.read(RXBuffer, UDP_TX_PACKET_MAX_SIZE);

		// Read & compute speed
		speedPID_In = *speed;
		speedPID.Compute();

		// Read & compute ALT
		altPID_In = *alt;
		if(CLIMB_MODE == CLIMB_ALT) {	// if in ALTitude control mode
			altPID.Compute();			// Compute altPID out
			VVIPID_Target = altPID_Out;	// altPID out -> VVI Target
		}
		
		// Now we compute VVI
		// The VVI Target at this point is our own VVI target (in CLIMB_VVI mode)
		// or the output of the altPID (in CLIM_ALT mode), it doesn't make any difference
		// We compute VVIPID the same way.
		VVIPID_In = *VVI;
		VVIPID.Compute();
		elev_Target = VVIPID_Out;
		
		// New stuff here, the output is VVI is now linked to a linearServo
		// for smoother operation :-)
		elevServo.SetTarget(elev_Target);
		elev = elevServo.Compute();

		/* Read & compute heading
		* This formula is needed because the heading is [0,360[
		* and 350� + 20� = 10�, which is going to confuse the PID ^^
		*
		* So we do something like this : 
		* - Our heading target is always 0.0f
		* - our hdgPID input will be the difference between the target (0) and the actual heading
		* - So if we are at 350� with a target of 10�, the PID input will be -20.0 (thx to our formula)
		* - The PID know how to deal with this :  if diff is negative then turn right, otherwise turn left
		*
		* PS : the output of this PID will be used as input for roll target
		*/
		hding_v = fmod(hding_vt - *hding + 180.0f, 360.0f)  - 180.0f;
		if(hding_v < -180) hding_v += 360; // Eww eww eww
		hdingPID_In = hding_v;
		hdingPID.Compute();
		rollPID_Target = hdingPID_Out;
			
		
		// Read & compute roll
		// You know the drill know :
		// The roll target is set by the hdgPID (there isn't any optional mode like for CLIMB_MODE)
		// We use a LinearServo here too.
		rollPID_In = *roll;
		rollPID.Compute();
		ailrn_Target = rollPID_Out;
		ailrnServo.SetTarget(ailrn_Target);
		ailrn = ailrnServo.Compute();
		
		// Read & compute slip (rudder command)
		slipPID_In = *slip;
		slipPID.Compute();

		// Read & compute AoA according to VVI target
		//AoAPID_Target = VVIPID_Out;
		//AoAPID_In = *AoA;
		//AoAPID.Compute();
		
		
		// Copy thr 1 & 2 to TX Buffer
		memcpy(&TXBuffer[9], &speedPID_Out, sizeof(speedPID_Out));
		memcpy(&TXBuffer[13], &speedPID_Out, sizeof(speedPID_Out));
		
		// Copy pitch to TX Buffer (input is the output of the linear servo)
		memcpy(&TXBuffer[45], &elev, sizeof(float));

		// Copy Roll to TX Buffer (input is the output of the linear servo)
		memcpy(&TXBuffer[49], &ailrn, sizeof(rollPID_Out));

		// Copy rudder to TX Buffer
		memcpy(&TXBuffer[53], &slipPID_Out, sizeof(slipPID_Out));


		// Send packet \o/
		if(sendPacket) {	// This if is just the debug stuff, should always be true actually.
			udp.beginPacket(udp.remoteIP(), 49000);
			udp.write((uint8_t*)TXBuffer, 77);
			udp.endPacket();
		}
		
		// Print stuff on the serial port for debug
		debugloop++;
		if(printDebug && (debugloop == 10)) {
			Serial.print("spd : "); Serial.print(*speed);
			Serial.print(" / VVI : "); Serial.print(*VVI);
			Serial.print(" / thr : "); Serial.print(*throttle);
			Serial.print(" / AoA : "); Serial.print(*AoA);
			Serial.print(" / roll : "); Serial.print(*roll);
			Serial.print(" / slip : "); Serial.print(*slip);
			Serial.print(" / hdg : "); Serial.print(*hding);
			Serial.print(" / alt : "); Serial.print(*alt);
			//Serial.print(" / debug : "); Serial.print(freeRam());
			Serial.println();
			debugloop = 0;
			
		}
		
	} // End of "packet received?"

	
	// Read serial port for commands
	// This thing need a nice refactoring, because : Ewww... ugly
	int SCount = Serial.available();
	if(SCount > 0) { //We got data
		char in = Serial.read();
		switch(in) {
			case 'S':	// Set Speed
				Serial.read();
				speedPID_Target = Serial.parseFloat();
				Serial.print("Setting speed target to : "); Serial.println(speedPID_Target);
				break;
			case 'V':	// Set VVI
				Serial.read();
				VVIPID_Target = Serial.parseFloat();
				altPID.SetMode(MANUAL);
				CLIMB_MODE = CLIMB_VVI;
				Serial.print("Setting VVI target to : "); Serial.println(VVIPID_Target);
				break;
			//case 'R':	// Set Roll
			//	Serial.read();
			//	rollPID_Target = Serial.parseFloat();
			//	Serial.print("Setting roll target to : "); Serial.println(rollPID_Target);
			//	break;
			case 'A':	// Set Altitude
				Serial.read();
				altPID_Target = Serial.parseFloat();
				altPID.SetMode(AUTOMATIC);
				altPID.Compute();
				VVIPID_Target = altPID_Out;
				CLIMB_MODE = CLIMB_ALT;
				Serial.print("Setting alt target to : "); Serial.println(altPID_Target);
				break;
			case 'H':	// Set Heading
				Serial.read(); // Skip next char
				hding_vt = Serial.parseFloat();
				Serial.print("Setting heading target to : "); Serial.println(hding_vt);
				break;
			case 'G':	// Toggle Gear (sending the "g" command jey, for testing purpose)
				CharBuffer[5] = 'g';
				udp.beginPacket(udp.remoteIP(), 49000);
				udp.write((uint8_t*)CharBuffer, 6);
				udp.endPacket();
				break;
			case 'B':	// Toggle Break
				CharBuffer[5] = 'b';
				udp.beginPacket(udp.remoteIP(), 49000);
				udp.write((uint8_t*)CharBuffer, 6);
				udp.endPacket();
				break;
		} // end of switch/case
	} // end of if(serial data)
} // end of loop


void setupPID() {

	// SPEED PID
	speedPID.SetMode(AUTOMATIC);
	speedPID.SetOutputLimits(0.0,1.0);
	speedPID.SetSampleTime(10);

	// ALT PID
	altPID.SetMode(MANUAL);
	altPID.SetOutputLimits(-1500.0, 1000.0);
	altPID.SetSampleTime(10);

	// VVI PID
	VVIPID.SetMode(AUTOMATIC);
	VVIPID.SetOutputLimits(-1.0, 1.0);
	VVIPID.SetSampleTime(10);

	// AoA PID
	//AoAPID.SetMode(AUTOMATIC);
	//AoAPID.SetOutputLimits(-1.0, 1.0);
	//AoAPID.SetSampleTime(10);
	
	// Roll PID
	rollPID.SetMode(AUTOMATIC);
	rollPID.SetOutputLimits(-1.0, 1.0);
	rollPID.SetSampleTime(10);

	// Heading PID
	hdingPID.SetMode(AUTOMATIC);
	hdingPID.SetOutputLimits(-30.0, 30.0);
	hdingPID.SetSampleTime(10);
	hdingPID.SetControllerDirection(REVERSE);

	// Slip PID
	slipPID.SetMode(AUTOMATIC);
	slipPID.SetOutputLimits(-1.0, 1.0);
	slipPID.SetSampleTime(10);

}

void setupBuffer() {
	// Setup TX Buffer
	memset(&TXBuffer, 0, UDP_TX_PACKET_MAX_SIZE);
	TXBuffer[0] = 'D';
	TXBuffer[1] = 'A';
	TXBuffer[2] = 'T';
	TXBuffer[3] = 'A';
	TXBuffer[4] = (char)0;

	memcpy(&TXBuffer[5], &idx_thr, sizeof(idx_thr));	// 25L
	memcpy(&TXBuffer[9], &fzero, sizeof(fzero));		// thro1
	memcpy(&TXBuffer[13], &fzero, sizeof(fzero));		// thro2
	memcpy(&TXBuffer[17], &fzero, sizeof(fzero));		// thro3
	memcpy(&TXBuffer[21], &fzero, sizeof(fzero));		// thro4
	memcpy(&TXBuffer[25], &fzero, sizeof(fzero));		// thro5
	memcpy(&TXBuffer[29], &fzero, sizeof(fzero));		// thro6
	memcpy(&TXBuffer[33], &fzero, sizeof(fzero));		// thro7
	memcpy(&TXBuffer[37], &fzero, sizeof(fzero));		// thro8
	
	memcpy(&TXBuffer[41], &idx_joy, sizeof(idx_joy));	// 8L
	memcpy(&TXBuffer[45], &fzero, sizeof(fzero));		// elev
	memcpy(&TXBuffer[49], &fzero, sizeof(fzero));		// ailrn
	memcpy(&TXBuffer[53], &fzero, sizeof(fzero));		// ruddr
	memcpy(&TXBuffer[57], &fzero, sizeof(fzero));		// --
	memcpy(&TXBuffer[61], &fzero, sizeof(fzero));		// nwhel
	memcpy(&TXBuffer[65], &fzero, sizeof(fzero));		// --
	memcpy(&TXBuffer[69], &fzero, sizeof(fzero));		// --
	memcpy(&TXBuffer[73], &fzero, sizeof(fzero));		// --
	
	
	// Setup char buffer (to send keypress)
	CharBuffer[0] = 'C';
	CharBuffer[1] = 'H';
	CharBuffer[2] = 'A';
	CharBuffer[3] = 'R';
	CharBuffer[4] = (char)0;
	
	// Setup RX DATA
	speed = (float*)&RXBuffer[9];
	VVI   = (float*)&RXBuffer[53];
	pitch = (float*)&RXBuffer[81];
	roll  = (float*)&RXBuffer[85];
	hding = (float*)&RXBuffer[93];
	AoA   = (float*)&RXBuffer[117];
	slip  = (float*)&RXBuffer[145];
	alt   = (float*)&RXBuffer[173];
	throttle = (float*)&RXBuffer[189];
}

int freeRam () 
{
  extern int __heap_start, *__brkval; 
  int v; 
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval); 
}