
/*
 * pod_network.cpp  
 * 2017 - Nick Turner and Morgan Redfield
 * Licensed under the AGPLv3. For full license see LICENSE.md 
 * Copyright (c) 2017 LMN Architects, LLC
 * 
 * Handle networking via Ethernet and XBee.
 */

#include "pod_network.h"
#include "pod_config.h"
#include "pod_logging.h"
#include "pod_util.h"

#include <SPI.h>
#include <TimerOne.h>
#include "Ethernet.h"
#include "EthernetUdp.h"

#define xbee Serial1

// XBee packet buffering (old)
volatile char BuffXBee[BUFFXBEE_SIZE];
volatile unsigned int BuffHead = 0;
volatile unsigned int BuffTail = 0;
volatile unsigned int BuffOverruns = 0;
volatile bool BuffOverrun = false;

// XBee packet buffering
volatile char xbeeBuffer[XBEE_BUFFER_SIZE];
volatile size_t xbeeBufferHead = 0;
volatile size_t xbeeBufferElements = 0;
volatile size_t xbeeBufferOverrun = 0;
volatile bool xbeeBufferHold = false;

// How frequently data is pulled from hardware serial buffer (microseconds)
// through the use of a timer-driven interrupt service routine (ISR).
// Arduino buffer is size 64 (for Teensy++ 2.0 as of Arduino 1.8.5);
// at 9600 baud, should pull data off buffer at least every
// 64*10/9600 seconds = 67ms (8-N-1: 10 serial bits sent per byte of data).
//#define XBEE_READ_INTERVAL 67000
// ...However, trying to read a (nearly) full buffer can take a while,
// which can delay other ISRs from performing their duties (like low-level
// serial interface I/O ISRs).  More frequent reads mean shorter time
// spent in each ISR call.
#define XBEE_READ_INTERVAL 10000
// Other timing considerations: Serial1 hardware register on Teensy++ 2.0
// contains a single byte of received data? [UNCLEAR]  Serial1's own ISR
// must be able to run as rapidly as that register fills (~1ms); our XBee
// read ISR run time must be shorter than that so it does not interfere
// with the Serial1 ISR timing.

String set1;
String set2;
#define NETID_LEN 4
byte network[] = {0xA,0xB,0xC,0xD};

// Ethernet connection settings
#define MAC_LEN 6
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED}; // MAC address can be anything, as long as it is unique on network
//char timeServer[] =  "time.nist.gov"; // government NTP server
#define timeServer "time.nist.gov"
#define localPort 8888
#define NTP_PACKET_SIZE 48
byte packetBuffer[NTP_PACKET_SIZE];
EthernetUDP Udp;
#define serverPort 80
char pageName[] = "/LMNSensePod.php"; // Name of submission page. Log into EC2 Server and navigate to /var/www/html to view
int totalCount = 0;
char params[200];// insure params is big enough to hold your variables
#define delayMillis 10000UL // Delay between establishing connection and making POST request
unsigned long thisMillis = 0;
unsigned long lastMillis = 0;
unsigned long lastEthernetConnect = 0;  // maximum / 2
unsigned long lastEthernetBegin = 0;
#define ETHERNET_RECONNECT_TIME (5*60000UL)
#define WIZ812MJ_ES_PIN 20 // WIZnet SPI chip-select pin
#define WIZ812MJ_RESET_PIN 9 // WIZnet reset pin
bool online = false;

//--------------------------------------------------------------------------------------------- [XBee Management]

void xbeeSetup() {
  xbee.begin(9600);

  BuffHead = 0;
  BuffTail = 0;

  xbeeBufferHead = 0;
  xbeeBufferElements = 0;
  xbeeBufferOverrun = 0;
  xbeeBufferHold = false;
  // Use timer-based, interrupt-driven function calls to
  // ensure data is getting pulled from the Arduino buffer
  // before it can fill.
  Timer1.initialize(XBEE_READ_INTERVAL);
  Timer1.attachInterrupt(readXBeeISR);
}

/* Initialize the XBee serial interface and buffering mechanism. */
void setupXBee() {
  // Start serial interface between XBee and microcontroller.
  // Note there are multiple levels of buffers: the XBee
  // contains send/receive buffers and the Arduino core libraries
  // implement 64-byte microcontroller buffers for sending/
  // receiving data to/from the XBee.  On top of that, we have
  // our own buffer for received data to overcome limitations in
  // the Arduino receive buffer....
  xbee.begin(9600);
  
  // Initialize ring buffer for data that came across the XBee
  // network.
  xbeeBufferHead = 0;
  xbeeBufferElements = 0;
  xbeeBufferOverrun = 0;
  xbeeBufferHold = false;
  
  // Use timer-based, interrupt-driven function calls to
  // ensure data is getting pulled from the Arduino buffer
  // before it can fill.
  Timer1.initialize(XBEE_READ_INTERVAL);
  Timer1.attachInterrupt(readXBeeISR);
}

