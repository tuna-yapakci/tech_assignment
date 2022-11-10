# tech_assignment


Introduction----------------------------------------------------------------------------------------------------------

This project implements a simple communication protocol between two devices over a single line.
The code is designed to run on a Raspberry Pi 3 Model B Rev 1.2, however it may compile and run on different devices that
run a Linux based operating system with some adjustments.

I had only one Raspberry Pi at the time I was working on this project, thus I wrote the makefile to compile two of the same
driver with different names. During testing I simulated two devices on a single Raspberry Pi, where the communication is
done between two different GPIO pins controlled by two different drivers.

How to use the code, and how it works----------------------------------------------------------------------------------

The code is compiled and tested on a Raspberry Pi 3 Model B Rev 1.2, on Raspbian with kernel version 5.10.103-v7+
The file driver.c implements a kernel device driver. The communication protocol is controled in kernel level.

The file user_level_program.cpp implements a simple user level program that reads user input as messages to send, and prints
any received message. The user app registers itself to the kernel module, then communicates with the other side through it.

The compiled modules can be loaded into the kernel using the loader script provided, or manually using insmod.
When using insmod you need to set two parameters, gpio_pin_number and comm_role.

sudo insmod driver.ko gpio_pin_number=22 comm_role=0

gpio_pin_number can be any GPIO pin available on Raspberry Pi, and comm_role is the communication mode, 0 for master and 1 for slave.
As the communication is done between two identical drivers, the role of the program that is purely dictated by the user input.

Similarly to remove the drivers a remover script is provided, but again rmmod can also be used manually.

rmmod driver

The Wiring-------------------------------------------------------------------------------------------------------------

A picture of the wiring scheme that was used during the testing can be found on the repo.
Red cable is connected to power to generate the pullup.
White and black cables are connected to GPIO pins 22 and 17.
The pullup resistor is 10k ohms (if I understood correctly less would be better for fast pullup, however 10k is the only one I had)

The Communication Protocol---------------------------------------------------------------------------------------------

The protocol implemented in this project is very similar to 1-Wire protocol.

One of the two devices has the role "master", being the one that initiates the communication by sending a reset signal.

As there is no synchronised clock, the communication relies on accurate timings in both devices.

The protocol works as follows:
- The idle state of the line is 1 (high)
- Master initiates the communication by sending a reset signal,
    - Master pulls the line low for 300us
        - If the master no message to send, keeps the line at low for 200us more while slave samples, then releases for 50us
        - If the master has a message to send, he releases the line for 250us 
    - Slave pulls the line low for 100us to signal master that it is present and releases for 50us
    - Slave pulls the line low for another 100us if it has a message to send and releases for 100us
Reset period takes around 900us in total

Depending on the information that is exchanged during the reset period:
-If none of the sides has a message, master waits for 10ms then resets the communication.
-If both have a message, slave sends first and master reads.

-Immediately after the reset period, the reader side enters reading mode and the sender enters sending mode.
-Functions for reading and sending a message is the same for both roles.

One bit is transfered in 100us like this:
- To send a 1, sender pulls the line low for 15us, then releases it for 85us
- To send a 0, sender pulls the line low for 65us, then releases it for 35us
- Reader samples around 40us from the beginning and rests for 60us until next bit

Both sender and reader waits for 750us between each byte.

After reading 13 bytes (max message length, including header, length and checksum) (If the message is shorter remaining bytes are sent as 0xFF)
Sender waits for acknowledgement byte, that is 0x0F if the message is received successfully, or 0x00 if an error occurred.

-------------------------------------------------------------------------------------------------------------------------

Please feel free to ask me if you have any question