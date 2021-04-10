/*
 * HBWValve.cpp
 *
 * Created on: 05.05.2019
 * loetmeister.de
 * 
 * Based on work by: Harald Glaser
 */
 
#include "HBWValve.h"


HBWValve::HBWValve(uint8_t _pin, hbw_config_valve* _config)
{
  config = _config;
  pin = _pin;
  
  outputChangeNextDelay = OUTPUT_STARTUP_DELAY;
  outputChangeLastTime = 0;
  stateFlags.byte = 0;
  initDone = false;
  clearFeedback();
  
  digitalWrite(pin, OFF);
  pinMode(pin, OUTPUT);
}


// channel specific settings or defaults
void HBWValve::afterReadConfig()
{
  if (config->error_pos == 0xFF)  config->error_pos = 30;   // 15%
  if (config->valveSwitchTime == 0xFF || config->valveSwitchTime == 0)  config->valveSwitchTime = 18; // default 180s (factor 10!)

  if (!initDone) {
    valveLevel = config->error_pos;
    isFirstState = true;
    initDone = true;
  }
  nextState = init_new_state();
}


/*
 * set the desired Valve State in Manual Mode level = 0 - 200 like a Blind or Dimmer
 * Special values:
 * 201 - toggle automatic/manual
 * 205 - automatic (locks the channel to be controlled by linked PID channel)
 * 203 - manual (set error position 1st. Then allow any level 0...100%)
 */
/* standard public function - set a channel, directly or via peering event. Data array contains new value or all peering details */
void HBWValve::set(HBWDevice* device, uint8_t length, uint8_t const * const data)
{
  set(device, length, data, false);
}

// slighlty customized set() function, to allow PID channels to set level in automatic mode
void HBWValve::set(HBWDevice* device, uint8_t length, uint8_t const * const data, bool setByPID)
{
  if (!config->unlocked || !setByPID) return;  // locked channels can still be set by PID, but are blocked for external changes

/* TODO: Check if we allow setting level always (even when inAuto), but use the AUTO flag to fallback to error_pos if no set() was called
 * for some time when inAuto (? switch_time *x?). PIDs should still sync the inAuto flag, to not overwrite manual set levels */
  if ((*data >= 0 && *data <= 200) && (stateFlags.element.inAuto == MANUAL || setByPID))  // right limits only if manual or setByPID
  {
    setNewLevel(device, *data);
    
#ifdef DEBUG_OUTPUT
hbwdebug(F("Valve set, level: ")); hbwdebug(valveLevel);
hbwdebug(F(" inAuto: ")); hbwdebug(stateFlags.element.inAuto); hbwdebug(F("\n"));
#endif
  }
  else
  {
    switch (*data)
    {
      case SET_TOGGLE_AUTOMATIC:    // toogle PID mode
        stateFlags.element.inAuto = !stateFlags.element.inAuto;
        break;
      case SET_AUTOMATIC:
        stateFlags.element.inAuto = AUTOMATIC;
        break;
      case SET_MANUAL:
        stateFlags.element.inAuto = MANUAL;
        break;
    }
    setNewLevel(device, stateFlags.element.inAuto ? config->error_pos : valveLevel);
    
#ifdef DEBUG_OUTPUT
hbwdebug(F("Valve set mode, inAuto: ")); hbwdebug(stateFlags.element.inAuto); hbwdebug(F("\n"));
#endif
  }
}

void HBWValve::setNewLevel(HBWDevice* device, uint8_t NewLevel)
{
  if (valveLevel != NewLevel)  // set new state only if different
  {
    valveLevel < NewLevel ? stateFlags.element.upDown = 1 : stateFlags.element.upDown = 0;
    valveLevel = NewLevel;
    isFirstState = true;
    nextState = init_new_state();
	//TODO: Add timestamp here (or  use lastFeedbackTime?), to keep track of updated valve position for anti-stick?

    // Logging
    setFeedback(device, config->logging);
  }
}