/* Reads data from the XBee serial interface into a circular buffer.
   A wrapper to the readXBee() function that checks a flag to avoid
   modifying the buffer if the buffer is being accessed elsewhere,
   making this routine a thread-safe ISR when the main thread is
   accessing the buffer (assuming the main thread sets the flag). */
void readXBeeISR() {
  // Avoid modifying the buffer if currently in use by main thread
  // routines (which should set this flag).  Allows this routine
  // to be safely interrupt-driven.
  if (xbeeBufferHold) return;
  //Serial.print(F("readXBeeISR: "));
  //Serial.println(millis());
  readXBee();
}

/* Reads data from the XBee serial interface into a circular buffer.
   Note Arduino uses interrupts to grab hardware serial data as it
   arrives, placing it into a 64 character buffer (for Teensy++ 2.0,
   as of Arduino 1.8.5).  This routine pulls data from that buffer
   into our own, larger buffer, which reduced the chance of overflow
   and allows for better overflow handling.  This routine should be
   called often to ensure the Arduino buffer does not overflow and
   data is lost. */
void readXBee() {
  // Define XBEE_DEBUG for verbose XBee debugging output.
  // This can considerably slow this ISR-called routine
  // and use of Serial output in an ISR can introduce
  // unintended behaviour.
  #if defined(XBEE_DEBUG)
  if (!xbee.available()) return;
  Serial.print(F("readXBee ["));
  Serial.print(millis());
  Serial.print("]: ");
  #endif
  // Will extract all currently available data
  while (xbee.available()) {
    // If buffer overruns, ignore incoming data.
    if (xbeeBufferElements >= XBEE_BUFFER_SIZE) {
      // Standard serial: continue through loop to clear buffer
      //xbee.read();
      //xbeeBufferOverrun++;
      // Teensy serial: can clear buffer all at once
      xbeeBufferOverrun += xbee.available();
      xbee.clear();
      #if defined(XBEE_DEBUG)
      Serial.println(F("(overflow)"));
      #endif
      return;
    } else {
      xbeeBuffer[xbeeBufferHead] = xbee.read();
      //Serial.print(xbeeBuffer[xbeeBufferHead]);
      xbeeBufferHead = (xbeeBufferHead + 1) % XBEE_BUFFER_SIZE;
      xbeeBufferElements++;
    }
  }
  #if defined(XBEE_DEBUG)
  Serial.println();
  #endif
}

void sendXBee(const String packet)
{
  // Serial output should be flushed here as activity may
  // interfere with XBee communication.
  Serial.print(F("XBee send: "));
  Serial.println(packet);
  Serial.flush();

  // Would probably work...
  //xbee.write(PACKET_START_TOKEN);
  //xbee.write(packet);
  //xbee.write(PACKET_END_TOKEN);
  
  // Send payload to serial all at once in the hopes that the
  // XBee will place it it in a single network packet.
  // By default, data in XBee input buffer is sent when packet
  // is full or 3 UART character transmission times have passed
  // without activity.
  // Note we omit null-termination character.
  size_t bufLength = packet.length()+2;
  char buf[bufLength];
  strcpy(&buf[1],packet.c_str());
  buf[0] = PACKET_START_TOKEN;
  buf[bufLength-1] = PACKET_END_TOKEN;
  xbee.write(buf);

  // Hardware serial interface is operated through ISRs.
  // Give dedicated time here for those ISRs to run as any
  // routines that delay the ISRs risk causing I/O errors and
  // garbage appearing in the data packets.  This is a
  // precaution and may not be necessary if the rest of the
  // code is well-behaved....
  // Time: 1000*10/9600 ms per character @ 9600 baud, but add
  // add extra time as there may be gaps between characters.
  //delay(1000 * 15 * bufLength / 9600);
  // Even better: flush() waits for Arduino serial output buffer
  // to finish sending to the XBee.  Then add delay for XBee to
  // upload data over network.
  xbee.flush();
  delay(100);
}

/* Prevent the XBee buffer from being modified by ISR.
   Returns the prior hold state. */
bool holdXBeeBuffer() {
  // Disable interrupts to prevent ISR from running during routine.
  // Store previous interrupt state so we can restore it afterwards.
  uint8_t oldSREG = SREG;  // Save interrupt status (among other things)
  cli();
  bool prevState = xbeeBufferHold;
  xbeeBufferHold = true;
  SREG = oldSREG;  // Restore interrupt status
  return prevState;
}

/* Allows the XBee buffer to be modified by ISR. */
void releaseXBeeBuffer() {
  // Disable interrupts to prevent ISR from running during routine.
  // Store previous interrupt state so we can restore it afterwards.
  uint8_t oldSREG = SREG;  // Save interrupt status (among other things)
  cli();
  xbeeBufferHold = false;
  SREG = oldSREG;  // Restore interrupt status
}

