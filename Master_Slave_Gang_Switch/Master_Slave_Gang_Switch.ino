//*************************************************************
//**************MASTER_SLAVE GANG SWITCH - PROGRAMMABLE  ******
//*************************************************************
//Version 1.0
//***** Coded by ASM 2019

#include <Arduino.h>
#include <EEPROM.h>
#include "EmonLib.h" // Include Emon Library
#include "OneButton.h"
#include "jled.h"

#define RELAY_PIN 4       //output pin to control relay for peripherals of TV
#define MENU_BN_PIN A1    // menu bn is connected to this digital pin
#define CURR_SENSOR_PIN 0 // pin number for the current sensor. this number pertains to ANALOG pin
#define RED_LED_PIN 3
#define GREEN_LED_PIN 5
#define BLUE_LED_PIN 6

class LedBrightness : public jled::BrightnessEvaluator
{
	uint8_t Eval(uint32_t t) const override
	{
		// this function returns changes between 0 and 255 and
		// vice versa every 250 ms.
		return 127;
	}
	uint16_t Period() const override { return 5000; }
};

LedBrightness ledbrightness;
const int blinkRate = 3;                         // blinking freq in hertz
const float zeroCurrent = .028;                  // 28mA
const int currSamplingRate = 6;                  // per second rate  e.g. 10 times/sec
const int minSamplesForConfirmationOfAction = 5; //Samples to be checked before switching peripherals
const float magicNumber = 1.23;
const int resampleDelay = 4000; //delay to wait before resample before switching relay

//setup Leds for indicator - red breathe, green breathe`
auto red_led = JLed(RED_LED_PIN).FadeOn(200).FadeOff(200).LowActive();
auto green_led = JLed(GREEN_LED_PIN).FadeOn(200).FadeOff(200).LowActive().UserFunc(&ledbrightness);
auto blue_blinking_error_led = JLed(BLUE_LED_PIN).Blink(150, 150).Forever().LowActive();
auto green_blinking_led = JLed(GREEN_LED_PIN).Blink(500, 500).Forever().LowActive().UserFunc(&ledbrightness);
auto red_blinking_led = JLed(RED_LED_PIN).Blink(500, 500).Forever().LowActive();

struct userSettings
{ // struct to store user settings for reference current values
	float standbyStateCurr;
	float onStateCurr;
	float magicNumber;
};

bool redBlinkLedStatus = false; //track status of red led for blinking check

enum tvState
{
	TV_OFF,
	TV_ON,
	TV_NOT_CONNECTED,
	SYS_ERROR
};

enum relayState
{
	OFF,
	ON
};

enum MenuItems
{
	SET_STANDBY_CURRENT,
	SET_ON_CURRENT
};

enum Mode
{
	CHANGE_MODE,
	DONT_CHANGE_MODE,
	DELAY_MODE_CHANGE
};

OneButton settingBn(MENU_BN_PIN, true);
EnergyMonitor currMon; // object instance of current monitor

struct userSettings storedSettings;

bool isInProgrammingMode = false;

int peripheralState = TV_OFF;                 //state monitor variable - initially the state of the peripherals is set
int peripheralDesiredState = peripheralState; //the state in which the peripherals are now desired

int consecutiveSampleCount = 0; //switch peripherals based on predecided consecutive samples confirming the same
int settingMenuItemCount;       //what value are we sampling to set
double sampleValue;             //read sample value during programming/ settings
int isModeChange = CHANGE_MODE; //to check if the mode has just changed. If yes then wait for another 3 seconds and resample it before changing the relay output to prevent undesierd relay flicker
unsigned long resampleDelayStartTime;

//custom functions declarations
void getIntoProgrammingMode();
void setPeripheralsState(int state);
void setUserPref();
void saveSettings();
void switchOffAllLeds();
void bnTick();

int timeCounter; //store millis() of prev reading
bool printOnce = false;

void setup()
{
	pinMode(RELAY_PIN, OUTPUT);     // pin to control relay for peripherals of TV
	pinMode(RED_LED_PIN, OUTPUT);   // RED LED PIN
	pinMode(GREEN_LED_PIN, OUTPUT); // GREEN LED PIN
	currMon.current(CURR_SENSOR_PIN, 30);

	digitalWrite(RED_LED_PIN, LOW);   //switch off red led
	digitalWrite(GREEN_LED_PIN, LOW); //switch off green led
	digitalWrite(RELAY_PIN, LOW);     //switch off relay initially

	setPeripheralsState(peripheralState);

	EEPROM.get(0, storedSettings); //load settings from eeprom
	if (storedSettings.magicNumber != magicNumber)
	{ //check if the settings are already stored in EEPROM. if not then straightway go to programming mode
		isInProgrammingMode = true;
		storedSettings.magicNumber = magicNumber;
	}
	settingBn.attachDoubleClick(getIntoProgrammingMode); //setup the menu bn
	settingBn.attachClick(saveSettings);
	settingMenuItemCount = 0;
	PCICR |= (1 << PCIE1);   // This enables Pin Change Interrupt 1 that covers the Analog input pins or Port C.
	PCMSK1 |= (1 << PCINT9); // This enables the interrupt for pin 1 of Port C: This is A1.
							 // attachInterrupt(digitalPinToInterrupt(MENU_BN_PIN),bnTick, FALLING);
	timeCounter = millis();
}
void bnTick()
{
	settingBn.tick();
}

