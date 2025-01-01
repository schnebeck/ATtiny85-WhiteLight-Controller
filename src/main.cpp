/*
 * Program: LED Lighting Controller
 * Author: Thorsten Schnebeck
 * Date: 2024-12-30
 * 
 * Description:
 * This program controls an LED lighting unit with independently adjustable cold and warm white LED strings 
 * using PWM signals generated by an ATtiny85 microcontroller, utilizing commands from an IR remote control. 
 * It provides functionality to smoothly transition between colors (cold white and warm white), read and store 
 * color settings in EEPROM, and adjust brightness levels. The program utilizes Timer1 for PWM signal generation 
 * and includes routines for setting PWM values, performing smooth fades between colors, and saving/retrieving settings.
* 
 * Key Features:
 * - Reading IR remote control commands (NEC format).
 * - PWM control for dual LED channels (cold white and warm white).
 * - Adjustable brightness and color transitions with specified durations.
 * - Persistent storage of color settings in EEPROM.
 * - Optimized for minimal resource usage on the ATtiny85.
 * 
 * Hardware Requirements:
 * - ATtiny85 microcontroller (8 MHz clock).
 * - LED strings controlled via PB1 and PB4 PWM outputs.
 * - NEC format IR remote control.
 * 
 * Note:
 * This program is designed for embedded systems with limited resources and does not rely on a serial monitor 
 * for debugging or user interaction. Additionally, while the main development was done on a Digispark ATtiny85 
 * board, the microcontroller was programmed using an ISP programmer rather than a bootloader.
 */

#define IRMP_SUPPORT_NEC_PROTOCOL 1 
#define IR_RECEIVE_PIN 2 // PB2 on ATtiny85
#define ARDUINO_AVR_DIGISPARK 1

#include <Arduino.h>
#include <EEPROM.h>
#include <irmp.hpp>

const int EEPROM_COLD_WHITE_ADDR = 0;
const int EEPROM_WARM_WHITE_ADDR = 1;

IRMP_DATA irmp_data;

struct PWMdata {
    uint8_t coldWhite;
    uint8_t warmWhite;
};

struct PWMdata whiteColor = {128, 128};
uint8_t brightness = 128;

const uint8_t presets[5][2] = {
    {255, 1},   // Preset 0
    {255, 128}, // Preset 1
    {255, 255}, // Preset 2
    {128, 255}, // Preset 3
    {1, 255}    // Preset 4
};

uint8_t current_preset = 0;
bool is_on = true;
bool is_night = false;

// Declaration of functions
void mdelay(uint16_t time);
struct PWMdata setBrightness(uint8_t brightness,  struct PWMdata white);
void fadePWM(struct PWMdata start, struct PWMdata stop, int16_t durationMs = 500);
void setupPWM();
void setPWM(struct PWMdata white);
void processNECCommand(uint16_t command);
struct PWMdata readColor();
uint8_t getBrightness(struct PWMdata color);    


void mdelay(uint16_t time) {
    for (uint16_t j = 1; j<time; j++) {
        // delay loop for approx. 1ms @ 8Mhz 
        for (uint16_t i = 0; i < 700; i++) {
            __asm__ __volatile__ (
                "nop\n\t"
                "nop\n\t"
                "nop\n\t" 
                "nop\n\t" 
                "nop\n\t" 
                "nop\n\t" 
                "nop\n\t" 
                "nop\n\t" 
            );    
        }
    }
}

struct PWMdata setBrightness(uint8_t brightness, struct PWMdata white) {
    uint16_t newColdWhitePWM;
    uint16_t newWarmWhitePWM;

    // increase integer mathematics accuracy by using scaling
    const uint16_t scaleFactor = 100;