/* Resets the XBee buffer to its empty state. */
void resetXBeeBuffer() {
  // Disable interrupts to prevent ISR from changing the buffer.
  // Store previous interrupt state so we can restore it afterwards.
  uint8_t oldSREG = SREG;  // Save interrupt status (among other things)
  cli();  // Disable interrupts
  xbeeBufferHead = 0;
  xbeeBufferElements = 0;
  xbeeBufferOverrun = 0;
  SREG = oldSREG;  // Restore interrupt status
}

/* Removes buffer data before the first packet and or after the
   last complete packet. The latter should be performed on a
   buffer overrun as data beyond the last complete packet belongs
   to a packet where data was thrown out. */
void cleanXBeeBuffer(const bool cleanStart, const bool cleanEnd) {
  // Prevent ISR from modifying buffer during this routine.
  // Will return to previous hold state.
  bool wasHeld = holdXBeeBuffer();
  
  // Empty buffer
  if (xbeeBufferElements == 0) {
    //xbeeBufferHead = 0;
    if (!wasHeld) releaseXBeeBuffer();
    return;
  }
  // Remove everything prior to first start token
  if (cleanStart) {
    while ((xbeeBufferElements > 0) && (xbeeBuffer[(xbeeBufferHead - xbeeBufferElements) % XBEE_BUFFER_SIZE] != PACKET_START_TOKEN)) {
      xbeeBufferElements--;
    }
  }
  // Remove everything aftter last end token
  if (cleanEnd) {
    while ((xbeeBufferElements > 0) && (xbeeBuffer[xbeeBufferHead] != PACKET_END_TOKEN)) {
      xbeeBufferElements--;
      xbeeBufferHead = (xbeeBufferHead - 1) % XBEE_BUFFER_SIZE;
    }
  }
  if (!wasHeld) releaseXBeeBuffer();
}

/* Returns the next available XBee data packet from the XBee buffer,
   or an empty string if no packet is available. */
String getXBeeBufferPacket() {
  const String EMPTY_STRING = "";
  
  // Ensure buffer is not modified while in this routine.
  // At return, we will return held state back to original state.
  bool wasHeld = holdXBeeBuffer();
  
  //noInterrupts();
  //const bool hold = xbeeBufferHold;
  //if (!hold) xBeeBufferHold = true;
  //interrupts();

  // Empty buffer
  if (xbeeBufferElements == 0) {
    if (!wasHeld) releaseXBeeBuffer();
    return EMPTY_STRING;
  }
  
  // Loop over buffer until we find a valid packet
  // or we reach the end.
  while (1) {
    // Remove everything prior to first start token
    while ((xbeeBufferElements > 0) && (xbeeBuffer[(xbeeBufferHead - xbeeBufferElements) % XBEE_BUFFER_SIZE] != PACKET_START_TOKEN)) {
      xbeeBufferElements--;
    }
    // (Nearly) empty buffer: no valid packets
    if (xbeeBufferElements < 2) break;
    // Look for end token
    size_t endLoc = (xbeeBufferHead - xbeeBufferElements + 1) % XBEE_BUFFER_SIZE;
    while ((endLoc != xbeeBufferHead) && (xbeeBuffer[endLoc] != PACKET_END_TOKEN)) {
      endLoc = (endLoc + 1) % XBEE_BUFFER_SIZE;
    }
    if (endLoc == xbeeBufferHead) break;
    // Search backwards from end for start token, in case there are multiple
    size_t startLoc = (endLoc - 1) % XBEE_BUFFER_SIZE;
    while (xbeeBuffer[startLoc] != PACKET_START_TOKEN) {
      startLoc = (startLoc - 1) % XBEE_BUFFER_SIZE;
    }
    if (startLoc != (xbeeBufferHead - xbeeBufferElements) % XBEE_BUFFER_SIZE) {
      xbeeBufferElements = (xbeeBufferHead - startLoc) % XBEE_BUFFER_SIZE;
      Serial.println(F("Warning: Invalid XBee data dropped (possible buffer overrun)."));
    }
    // Ignore empty packets
    if ((endLoc - startLoc) % XBEE_BUFFER_SIZE <= 2) {
      xbeeBufferElements = (xbeeBufferHead - endLoc + 1) % XBEE_BUFFER_SIZE;
      Serial.println(F("Warning: Dropped empty XBee packet."));
      continue;
    }
    // Now extract packet characters, excluding start & end tokens
    const size_t packetBufSize = (endLoc - startLoc) % XBEE_BUFFER_SIZE;
    char packetBuf[packetBufSize];
    for (size_t pos = 0; pos < packetBufSize-1; pos++) {
      packetBuf[pos] = xbeeBuffer[(startLoc+1+pos) % XBEE_BUFFER_SIZE];
    }
    packetBuf[packetBufSize-1] = '\0';
    if (!wasHeld) releaseXBeeBuffer();
    return packetBuf;
  }
  // If we reach here, we did not find a valid packet
  if (!wasHeld) releaseXBeeBuffer();
  return EMPTY_STRING;
}

