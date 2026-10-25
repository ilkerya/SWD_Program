I’ve written the complete Arduino sketch for the programmer. It hasn’t been compiled for the Giga or tried on hardware yet: the Arduino download server is blocked from my workspace, so I only checked it for C++ errors against placeholder versions of the libraries.



One design change: the STM32G071 has no JTAG port, only SWD (Serial Wire Debug, a 2-wire debug interface). So the programmer uses SWD for both chips, on the same TMS and TCK pins a JTAG connector already has. It works on the F405 as well.



How a run works



The START input goes low. BUSY goes high and ERROR is cleared.

The Giga connects and recognises the chip on its own (F405 or G071). If the chip doesn’t answer, it retries with the chip’s reset line held low.

It takes the .bin from the matching folder on the SD card, /F405 or /G071. Each folder must hold exactly one .bin. Before flashing it checks that the file really is an STM32 firmware image.

It erases, programs and verifies. Each step has its own progress bar on the screen, next to the file name, file size and chip type.

On success PASS goes high and the chip is reset so it starts running. On any error ERROR goes high, the error text is shown in red, and ERROR stays high until the next START.



Wiring



Giga pin	Goes to

D10 / D11 / D12 / D13	SD reader CS / MOSI / MISO / SCK. The SD reader’s VCC goes to 5V.

D22	Target SWCLK (PA14; F405 pin 49)

D23	Target SWDIO (PA13; F405 pin 46)

D24	Target NRST (F405 pin 7)

GND	Target GND. Ground must be shared.

D2	START: a button or open-collector output to GND (internal pull-up)

D3 / D4 / D5	ERROR / BUSY / PASS outputs (3.3 V, a few mA)



The display shield plugs into the connectors on the back of the Giga, so these header pins stay free. For a 24 V PLC, put optocouplers or transistors on the inputs and outputs.



SD card reader: I’d use the Treedix MicroSD breakout (2 pcs). It has a voltage regulator and level conversion on the board, so it works with the Giga’s 3.3 V signals. I couldn’t open the Amazon page itself, so check the pin labels when it arrives. A cheaper option is the DAOKAI 5-pack, but it needs its VCC on the Giga’s 5V pin. The Giga’s normal SPI pins are D10–D13; the 6-pin ICSP header is the other option (Arduino forum thread).



Setup and limits



Libraries: in the Arduino IDE, install SdFat, Arduino\_GigaDisplay\_GFX and Adafruit GFX. Select the “Arduino Giga R1” board (M7 core).

SD card: format it as FAT32. The firmware files must be raw .bin files built for address 0x08000000.

Read-out protection: a chip with read-out protection switched on fails with an error. Removing that protection isn’t built in.

Speed: the debug clock is 1 MHz, so a full 1 MB F405 should take about 30–40 s, and the 64 KB G071 a few seconds. Both are estimates. With short wires you can raise it to about 4 MHz with SWD\_FREQ\_HZ.

Settings: all pin numbers and options are in config.h.



I also saved a design note with the pin map and these decisions to the Jtag Programmer project.





I renamed the Arduino sketch to SWD\_Program, so you now open SWD\_Program/SWD\_Program.ino. The Arduino IDE needs the folder and the .ino file to have the same name, so I renamed both. I also changed the name in the code comments and in the design note in the project.



I can’t rename the claude.ai project itself, which is still called “Jtag Programmer”. You can rename it in claude.ai if you want that changed too.



Files are in SWD\_Program:



I made a state machine diagram of the firmware and added it to the sketch folder as docs/state\_machine in three versions: .mmd (the Mermaid source you can edit), .svg and .png.



It shows every step from power-on to the end of a flashing run, follows the code exactly, and lists what the ERROR, BUSY and PASS outputs do in each step:



Idle: the board starts up, then waits for START. While waiting it re-reads the SD card every 3 seconds, but only until the first run.

Flashing run: START begins a run. It goes from reading the SD card and connecting to the chip, through checking the file, to erase, program and verify. If the chip doesn’t answer at first, it retries with the reset line held low.

End of run: a run ends in PASS or ERROR. An error at any step goes straight to ERROR, and ERROR stays high until the next START.



Files are in SWD\_Program:



docs/state\_machine.png

SWD\_Program.zip



find the link if the Arduino sd card library that has been used



Searched the web greiman SdFat github Arduino library

The SD card library in the sketch is SdFat by Bill Greiman, not Arduino’s built-in SD library:



Source code: https://github.com/greiman/SdFat

Arduino library reference page: arduino.cc/reference/en/libraries/sdfat

To install it, open Library Manager in the Arduino IDE, search for “SdFat” and pick the one by Bill Greiman.



I picked SdFat because SD card examples on the Giga use it (Arduino forum thread). It also lets the sketch pick which SPI port the SD reader is on. This project uses SPI1, the header pins D10–D13.





