//*******************************************************************
//
// HBW-CC-VD-8, RS485 2-channel PID Valve actuator
//
// Homematic Wired Hombrew Hardware
// Arduino NANO als Homematic-Device
// 
// - Direktes Peering für Temperatursensoren möglich. (HBWLinkInfoMessage*)
//
// http://loetmeister.de/Elektronik/homematic/index.htm#modules
//
//*******************************************************************
// Changes
// v0.01
// - initial version


#define HARDWARE_VERSION 0x01
#define FIRMWARE_VERSION 0x0001

#define NUMBER_OF_PID_CHAN 2   // output channels - PID regulator
#define NUMBER_OF_VD_CHAN NUMBER_OF_PID_CHAN   // output channels - valve actuator (has to be same amount as PIDs)
#define NUMBER_OF_TEMP_CHAN 2   // input channels - 1-wire temperature sensors

#define ADDRESS_START_CONF_TEMP_CHAN 0x7

#define NUM_LINKS_PID 20    // address step 7
#define LINKADDRESSSTART_PID 0x140   // ends @0x
#define NUM_LINKS_VD 18    // address step 6
#define LINKADDRESSSTART_VD 0x370   // ends @0x
#define NUM_LINKS_TEMP 18    // address step 6
#define LINKADDRESSSTART_TEMP 0x370   // ends @0x


#define HMW_DEVICETYPE 0x97 //device ID (make sure to import hbw_ .xml into FHEM)

//#define USE_HARDWARE_SERIAL   // use hardware serial (USART) - this disables debug output


// HB Wired protocol and modules
#include <HBWired.h>
#include <HBWOneWireTempSensors.h>
#include "HMWPids.h"
#include <HBWLinkInfoMessageSensor.h>
#include <HBWLinkInfoMessageActuator.h>


// Pins
#ifdef USE_HARDWARE_SERIAL
  #define RS485_TXEN 2  // Transmit-Enable
  #define BUTTON A6  // Button fuer Factory-Reset etc.
  #define ADC_BUS_VOLTAGE A7  // analog input to measure bus voltage
  
  #define ONEWIRE_PIN	A5 // Onewire Bus
  #define VD1  3  // valve "PWM on valium" output
  #define VD2  4
//  #define VD3  5
//  #define VD4  6
//  #define VD5  7
//  #define VD6  8
//  #define VD7  9
//  #define VD8  10

#else
  #define RS485_RXD 4
  #define RS485_TXD 2
  #define RS485_TXEN 3  // Transmit-Enable
  #define BUTTON 8  // Button fuer Factory-Reset etc.
  #define ADC_BUS_VOLTAGE A7  // analog input to measure bus voltage

  #define ONEWIRE_PIN	10 // Onewire Bus
  #define VD1  A1
  #define VD2  A2
//  #define VD3  5
//  #define VD4  6
//  #define VD5  7
//  #define VD6  12
//  #define VD7  9
//  #define VD8  10
  //#define xx10 NOT_A_PIN  // dummy pin to fill the array elements
  
  #include "FreeRam.h"
  #include "HBWSoftwareSerial.h"
  // HBWSoftwareSerial can only do 19200 baud
  HBWSoftwareSerial rs485(RS485_RXD, RS485_TXD); // RX, TX
#endif  //USE_HARDWARE_SERIAL

#define LED LED_BUILTIN        // Signal-LED

#define NUMBER_OF_CHAN NUMBER_OF_VD_CHAN + NUMBER_OF_PID_CHAN + NUMBER_OF_TEMP_CHAN


struct hbw_config {
  uint8_t logging_time;     // 0x01
  uint32_t central_address;  // 0x02 - 0x05
  uint8_t direct_link_deactivate:1;   // 0x06:0
  uint8_t              :7;   // 0x06:1-7
  hbw_config_onewire_temp TempOWCfg[NUMBER_OF_TEMP_CHAN]; // 0x07 - 0x..(address step 14)
  hbw_config_pid pidCfg[NUMBER_OF_PID_CHAN];          // 0x.. - 0x..(address step 9)
  hbw_config_pid_valve pidValveCfg[NUMBER_OF_VD_CHAN];   // 0x.. - 0x..(address step 3)
} hbwconfig;


HBWChannel* channels[NUMBER_OF_CHAN];  // total number of channels for the device

// global pointer for OneWire channels
hbw_config_onewire_temp* tempConfig[NUMBER_OF_TEMP_CHAN]; // pointer for config