void processXBee() {
  // Prevent buffer from being altered by ISR
  holdXBeeBuffer();
  
  //noInterrupts();
  //xbeeBufferHold = true;
  //interrupts();

  // Not necessary if an ISR is used to pull data from the
  // Arduino serial buffer, but it doesn't hurt.
  readXBee();
  
  // If there was a buffer overrun, remove any incomplete packet
  // at the end.
  if (xbeeBufferOverrun > 0) {
    cleanXBeeBuffer(true,true);
    xbeeBufferOverrun = 0;
  }

  // Timing note: The web upload routines can take a long time
  // to complete.  We re-enable the XBee buffer read ISR to
  // prevent buffer overrun during this time. The
  // getXBeeBufferPacket() is ISR-safe.
  releaseXBeeBuffer();

  // Cycle over packets until we find a valid one.
  // TIMING NOTE: 
  String packet;
  bool uploaded = false;
  while((packet = getXBeeBufferPacket()).length() > 0) {
    Serial.print(F("XBee packet: "));
    Serial.println(packet);
    Serial.flush();
    switch(packet.charAt(0)) {
      case 'V':
//        noInterrupts();
//        xbeeBufferHold = false;
//        interrupts();
        xbeeReading(packet);
        uploaded = true;
        break;
      case 'R':
//        noInterrupts();
//        xbeeBufferHold = false;
//        interrupts();
        xbeeRate(packet);
        uploaded = true;
        break;
      case 'S':
//        noInterrupts();
//        xbeeBufferHold = false;
//        interrupts();
        set1 = packet;
        if (set1.length() > 0 && set2.length() > 0) {
          xbeeSettings(set1, set2);
          uploaded = true;
          set1 = "";
          set2 = "";
        }
        break;
      case 'T':
//        noInterrupts();
//        xbeeBufferHold = false;
//        interrupts();
        set2 = packet;
        if (set1.length() > 0 && set2.length() > 0) {
          xbeeSettings(set1, set2);
          uploaded = true;
          set1 = "";
          set2 = "";
        }
        break;
      // Invalid packet: do nothing
      default:
        break;
    }
    // If a packet was uploaded, do not parse another one
    // in this function call to avoid spending an extended
    // time in this routine.
    //if (uploaded) break;
    // Use below to avoid compiler warning if above commented...
    (void)uploaded;
  }
  
  // Allow ISR to add data to buffer
//  noInterrupts();
//  xbeeBufferHold = false;
//  interrupts();
}