    if (white.coldWhite >= white.warmWhite) {
        newColdWhitePWM = brightness;
        // add 9/10th of the divisor to the divident for better rounding results
        uint16_t temp = (uint16_t(white.coldWhite) * scaleFactor + ((uint16_t(white.warmWhite) * 9) / 10)) / uint16_t(white.warmWhite);
        newWarmWhitePWM = (brightness * scaleFactor + ((temp * 9) / 10)) / temp;

    } else {
        newWarmWhitePWM = brightness;
        // add 9/10th of the divisor to the divident for better rounding results
        uint16_t temp = (uint16_t(white.warmWhite) * scaleFactor + ((uint16_t(white.coldWhite) * 9) / 10)) / uint16_t(white.coldWhite);
        newColdWhitePWM = (brightness * scaleFactor + ((temp * 9) / 10)) / temp;
    }
    // check valid parameter range (1-255)
    return {uint8_t(constrain(newColdWhitePWM, 1, 255)), uint8_t(constrain(newWarmWhitePWM, 1, 255))};
}

void fadePWM(struct PWMdata start, struct PWMdata stop, int16_t durationMs) {
    // Anzahl der Schritte (z. B. 50 Schritte für sanftes Fading)
    const int16_t steps = 50;
    // single step duration in milliseconds
    int16_t stepDelay = durationMs / steps;

    // step size per channel
    int16_t stepCold = (int16_t(stop.coldWhite) - int16_t(start.coldWhite)) / steps;
    int16_t stepWarm = (int16_t(stop.warmWhite) - int16_t(start.warmWhite)) / steps;

    // initialize start values
    int16_t currentCold = start.coldWhite;
    int16_t currentWarm = start.warmWhite;

    for (uint16_t i = 0; i <= steps; i++) {
        // set current color temperature values
        setPWM((PWMdata){
            uint8_t(constrain(currentCold, 1, 255)),
            uint8_t(constrain(currentWarm, 1, 255))
        });
        // next fading increment/decrement
        currentCold += stepCold;
        currentWarm += stepWarm;

        // Delay per single fading step
        mdelay(stepDelay);
    }

    // To cover rounding problems set stop color temperatur at the end
    setPWM(stop);
}

void setupPWM() {
    // Set PB1 and PB4 as outputs
    DDRB |= (1 << PB1) | (1 << PB4);

    // Configure Timer1 for Fast PWM mode
    TCCR1 = (1 << CTC1) | (1 << COM1A1) |(1 << PWM1A) | (1 << CS12) |  (0 << CS11) | (1 << CS10); // Enable Fast PWM on OC1A
    GTCCR = (1 << COM1B1) | (1 << PWM1B);                                                         // Enable Fast PWM on OC1B

    // Set the PWM frequency by setting OCR1C (TOP value)
    OCR1C = 255; // Maximum resolution (8-bit, 0-255)
}

void setPWM(struct PWMdata white) {
    if (white.coldWhite == 1) OCR1A = 0; else OCR1A = white.coldWhite; // Duty cycle for PB1 (0, 1-255)
    if (white.warmWhite == 1) OCR1B = 0; else OCR1B = white.warmWhite; // Duty cycle for PB4 (0, 1-255)
}