//ISR to check for bn press
// The Interrupt Service Routine for Pin Change Interrupt 1
// This routine will only be called on any signal change on A1: exactly where we need to check.
ISR(PCINT1_vect)
{
	// keep watching the push button:
	settingBn.tick(); // just call tick() to check the state.
}

void loop()
{

	if (isInProgrammingMode)
	{
		setUserPref();
	}
	else
	{
		if ((millis() - timeCounter) >= currSamplingRate)
		{
			timeCounter = millis();
			currMon.calcIrms(1480);
			if (printOnce)
			{
				printOnce = false;
			}
		}
		if ((currMon.Irms <= 0) || ((storedSettings.standbyStateCurr < currMon.Irms) && (currMon.Irms < storedSettings.onStateCurr)))
		{
			peripheralDesiredState = SYS_ERROR;
		}
		else if (currMon.Irms <= zeroCurrent)
		{
			peripheralDesiredState = TV_NOT_CONNECTED;
		}
		else if (currMon.Irms <= storedSettings.standbyStateCurr)
		{
			peripheralDesiredState = TV_OFF;
		}
		else if (currMon.Irms >= storedSettings.onStateCurr)
		{
			peripheralDesiredState = TV_ON;
		}
		if (peripheralState != peripheralDesiredState)
		{
			switch (isModeChange)
			{
			case CHANGE_MODE:
				isModeChange = DONT_CHANGE_MODE;
				setPeripheralsState(peripheralDesiredState);
				break;
			case DONT_CHANGE_MODE:
				isModeChange = DELAY_MODE_CHANGE;
				resampleDelayStartTime = millis();
				break;
			case DELAY_MODE_CHANGE:
				isModeChange = ((millis() - resampleDelayStartTime) > resampleDelay) ? CHANGE_MODE : isModeChange;
				break;
			}
		}
		else if ((isModeChange == CHANGE_MODE) || (isModeChange = DELAY_MODE_CHANGE))
		{
			isModeChange = DONT_CHANGE_MODE;
		}
	}

	red_led.Update();
	green_led.Update();
	red_blinking_led.Update();
	blue_blinking_error_led.Update();
	green_blinking_led.Update();
	delay(10);
}

void getIntoProgrammingMode()
{
	if (isInProgrammingMode)
		return;
	isInProgrammingMode = true;
}

void setUserPref()
{
	//save user config settings here
	switch (settingMenuItemCount)
	{
	case SET_STANDBY_CURRENT:
		switchOffAllLeds();
		red_blinking_led.Reset();
		break;
	case SET_ON_CURRENT:
		switchOffAllLeds();
		green_blinking_led.Reset();
		break;
	}
	//get current sample @ 20 times per second and keep averaging it
	if ((millis() - timeCounter) >= (currSamplingRate / 2))
	{
		timeCounter = millis();
		currMon.calcIrms(1480);
		sampleValue = (sampleValue != 0) ? (sampleValue + currMon.Irms) / 2 : currMon.Irms;
	}
}

void saveSettings()
{
	if (!isInProgrammingMode)
		return;
	switch (settingMenuItemCount++)
	{
	case SET_STANDBY_CURRENT:
		storedSettings.standbyStateCurr = sampleValue * 1.20; //take 20 % margin extra current the to be on safer side
		sampleValue = 0;
		break;
	case SET_ON_CURRENT:
		storedSettings.onStateCurr = sampleValue * .75; //take 20 % margin less current to be on safer side
		storedSettings.onStateCurr = (storedSettings.onStateCurr < storedSettings.standbyStateCurr) ? storedSettings.standbyStateCurr : storedSettings.onStateCurr;
		sampleValue = 0;
		EEPROM.put(0, storedSettings);
		isInProgrammingMode = false;
		settingMenuItemCount = 0;
		setUserPref();
		printOnce = true; //DELETE
		break;
	default:
		break;
	}
}

void setLedColor(int selectedColor)
{ //sets the LED COLORS for indication of state

	switch (selectedColor)
	{
	case TV_OFF: //solid red color
		switchOffAllLeds();
		red_led.On();
		break;
	case TV_ON: //solid green color
		switchOffAllLeds();
		green_led.On();
		break;
	case TV_NOT_CONNECTED: //solid amber color
		switchOffAllLeds();
		green_led.On();
		red_led.On();
		break;
	default: //flashing red color
		switchOffAllLeds();
		blue_blinking_error_led.Reset();
		break;
	}
}

void setRelayState(int state)
{
	switch (state)
	{
	case TV_OFF:
		digitalWrite(RELAY_PIN, LOW);

		break;
	case TV_ON:
		digitalWrite(RELAY_PIN, HIGH);

		break;
	case TV_NOT_CONNECTED:
		digitalWrite(RELAY_PIN, LOW); //switch off the peripherals if tv is not connected

		break;
	default:
		digitalWrite(RELAY_PIN, LOW);

		break;
	}
}

void setPeripheralsState(int state)
{
	peripheralState = state;
	setLedColor(state);
	setRelayState(state);
}

void switchOffAllLeds()
{
	green_led.Stop();
	red_led.Stop();
	if (red_blinking_led.IsRunning())
		red_blinking_led.Stop();
	if (green_blinking_led.IsRunning())
		green_blinking_led.Stop();
	if (blue_blinking_error_led.IsRunning())
		blue_blinking_error_led.Stop();
}