void uploadXBee(){
  String incoming;

  /*
  //cli();
  if (xbee.available()) {
    Serial.print(F("XBee read: "));
    while (xbee.available()){
      BuffXBee[BuffHead] = xbee.read();
      if(BuffXBee[BuffHead] != -1) {
        Serial.print(BuffXBee[BuffHead]);
        BuffHead++;
        if(BuffHead >= BUFFXBEE_SIZE)
          BuffHead = 0;
        if (BuffHead == BuffTail) {
          BuffOverrun = true;
        }
        if (BuffOverrun) {
          BuffOverruns++;
        }
      }
    }
    Serial.println();
  }
  //sei();

  if (BuffOverrun) {
    Serial.print("WARNING: XBee buffer overran by ");
    Serial.print(BuffOverruns);
    Serial.println(" bytes");
    BuffOverrun = false;
    BuffOverruns = 0;
    BuffHead = 0;
    BuffTail = 0;
  }
  */
  
  // Prevent buffer from being altered by ISR
  noInterrupts();
  xbeeBufferHold = true;
  interrupts();

  // Not necessary if an ISR is used to pull data from the
  // Arduino serial buffer, but it doesn't hurt.
  //Serial.print(F("readXBee:    "));
  //Serial.println(millis());
  readXBee();
  
  // If there was a buffer overrun, remove any incomplete packet
  // at the end.
  if (xbeeBufferOverrun > 0) {
    Serial.print("WARNING: XBee buffer overran by ");
    Serial.print(xbeeBufferOverrun);
    Serial.println(" bytes");
    //cleanXBeeBuffer(true,true);
    xbeeBufferHead = 0;
    xbeeBufferElements = 0;
    xbeeBufferOverrun = 0;
  }

  BuffHead = xbeeBufferHead;
  BuffTail = (xbeeBufferHead - xbeeBufferElements) % XBEE_BUFFER_SIZE;

  if (BuffTail == BuffHead) {
    //Serial.println("Nothing in buffer to process.");
    noInterrupts();
    xbeeBufferHold = false;
    interrupts();
    return; // we have nothing in the buffer
  }
  
  // first get the type of packet
  char pkt_type = xbeeBuffer[BuffTail];

  // then get the packet itself
  unsigned int str_idx = BuffTail;
  while (xbeeBuffer[str_idx] != ';' && str_idx != BuffHead) {
    incoming.concat(xbeeBuffer[str_idx]);
    str_idx++;
    if (str_idx >= XBEE_BUFFER_SIZE) {
      str_idx = 0;
    }
  }

  // we got a full packet
  if ((str_idx != BuffHead) && xbeeBuffer[str_idx] == ';') {
    BuffTail = str_idx + 1;
    if (BuffTail >= XBEE_BUFFER_SIZE) {
      BuffTail = 0;
    }
  // Reached head before finding end of packet
  } else {
    // we got something corrupt, so drop it
    if (incoming.length() > 64) {
      BuffTail = str_idx;
    }
    // no full packet (yet)
    noInterrupts();
    xbeeBufferHold = false;
    interrupts();
    return;
  }
  /*
  if (xbeeBuffer[str_idx] == ';' && str_idx != BuffHead) {
    // we got a full packet
    BuffTail = str_idx + 1;
    if (BuffTail >= XBEE_BUFFER_SIZE) {
      BuffTail = 0;
    }
  } else if (incoming.length() > 64) {
    // we got something corrupt, so drop it
    BuffTail = str_idx + 1;
    while (BuffTail != BuffHead && xbeeBuffer[BuffTail] != ';') {
      BuffTail++;
      if (BuffTail >= XBEE_BUFFER_SIZE) {
        BuffTail = 0;
      }
    }
    noInterrupts();
    xbeeBufferHold = false;
    interrupts();
    return;
  } else {
    // didn't get a full packet, but we may yet
    noInterrupts();
    xbeeBufferHold = false;
    interrupts();
    return;
  }
  */
  
  Serial.println("XBee packet: "+incoming);
  //Serial.print("Free RAM: ");
  //Serial.println(freeRAM());
  Serial.flush();
  
  // TIMING NOTE: The web upload can take ~ 1 second to complete,
  // during which the XBee is not being read.  This leads to
  // the Arduino 64-byte buffer overflowing if 2+ XBee packets
  // arrive during this time....
  switch(pkt_type) {
    case 'V':
      xbeeReading(incoming);
      break;
    case 'R':
      xbeeRate(incoming);
      break;
    case 'S':
      set1 = (incoming);
      if (set1.length() > 0 && set2.length() > 0) {
        xbeeSettings(set1, set2);
        set1 = "";
        set2 = "";
      }
      break;
    case 'T':
      set2 = (incoming);
      if (set1.length() > 0 && set2.length() > 0) {
        xbeeSettings(set1, set2);
        set1 = "";
        set2 = "";
      }
      break;
    default:
      // invalid packet, do nothing
      break;
  }
  
  xbeeBufferElements = (xbeeBufferHead - BuffTail) % XBEE_BUFFER_SIZE;
  
  // Allow ISR to add data to buffer
  noInterrupts();
  xbeeBufferHold = false;
  interrupts();
}


void xbeeConfig() {
  String meshMode;
  if (getModeCoord()) {
    meshMode = "1"; //coordinator
  } else {
    meshMode = "0"; //drone
  }

  xbeeUpdateSetting("CE","0"); // Set device to drone before entering new network.
  xbeeUpdateSetting("ID", getNetID());
  
  delay(1100);
  xbee.print("+++");
  delay(1100);
  Serial.print((char)xbee.read());
  xbee.print("ATCE " + meshMode + "\r");
  delay(100);
  Serial.print((char)xbee.read());
  xbee.print("ATWR\r");
  delay(100);
  Serial.print((char)xbee.read());
  xbee.print("ATCN\r");
  delay(100);
  while(xbee.available())
    xbee.read();

}

void xbeeGetMac(byte * macL, uint8_t max_mac_len){
  if (max_mac_len < 6) {
    Serial.println(F("mac array is too short"));
    return;
  }

  //byte macL[6];
  xbeeCommandMode();
  //Serial.println();
  xbee.print("ATSL\r");
  xbee.readBytes(macL,6);
  //xbee.print("ATCN\r");
  delay(100);
  while(xbee.available())
    //Serial.print((char)xbee.read());
    xbee.read();
  //Serial.println();
  //for(int x = 0; x<sizeof(macL);x++)
    //Serial.println(macL[x]);
  //return macL;
}

void xbeeGetNetwork(byte * netID, uint8_t max_net_len){
  //Serial.println(sizeof(netID));
  if (max_net_len < 4){
    Serial.println(F("Network ID array is too short"));
    return;
  }
  
  xbeeRequestSetting("ID");
  xbee.readBytes(netID,5);
  while(xbee.available()){
    Serial.println(F("Leftover Buffer: "));
    Serial.write((char)xbee.read());
  }
  xbeeCloseCommand();
  //Serial.write(netID, sizeof(netID));
}