class HBVdDevice : public HBWDevice {
    public:
    HBVdDevice(uint8_t _devicetype, uint8_t _hardware_version, uint16_t _firmware_version,
               Stream* _rs485, uint8_t _txen, 
               uint8_t _configSize, void* _config, 
               uint8_t _numChannels, HBWChannel** _channels,
               Stream* _debugstream, HBWLinkSender* linksender = NULL, HBWLinkReceiver* linkreceiver = NULL,
               OneWire* oneWire = NULL, hbw_config_onewire_temp** _tempSensorconfig = NULL) :
    HBWDevice(_devicetype, _hardware_version, _firmware_version,
              _rs485, _txen, _configSize, _config, _numChannels, ((HBWChannel**)(_channels)),
              _debugstream, linksender, linkreceiver) {
                d_ow = oneWire;
                tempSensorconfig = _tempSensorconfig;
    };
    virtual void afterReadConfig();
    
    private:
      OneWire* d_ow;
      hbw_config_onewire_temp** tempSensorconfig;
};

// device specific defaults
void HBVdDevice::afterReadConfig() {
  if (hbwconfig.logging_time == 0xFF) hbwconfig.logging_time = 50;

  HBWOneWireTemp::sensorSearch(d_ow, tempSensorconfig, (uint8_t) NUMBER_OF_TEMP_CHAN, (uint8_t) ADDRESS_START_CONF_TEMP_CHAN);
};

HBVdDevice* device = NULL;


void setup()
{
  // variables for all OneWire channels
  OneWire* g_ow = NULL;
  uint32_t g_owLastReadTime = 0;
  uint8_t g_owCurrentChannel = 255; // init with 255! used as trigger/reset in channel loop()
  g_ow = new OneWire(ONEWIRE_PIN);

  // create channels
  byte valvePin[NUMBER_OF_VD_CHAN] = {VD1, VD2};  // assing pins
  HBWPids* g_pids[NUMBER_OF_PID_CHAN];
  
  for(uint8_t i = 0; i < NUMBER_OF_PID_CHAN; i++) {
    g_pids[i] = new HBWPids(&(hbwconfig.pidValveCfg[i]), &(hbwconfig.pidCfg[i]));
    channels[i] = g_pids[i];
    channels[i + NUMBER_OF_PID_CHAN] = new HBWPidsValve(valvePin[i], g_pids[i], &(hbwconfig.pidValveCfg[i]));
  }
  for(uint8_t i = 0; i < NUMBER_OF_TEMP_CHAN; i++) {
    channels[i + NUMBER_OF_PID_CHAN *2] = new HBWOneWireTemp(g_ow, &(hbwconfig.TempOWCfg[i]), &g_owLastReadTime, &g_owCurrentChannel);
    tempConfig[i] = &(hbwconfig.TempOWCfg[i]);
  }

  // check if HBWLinkInfoMessage support is enabled
  #if !defined(Support_HBWLink_InfoMessage) && defined(NUM_LINKS_PID)
  #error enable/define Support_HBWLink_InfoMessage in HBWired.h
  #endif


#ifdef USE_HARDWARE_SERIAL  // RS485 via UART Serial, no debug (_debugstream is NULL)
  Serial.begin(19200, SERIAL_8E1);
  
  device = new HBVdDevice(HMW_DEVICETYPE, HARDWARE_VERSION, FIRMWARE_VERSION,
                             &Serial, RS485_TXEN, sizeof(hbwconfig), &hbwconfig,
                             NUMBER_OF_CHAN, (HBWChannel**)channels,
                             NULL,
                             new HBWLinkInfoMessageSensor(NUM_LINKS_TEMP,LINKADDRESSSTART_TEMP),
                             new HBWLinkInfoMessageActuator(NUM_LINKS_PID,LINKADDRESSSTART_PID),
                             g_ow, tempConfig);
  
  device->setConfigPins(BUTTON, LED);  // use analog input for 'BUTTON'
  //device->setStatusLEDPins(LED, LED); // Tx, Rx LEDs
  
#else
  Serial.begin(19200);
  rs485.begin();    // RS485 via SoftwareSerial
  
  device = new HBVdDevice(HMW_DEVICETYPE, HARDWARE_VERSION, FIRMWARE_VERSION,
                             &rs485, RS485_TXEN, sizeof(hbwconfig), &hbwconfig,
                             NUMBER_OF_CHAN, (HBWChannel**)channels,
                             &Serial,
                             new HBWLinkInfoMessageSensor(NUM_LINKS_TEMP,LINKADDRESSSTART_TEMP),
                             new HBWLinkInfoMessageActuator(NUM_LINKS_PID,LINKADDRESSSTART_PID),
                             g_ow, tempConfig);
  
  device->setConfigPins(BUTTON, LED);  // 8 (button) and 13 (led) is the default
  //device->setStatusLEDPins(LED, LED); // Tx, Rx LEDs

  hbwdebug(F("B: 2A "));
  hbwdebug(freeRam());
  hbwdebug(F("\n"));
#endif
}


void loop()
{
  device->loop();
};