void processNECCommand(uint16_t command) {
    const uint16_t increment = 4;
    struct PWMdata newColor;

    switch (command) {
        case 69: // On-Off toggle
            is_on = !is_on;
            if (is_on) {
                fadePWM((PWMdata){0,0}, whiteColor);
            } else {
                fadePWM(whiteColor, (PWMdata){0,0});
            }
            break;   

        case 71: // Select presettings 0..4
            current_preset = (current_preset + 1) % 5;
            newColor = setBrightness(brightness, (PWMdata){presets[current_preset][0], presets[current_preset][1]});
            fadePWM(whiteColor, newColor);
            whiteColor = newColor;
            break;

        case 9: // brighter
            if (brightness < (255-increment)) brightness += increment; else brightness = 255;
            whiteColor = setBrightness(brightness, whiteColor);
            setPWM(whiteColor);
            break;

        case 7: // darker
            if (brightness >= increment) brightness -= increment; else brightness = 0;
            whiteColor = setBrightness(brightness, whiteColor);
            setPWM(whiteColor);
            break;

        case 25: // colder white
            if (whiteColor.coldWhite < (255-increment)) whiteColor.coldWhite += increment; else whiteColor.coldWhite = 255;
            if (whiteColor.warmWhite >= increment) whiteColor.warmWhite -= increment; else whiteColor.warmWhite = 0;
            if (whiteColor.coldWhite == 0) whiteColor.coldWhite = 1; 
            if (whiteColor.warmWhite == 0) whiteColor.warmWhite = 1; 
            if (whiteColor.coldWhite > whiteColor.warmWhite) {
                brightness = whiteColor.coldWhite;
            } else {
                brightness = whiteColor.warmWhite;
            }
            setPWM(whiteColor);
            break;

        case 64: // warmer white
            if (whiteColor.warmWhite < (255-increment)) whiteColor.warmWhite += increment; else whiteColor.warmWhite = 255;
            if (whiteColor.coldWhite >= increment) whiteColor.coldWhite -= increment; else whiteColor.coldWhite = 0;
            if (whiteColor.coldWhite == 0) whiteColor.coldWhite = 1; 
            if (whiteColor.warmWhite == 0) whiteColor.warmWhite = 1;
            if (whiteColor.coldWhite > whiteColor.warmWhite) {
                brightness = whiteColor.coldWhite;
            } else {
                brightness = whiteColor.warmWhite;
            } 
            setPWM(whiteColor);
            break;

        case 8: // Nightlight
            is_night = !is_night;
            if (is_night) {
                fadePWM(whiteColor, setBrightness(5, (PWMdata){presets[4][0], presets[4][1]}));
            } else {
                whiteColor = readColor();
                brightness = getBrightness(whiteColor);
                fadePWM(set/*
 * Program: LED Lighting Controller
 * Author: Thorsten Schnebeck
 * Date: 2024-12-30
 * 
 * Description:
 * This program controls an LED lighting unit with independently adjustable cold and warm white LED strings 
 * using PWM signals generated by an ATtiny85 microcontroller, utilizing commands from an IR remote control. 
 * It provides functionality to smoothly transition between colors (cold white and warm white), read and store 
 * color settings in EEPROM, and adjust brightness levels. The program utilizes Timer1 for PWM signal generation 
 * and includes routines for setting PWM values, performing smooth fades between colors, and saving/retrieving settings.
* 
 * Key Features:
 * - Reading IR remote control commands (NEC format).
 * - PWM control for dual LED channels (cold white and warm white).
 * - Adjustable brightness and color transitions with specified durations.
 * - Persistent storage of color settings in EEPROM.
 * - Optimized for minimal resource usage on the ATtiny85.
 * 
 * Hardware Requirements:
 * - ATtiny85 microcontroller (8 MHz clock).
 * - LED strings controlled via PB1 and PB4 PWM outputs.
 * - NEC format IR remote control.
 * 
 * Note:
 * This program is designed for embedded systems with limited resources and does not rely on a serial monitor 
 * for debugging or user interaction. Additionally, while the main development was done on a Digispark ATtiny85 
 * board, the microcontroller was programmed using an ISP programmer rather than a bootloader.
 */

#define IRMP_SUPPORT_NEC_PROTOCOL 1 
#define IR_RECEIVE_PIN 2 // PB2 on ATtiny85
#define ARDUINO_AVR_DIGISPARK 1

#include <Arduino.h>
#include <EEPROM.h>
#include <irmp.hpp>

const int EEPROM_COLD_WHITE_ADDR = 0;
const int EEPROM_WARM_WHITE_ADDR = 1;

IRMP_DATA irmp_data;

struct PWMdata {
    uint8_t coldWhite;
    uint8_t warmWhite;
};

struct PWMdata whiteColor = {128, 128};
uint8_t brightness = 128;

const uint8_t presets[5][2] = {
    {255, 1},   // Preset 0
    {255, 128}, // Preset 1
    {255, 255}, // Preset 2
    {128, 255}, // Preset 3
    {1, 255}    // Preset 4
};

uint8_t current_preset = 0;
bool is_on = true;
bool is_night = false;

// Declaration of functions
void mdelay(uint16_t time);
struct PWMdata setBrightness(uint8_t brightness,  struct PWMdata white);
void fadePWM(struct PWMdata start, struct PWMdata stop, int16_t durationMs = 500);
void setupPWM();
void setPWM(struct PWMdata white);
void processNECCommand(uint16_t command);
struct PWMdata readColor();
uint8_t getBrightness(struct PWMdata color);    


void mdelay(uint16_t time) {
    for (uint16_t j = 1; j<time; j++) {
        // delay loop for approx. 1ms @ 8Mhz 
        for (uint16_t i = 0; i < 700; i++) {
            __asm__ __volatile__ (
                "nop\n\t"
                "nop\n\t"
                "nop\n\t" 
                "nop\n\t" 
                "nop\n\t" 
                "nop\n\t" 
                "nop\n\t" 
                "nop\n\t" 
            );    
        }
    }
}

struct PWMdata setBrightness(uint8_t brightness, struct PWMdata white) {
    uint16_t newColdWhitePWM;
    uint16_t newWarmWhitePWM;

    // increase integer mathematics accuracy by using scaling
    const uint16_t scaleFactor = 100;

    if (white.coldWhite >= white.warmWhite) {
        newColdWhitePWM = brightness;
        // add 9/10th of the divisor to the divident for better rounding results
        uint16_t temp = (uint16_t(white.coldWhite) * scaleFactor + ((uint16_t(white.warmWhite) * 9) / 10)) / uint16_t(white.warmWhite);
        newWarmWhitePWM = (brightness * scaleFactor + ((temp * 9) / 10)) / temp;

    } else {
        newWarmWhitePWM = brightness;
        // add 9/10th of the divisor to the divident for better rounding results
        uint16_t temp = (uint16_t(white.warmWhite) * scaleFactor + ((uint16_t(white.coldWhite) * 9) / 10)) / uint16_t(white.coldWhite);
        newColdWhitePWM = (brightness * scaleFactor + ((temp * 9) / 10)) / temp;
    }
    // check valid parameter range (1-255)
    return {uint8_t(constrain(newColdWhitePWM, 1, 255)), uint8_t(constrain(newWarmWhitePWM, 1, 255))};
}

void fadePWM(struct PWMdata start, struct PWMdata stop, int16_t durationMs) {
    // Anzahl der Schritte (z. B. 50 Schritte für sanftes Fading)
    const int16_t steps = 50;
    // single step duration in milliseconds
    int16_t stepDelay = durationMs / steps;

    // step size per channel
    int16_t stepCold = (int16_t(stop.coldWhite) - int16_t(start.coldWhite)) / steps;
    int16_t stepWarm = (int16_t(stop.warmWhite) - int16_t(start.warmWhite)) / steps;

    // initialize start values
    int16_t currentCold = start.coldWhite;
    int16_t currentWarm = start.warmWhite;

    for (uint16_t i = 0; i <= steps; i++) {
        // set current color temperature values
        setPWM((PWMdata){
            uint8_t(constrain(currentCold, 1, 255)),
            uint8_t(constrain(currentWarm, 1, 255))
        });
        // next fading increment/decrement
        currentCold += stepCold;
        currentWarm += stepWarm;

        // Delay per single fading step
        mdelay(stepDelay);
    }

    // To cover rounding problems set stop color temperatur at the end
    setPWM(stop);
}

void setupPWM() {
    // Set PB1 and PB4 as outputs
    DDRB |= (1 << PB1) | (1 << PB4);

    // Configure Timer1 for Fast PWM mode
    TCCR1 = (1 << CTC1) | (1 << COM1A1) |(1 << PWM1A) | (1 << CS12) |  (0 << CS11) | (1 << CS10); // Enable Fast PWM on OC1A
    GTCCR = (1 << COM1B1) | (1 << PWM1B);                                                         // Enable Fast PWM on OC1B

    // Set the PWM frequency by setting OCR1C (TOP value)
    OCR1C = 255; // Maximum resolution (8-bit, 0-255)
}

void setPWM(struct PWMdata white) {
    if (white.coldWhite == 1) OCR1A = 0; else OCR1A = white.coldWhite; // Duty cycle for PB1 (0, 1-255)
    if (white.warmWhite == 1) OCR1B = 0; else OCR1B = white.warmWhite; // Duty cycle for PB4 (0, 1-255)
}

void processNECCommand(uint16_t command) {
    const uint16_t increment = 4;
    struct PWMdata newColor;

    switch (command) {
        case 69: // On-Off toggle
            is_on = !is_on;
            if (is_on) {
                fadePWM((PWMdata){0,0}, whiteColor);
            } else {
                fadePWM(whiteColor, (PWMdata){0,0});
            }
            break;   

        case 71: // Select presettings 0..4
            current_preset = (current_preset + 1) % 5;
            newColor = setBrightness(brightness, (PWMdata){presets[current_preset][0], presets[current_preset][1]});
            fadePWM(whiteColor, newColor);
            whiteColor = newColor;
            break;

        case 9: // brighter
            if (brightness < (255-increment)) brightness += increment; else brightness = 255;
            whiteColor = setBrightness(brightness, whiteColor);
            setPWM(whiteColor);
            break;

        case 7: // darker
            if (brightness >= increment) brightness -= increment; else brightness = 0;
            whiteColor = setBrightness(brightness, whiteColor);
            setPWM(whiteColor);
            break;

        case 25: // colder white
            if (whiteColor.coldWhite < (255-increment)) whiteColor.coldWhite += increment; else whiteColor.coldWhite = 255;
            if (whiteColor.warmWhite >= increment) whiteColor.warmWhite -= increment; else whiteColor.warmWhite = 0;
            if (whiteColor.coldWhite == 0) whiteColor.coldWhite = 1; 
            if (whiteColor.warmWhite == 0) whiteColor.warmWhite = 1; 
            if (whiteColor.coldWhite > whiteColor.warmWhite) {
                brightness = whiteColor.coldWhite;
            } else {
                brightness = whiteColor.warmWhite;
            }
            setPWM(whiteColor);
            break;

        case 64: // warmer white
            if (whiteColor.warmWhite < (255-increment)) whiteColor.warmWhite += increment; else whiteColor.warmWhite = 255;
            if (whiteColor.coldWhite >= increment) whiteColor.coldWhite -= increment; else whiteColor.coldWhite = 0;
            if (whiteColor.coldWhite == 0) whiteColor.coldWhite = 1; 
            if (whiteColor.warmWhite == 0) whiteColor.warmWhite = 1;
            if (whiteColor.coldWhite > whiteColor.warmWhite) {
                brightness = whiteColor.coldWhite;
            } else {
                brightness = whiteColor.warmWhite;
            } 
            setPWM(whiteColor);
            break;

        case 8: // Nightlight
            is_night = !is_night;
            if (is_night) {
                fadePWM(whiteColor, setBrightness(5, (PWMdata){presets[4][0], presets[4][1]}));
            } else {
                whiteColor = readColor();
                brightness = getBrightness(whiteColor);
                fadePWM(setBrightness(5, (PWMdata){presets[4][0], presets[4][1]}), whiteColor);
            }
            break;

        case 12: // 10%
            newColor = setBrightness(25, whiteColor);   // 25 = 10% of 255
            fadePWM(whiteColor, newColor); 
            whiteColor = newColor;
            break;

        case 24: // 50%
            newColor = setBrightness(128, whiteColor);  // 128 = 50% of 255
            fadePWM(whiteColor, newColor); 
            whiteColor = newColor;
            break;

        case 94: // 100%
            newColor = setBrightness(255, whiteColor);  // 255 = 100% of 255
            fadePWM(whiteColor, newColor); 
            whiteColor = newColor;
            break;

        case 28: // D1: Store current color temperatur
            EEPROM.update(EEPROM_COLD_WHITE_ADDR, whiteColor.coldWhite);
            EEPROM.update(EEPROM_WARM_WHITE_ADDR, whiteColor.warmWhite);
            setPWM((PWMdata){0,0});
            mdelay(200);
            setPWM(whiteColor);
            break;
    }
}

struct PWMdata readColor() {
    struct PWMdata color;
    color.coldWhite = EEPROM.read(EEPROM_COLD_WHITE_ADDR);
    color.warmWhite = EEPROM.read(EEPROM_WARM_WHITE_ADDR);
    return color;
}

uint8_t getBrightness(struct PWMdata color) {    
    if (color.coldWhite > whiteColor.warmWhite) {
        return color.coldWhite;
    } else {
        return color.warmWhite;
    }
}

// Startup function
void setup() {
    delay(10);
    pinMode(PB1, OUTPUT);
    pinMode(PB4, OUTPUT);
    digitalWrite(PB1, LOW);
    digitalWrite(PB4, LOW);
    irmp_init();
    setupPWM();

    // load saved settings from EEPROM
    whiteColor = readColor();
    brightness = getBrightness(whiteColor);
    fadePWM((PWMdata){0,0}, whiteColor);
}

// Main loop
void loop() {
    if (irmp_get_data(&irmp_data)) {
        if (irmp_data.protocol != IRMP_ONKYO_PROTOCOL) {
            if (irmp_data.protocol == IRMP_NEC_PROTOCOL && irmp_data.address == 0x00) { 
                if ((irmp_data.flags == 0) || 
                    (irmp_data.command ==  7 ||
                     irmp_data.command ==  9 ||
                     irmp_data.command == 25 ||
                     irmp_data.command == 64
                    )
                   ) {
                    processNECCommand(irmp_data.command);
                }
            }
        }
    }
}
Brightness(5, (PWMdata){presets[4][0], presets[4][1]}), whiteColor);
            }
            break;

        case 12: // 10%
            newColor = setBrightness(25, whiteColor);   // 25 = 10% of 255
            fadePWM(whiteColor, newColor); 
            whiteColor = newColor;
            break;

        case 24: // 50%
            newColor = setBrightness(128, whiteColor);  // 128 = 50% of 255
            fadePWM(whiteColor, newColor); 
            whiteColor = newColor;
            break;

        case 94: // 100%
            newColor = setBrightness(255, whiteColor);  // 255 = 100% of 255
            fadePWM(whiteColor, newColor); 
            whiteColor = newColor;
            break;

        case 28: // D1: Store current color temperatur
            EEPROM.update(EEPROM_COLD_WHITE_ADDR, whiteColor.coldWhite);
            EEPROM.update(EEPROM_WARM_WHITE_ADDR, whiteColor.warmWhite);
            setPWM((PWMdata){0,0});
            mdelay(200);
            setPWM(whiteColor);
            break;
    }
}