void xbeeRate(String incoming){
  String did, sensor, val, datetime;
  int one,two,three, four;

  one = incoming.indexOf(',') +1;
  two = incoming.indexOf(',',one) +1;
  three = incoming.indexOf(',',two) +1;
  four = incoming.indexOf(',',three) +1;
  did = incoming.substring(one,two-1);
  sensor = incoming.substring(two, three-1);
  val = incoming.substring(three, four-1);
  datetime = incoming.substring(four);
  /*Serial.println(one);
  Serial.println(two);
  Serial.println(three);
  Serial.println(did);
  Serial.println(sensor);
  Serial.println(val);*/
  updateRate(did,sensor,val,datetime);
}

void xbeeSettings(String incoming, String incoming2){
  String did,location,project; 
  String coordinator,rate,build,teardown,datetime,netid;
  int one,two,three,four,five,six,seven,eight, nine;

  one = incoming.indexOf(',') +1;
  two = incoming.indexOf(',',one) +1;
  three = incoming.indexOf(',',two) +1;
  did = incoming.substring(one,two-1);
  location = incoming.substring(two, three-1);
  project = incoming.substring(three);
  
  four = incoming2.indexOf(',') +1;
  five = incoming2.indexOf(',',four) +1;
  six = incoming2.indexOf(',',five) +1;
  seven = incoming2.indexOf(',',six) +1;
  eight = incoming2.indexOf(',',seven) +1;
  nine = incoming2.indexOf(',',eight) +1;
  coordinator = incoming2.substring(four, five-1);
  rate = incoming2.substring(five,six-1);
  build = incoming2.substring(six,seven-1);
  teardown = incoming2.substring(seven,eight-1);
  datetime = incoming2.substring(eight, nine-1);
  netid = incoming2.substring(nine);
  
  updateConfig(did,location,coordinator,project,rate,build,teardown,datetime,netid);
}

void xbeeReading(String incoming){
  String did, sensor, val, datetime;
  int one,two,three, four;

  one = incoming.indexOf(',') +1;
  two = incoming.indexOf(',',one) +1;
  three = incoming.indexOf(',',two) +1;
  four = incoming.indexOf(',',three) +1;
  did = incoming.substring(one,two-1);
  sensor = incoming.substring(two, three-1);
  val = incoming.substring(three, four-1);
  datetime = incoming.substring(four);
  //Serial.println(one);
  //Serial.println(two);
  //Serial.println(three);
  //Serial.println(did);
  //Serial.println(sensor);
  //Serial.println(val);
  //Serial.println(datetime);
  postReading(did,sensor,val,datetime);
}


void XBeeSend(String message)
{
  #ifdef DEBUG
  writeDebugLog(F("Fxn: XBeeSend"));
  #endif
  char tx[message.length()+1];
  message.toCharArray(tx,message.length()+1);

  Serial.print("XBee send: ");
  Serial.println(tx);
  //Serial.print("Free RAM: ");
  //Serial.println(freeRAM());
  Serial.flush();

  //noInterrupts();
  xbee.write(tx);
  // Give time for serial interface ISRs to run
  //delay(50);
  // Give even more time for serial interface/XBee to run
  //delay(250);
  // Wait for serial output buffer to empty, then give
  // time for XBee to upload
  xbee.flush();
  //interrupts();
  delay(100);
}

void xbeeRequestSetting(String setting){
  xbeeCommandMode();
  xbee.print("AT" + setting + "\r");
  delay(200);
  
}

void xbeeUpdateSetting(String setting, String val) {
  xbeeCommandMode();
  xbee.print("AT" + setting + " " + val + "\r");
  delay(100);
  while(xbee.available()){
    xbee.read();
  }
  xbeeWriteSettings();
  xbeeCloseCommand();
}

bool xbeeCommandMode() {
  byte ack[2];
  delay(1100);
  xbee.print("+++");
  delay(1100);
  xbee.readBytes(ack,2);
  if(ack[0] != 'O' && ack[1] != 'K'){
    return false;
  }
  while(xbee.available())
    xbee.read();
  return true;
}

bool xbeeWriteSettings(){
  byte ack[2];
  xbee.print("ATWR\r");
  delay(100);
  xbee.readBytes(ack,2);
  if(ack[0] != 'O' && ack[1] != 'K'){
    return false;
  }
  return true;
}

void xbeeCloseCommand(){
  xbee.print("ATCN\r");
  delay(100);
  while(xbee.available())
    xbee.read();
}

//--------------------------------------------------------------------------------------------- [Upload Support]

void ethernetSetup() {
  // Ethernet connection
  
  pinMode(ETHERNET_EN, OUTPUT);
  digitalWrite(ETHERNET_EN, HIGH);
  delay(10);

  pinMode(WIZ812MJ_RESET_PIN,OUTPUT);
  digitalWrite(WIZ812MJ_RESET_PIN,LOW);
  delay(2);
  digitalWrite(WIZ812MJ_RESET_PIN,HIGH);

  pinMode(WIZ812MJ_ES_PIN, OUTPUT); 
  Ethernet.init(WIZ812MJ_ES_PIN);

  ethernetBegin();
}

