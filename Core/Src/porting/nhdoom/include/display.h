/* nhdoom host port: shadow of src/display.h.
 * Plain prototypes for the LCD HAL the engine references, plus screen dims. */
#ifndef DISPLAY_H
#define DISPLAY_H
#include <stdbool.h>
#include <stdint.h>

#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 240
#define MAXCOLS (SCREEN_WIDTH / 8)

void UpdateDisplay(void);
void DisplayInit(void);
void BeginUpdateDisplay(void);
void EndUpdateDisplay(void);
void SelectDisplay(void);
void DisplayWriteData(uint8_t value);
void initDisplaySpi(void);
void SetBacklight(bool on);
#endif