/* standard public function - returns length of data array. Data array contains current channel reading */
uint8_t HBWValve::get(uint8_t* data)
{
  *data++ = valveLevel;
  *data = stateFlags.byte;

  return 2;
}


// helper functions to allow integration with PID channels (access to private variables)
bool HBWValve::getPidsInAuto()
{
  return stateFlags.element.inAuto;
}

void HBWValve::setPidsInAuto(bool newAuto)
{
  stateFlags.element.inAuto = newAuto;
}


/* standard public function - called by device main loop for every channel in sequential order */
void HBWValve::loop(HBWDevice* device, uint8_t channel)
{
  // startup handling. Only relevant if all channel remain at same error pos.
  if (outputChangeLastTime == 0 && outputChangeNextDelay == OUTPUT_STARTUP_DELAY) {
    outputChangeNextDelay = OUTPUT_STARTUP_DELAY * (channel + 1);
  }

  uint32_t now = millis();

  if (now - outputChangeLastTime >= (uint32_t)outputChangeNextDelay *100)
  {
    switchstate(nextState);
    outputChangeLastTime = now;
  }
  
  // feedback trigger set?
  checkFeedback(device, channel);
}


// called by loop() with next state, if delay time has passed
void HBWValve::switchstate(bool State)
{
  outputChangeNextDelay = set_timer(isFirstState, nextState);
  nextState = (State == VENTON ? VENTOFF : VENTON);
  stateFlags.element.status = (nextState ^ config->n_inverted);
  digitalWrite(pin, stateFlags.element.status);
  isFirstState = false;
  
 #ifdef DEBUG_OUTPUT
  hbwdebug(F("switchtstate, pin: ")); hbwdebug(pin);
  State == VENTOFF ? hbwdebug(F(" VENTOFF")) : hbwdebug(F(" VENTON"));
  hbwdebug(F(" next delay: ")); hbwdebug((uint32_t)outputChangeNextDelay *100); hbwdebug(F("\n"));
 #endif
}


uint16_t HBWValve::set_timer(bool firstState, bool Status)
{
  if (firstState == true)
    return set_peakmiddle(onTimer, offTimer);

  if (Status == VENTON)  //on
    return onTimer;
  else
    return offTimer;
}


/* bisect the timer the first time */
uint16_t HBWValve::set_peakmiddle (uint16_t ontimer, uint16_t offtimer)
{
  if (first_on_or_off(ontimer, offtimer))
    return ontimer / 2;
  else
    return offtimer / 2;
}


bool HBWValve::first_on_or_off(uint16_t ontimer, uint16_t offtimer)
{
  return (ontimer >= offtimer);
}


bool HBWValve::init_new_state()
{
  onTimer = set_ontimer(valveLevel); // TODO?: option to reduce by 0...15%? e.g. (valveLevel > config->valvePctCap) ? (valveLevel - config->valvePctCap) : 0)
  offTimer = set_offtimer(onTimer);
  
  #ifdef DEBUG_OUTPUT
  hbwdebug(F("Valve init_new_state, onTimer: "));  hbwdebug((uint32_t)onTimer*100);
  hbwdebug(F(" offTimer: "));  hbwdebug((uint32_t)offTimer*100);
  hbwdebug(F(" valveSwitchTime: "));  hbwdebug((uint32_t)config->valveSwitchTime *10000);  hbwdebug(F("\n"));
  #endif
  
  if (first_on_or_off(onTimer, offTimer)) {
    return VENTON;
  } else {
    return VENTOFF;
  }
}


uint16_t HBWValve::set_ontimer(uint8_t VentPositionRequested) {
    return ((((uint16_t)config->valveSwitchTime) * VentPositionRequested) / 2);
}


uint16_t HBWValve::set_offtimer(uint16_t ontimer) {
    return ((uint16_t)config->valveSwitchTime *100 - ontimer);
}