struct PWMdata readColor() {
    struct PWMdata color;
    color.coldWhite = EEPROM.read(EEPROM_COLD_WHITE_ADDR);
    color.warmWhite = EEPROM.read(EEPROM_WARM_WHITE_ADDR);
    return color;
}

uint8_t getBrightness(struct PWMdata color) {    
    if (color.coldWhite > whiteColor.warmWhite) {
        return color.coldWhite;
    } else {
        return color.warmWhite;
    }
}

// Startup function
void setup() {
    delay(10);
    pinMode(PB1, OUTPUT);
    pinMode(PB4, OUTPUT);
    digitalWrite(PB1, LOW);
    digitalWrite(PB4, LOW);
    irmp_init();
    setupPWM();

    // load saved settings from EEPROM
    whiteColor = readColor();
    brightness = getBrightness(whiteColor);
    fadePWM((PWMdata){0,0}, whiteColor);
}

// Main loop
void loop() {
    if (irmp_get_data(&irmp_data)) {
        if (irmp_data.protocol != IRMP_ONKYO_PROTOCOL) {
            if (irmp_data.protocol == IRMP_NEC_PROTOCOL && irmp_data.address == 0x00) { 
                if ((irmp_data.flags == 0) || 
                    (irmp_data.command ==  7 ||
                     irmp_data.command ==  9 ||
                     irmp_data.command == 25 ||
                     irmp_data.command == 64
                    )
                   ) {
                    processNECCommand(irmp_data.command);
                }
            }
        }
    }
}