bool ethernetBegin() {
  lastEthernetBegin = millis();
  Serial.println(F("Starting ethernet..."));
  
  // Reset ethernet chip
  digitalWrite(WIZ812MJ_RESET_PIN,LOW);
  delay(1);
  digitalWrite(WIZ812MJ_RESET_PIN,HIGH);
  delay(1);
  
  // Power cycle
  //digitalWrite(ETHERNET_EN,LOW);
  //delay(10);
  //digitalWrite(ETHERNET_EN,HIGH);
  //delay(10);
  
  //Ethernet.init(WIZ812MJ_ES_PIN);
  
  unsigned long eth_timeout = 10000;
  xbeeGetMac(mac, MAC_LEN);
  //Serial.println(F("got mac..."));
  //Serial.write(mac,6);
  if(!Ethernet.begin(mac, eth_timeout)){
    Serial.println(F("Ethernet initialization failed. Readings will not be pushed to remote database."));
    return false;
  }
  else {
    online = true;
    Serial.print("Ethernet initialized. IP: ");
    Serial.println(Ethernet.localIP());
    Udp.begin(localPort);
    getTimeFromWeb();
    return true;
  }
}

bool ethernetOnline() {
  return online;
}

void ethernetMaintain() {
  unsigned long t0 = millis();
  if (((t0 - lastEthernetConnect) >= ETHERNET_RECONNECT_TIME)
      && ((t0 - lastEthernetBegin) >= ETHERNET_RECONNECT_TIME)) {
    Serial.println("Extended period without successful internet connection.  Restarting ethernet...");
    ethernetBegin();
    return;
  }
  int stat = Ethernet.maintain(); // Must be performed regularly to maintain connection
  switch(stat) {
    case 0:
      // No action performed
      break;
    case 1:
      Serial.println(F("Ethernet: DHCP renewal failed."));
      break;
    case 2:
      Serial.println(F("Ethernet: DHCP renewal succeeded."));
      break;
    case 3:
      Serial.println(F("Ethernet: DHCP rebind failed."));
      break;
    case 4:
      Serial.println(F("Ethernet: DHCP rebind succeeded."));
      break;
    default:
      Serial.print(F("Ethernet: Unknown DHCP maintain error ("));
      Serial.print(stat);
      Serial.println(F(")."));
      break;
  }
}

void saveReading(String lstr, String rstr, String atstr, String gtstr, String sstr, String c2str, String p1str, String p2str, String cstr) {
  String Dstamp = formatDate();
  String Tstamp = formatTime();
  String DTstamp = Dstamp + " " + Tstamp;
  String sensorData = (Dstamp + ", " + Tstamp + ", " + lstr + ", " + rstr + ", " + atstr + ", " + gtstr + ", " + sstr + ", " + c2str + ", " + p1str + ", " + p2str + ", " + cstr);
  logDataSD(sensorData);

  if (lstr != "")
    postReading(getDevID(), "Light", lstr, DTstamp);
  if (rstr != "")
    postReading(getDevID(), "Humidity", rstr, DTstamp);
  if (atstr != "")
    postReading(getDevID(), "AirTemp", atstr, DTstamp);
  if (gtstr != "")
    postReading(getDevID(), "GlobalTemp", gtstr, DTstamp);
  if (sstr != "")
    postReading(getDevID(), "Sound", sstr, DTstamp);
  if (c2str != "")
    postReading(getDevID(), "CO2", c2str, DTstamp);
  if (p1str != "")
    postReading(getDevID(), "PM_2.5", p1str, DTstamp);
  if (p2str != "")
    postReading(getDevID(), "PM_10", p2str, DTstamp);
  if (cstr != "")
    postReading(getDevID(), "CO", cstr, DTstamp);
}

void postReading(String DID, String ST, String R, String DT)
{
  #ifdef DEBUG
  writeDebugLog(ST);
  #endif
  if(getModeCoord()){
    // String in temp string is data to be submitted to MySQL
    char p[200];
    String amp = "&";
    //String Datetime = getStringDatetime();
    String temp = "DeviceID=" + DID + amp + "SensorType=" + ST + amp + "Reading=" + R + amp + "ReadTime=" + DT;
    temp.toCharArray(p,200);
    if(!postPage(getServer(),serverPort,pageName,p)){
      Serial.println(F("Failed to upload sensor reading to remote. \n"));
      #ifdef DEBUG
      writeDebugLog(F("Failed to upload sensor reading to remote. \n"));
      #endif
    } else {
      //Serial.println(F("Uploaded sensor reading."));
      Serial.println(F("Uploaded sensor reading (")+ST+" @ "+DID+").");
    }
  } else {
    String message = "V,"+DID+","+ST+","+R+","+DT+";"; 
    XBeeSend(message);
    delay(1000);
  }
}

void updateRate(String DID, String ST, String R, String DT)
{
  if(getModeCoord()){
    char p[200];
    String amp = "&";
    //String Datetime = getStringDatetime();
    String temp = "DeviceID=" + DID + amp + "SensorType=" + ST + amp + "SampleRate=" + R + amp + "RateChange=" + DT;
    temp.toCharArray(p,200);
    if(!postPage(getServer(),serverPort,pageName,p)) {
      Serial.println(F("Failed to update sensor rate on remote."));
    } else {
      Serial.println(F("Uploaded sensor rate."));
    }
  } else {
    String message = "R,"+DID+","+ST+","+R+","+DT+";";
    Serial.println("XBee String: " + message);
    XBeeSend(message);
    delay(2500);
  }
}

void updateConfig(String DID, String Location, String Coordinator, String Project, String Rate, String Setup, String Teardown, String Datetime, String NetID)
{
  Datetime = getStringDatetime();
  if(getModeCoord()) {
    char p[200];
    String amp = "&";
    String temp = ("DeviceID=" + DID + amp + "Project=" + Project + amp + "Coordinator=" + Coordinator+ amp + "UploadRate=" + Rate + amp + "Location=" + Location + amp + "SetupDate=" + Setup + amp + "TeardownDate=" + Teardown + amp + "ConfigChange=" + Datetime + amp + "NetID=" + NetID);
    temp.toCharArray(p,200);
    if(!postPage(getServer(),serverPort,pageName,p)) {
      Serial.println(F("Failed to update device configuration on remote."));
    } else {
      Serial.println(F("Uploaded device configuration."));
    }
  } else {
    String message = "S,"+DID+","+Location+","+Project+";"; // Out of order from function call to balance XBee packet size
    String message2 = "T,"+Coordinator+","+Rate+","+Setup+","+Teardown+","+Datetime+","+NetID+";"; // out of order from function call to balance XBee packet size
    Serial.println("XBee String: " + message + message2+" " + message.length() + " " + message2.length());
    XBeeSend(message);
    delay(2000);
    XBeeSend(message2);
  }
}

// postPage is function that performs POST request and prints results.
byte postPage(const char* domainBuffer, int thisPort, const char* page, const char* thisData)
{
  #ifdef DEBUG
  writeDebugLog(F("Fxn: postPage()"));
  #endif
  //int inChar;
  char outBuf[200];
  EthernetClient client;

  //Serial.print(F("connecting..."));

  int stat;
  if((stat = client.connect(domainBuffer,thisPort)) == 1)
  {
    //Serial.println(F("connected"));
    sprintf(outBuf,"POST %s HTTP/1.1",page);
    client.println(outBuf);
    sprintf(outBuf,"Host: %s",domainBuffer);
    client.println(outBuf);
    client.println(F("Connection: close\r\nContent-Type: application/x-www-form-urlencoded"));
    sprintf(outBuf,"Content-Length: %u\r\n",strlen(thisData));
    client.println(outBuf);

    client.print(thisData);
    lastEthernetConnect = millis();
  } else {
    //Serial.println(F("failed"));
    switch(stat) {
      case 1:
        // Success
        break;
      case 0:
        Serial.println(F("Remote server upload failed"));
        break;
      // Below are only for DNS lookup errors?
      case -1:
        Serial.println(F("Remote server upload failed: timed out"));
        break;
      case -2:
        Serial.println(F("Remote server upload failed: invalid server"));
        break;
      case -3:
        Serial.println(F("Remote server upload failed: truncated"));
        break;
      case -4:
        Serial.println(F("Remote server upload failed: invalid response"));
        break;
      default:
        Serial.print(F("Remote server upload failed: unknown error ("));
        Serial.print(stat);
        Serial.println(F(")."));
        break;
    }
    return 0;
  }

  client.stop();

  return 1;
}

void getTimeFromWeb(){
  sendNTPpacket(timeServer); // send an NTP packet to a time server

  // wait to see if a reply is available
  delay(1000);
  if (Udp.parsePacket() == NTP_PACKET_SIZE) {
    // We've received a packet, read the data from it
    Udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer

    // the timestamp starts at byte 40 of the received packet and is four bytes,
    // or two words, long. First, extract the two words:

    unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
    unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
    // combine the four bytes (two words) into a long integer
    // this is NTP time (seconds since Jan 1 1900):
    unsigned long secsSince1900 = highWord << 16 | lowWord;
    Serial.print(F("Seconds since Jan 1 1900 = "));
    Serial.println(secsSince1900);

    // now convert NTP time into everyday time:
    Serial.print(F("Unix time = "));
    // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
    const unsigned long seventyYears = 2208988800UL;
    const unsigned long sevenHours = 25200UL; //Time zone difference.
    // subtract seventy years:
    unsigned long epoch = secsSince1900 - seventyYears - sevenHours;
    // print Unix time:
    if (epoch > SEC_2017) {
        setRTCTime(epoch);
    }

  } else Serial.println(F("Unable to get server time"));
}

// send an NTP request to the time server at the given address
void sendNTPpacket(const char* address) {
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;

  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

