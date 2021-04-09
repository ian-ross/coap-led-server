# Basic Zephyr build setup

1. Install Zephyr and selected Zephyr SDK to some central location (I
   put the main Zephyr distribution in `/opt/zephyr` and the SDK in
   `/opt/zephyr-sdk-0.11.2`).

2. Set environment variables to point to the Zephyr installation
   directories. For example:

```
ZEPHYR_BASE=/opt/zephyr/zephyr
ZEPHYR_TOOLCHAIN_VARIANT=zephyr
ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-0.11.2
```

3. Copy one of the samples from the Zephyr tree to some other
   location. For example:

```
cp -r /opt/zephyr/zephyr/samples/basic/blinky_pwm ~/code
```

4. Then run `west` in the copied project directory to build and flash
   the example:

```
cd ~/code/blinky_pwm
west build -b nrf52840dk_nrf52840 .
west flash
```

# Zephyr OpenThread echo server/client example

1. Copy the `net/sockets/echo_server` and `net/sockets/echo_client`
   examples to a working directory:

```
cp -r /opt/zephyr/zephyr/samples/net/sockets/echo_server ~/code
cp -r /opt/zephyr/zephyr/samples/net/sockets/echo_client ~/code
```

2. To use it on the nRF52840 dongle, the `echo_client` example has to
   be updated a bit compared to the code in the Zephyr tree so that it
   will log output using a USB CDC ACM virtual serial port. (The echo
   server uses the SEGGER J-Link connection for its logging so doesn't
   need any updates since we're running it on the development kit.)

   The required changes are adding the following lines to the bottom
   of the `overlay-ot.conf` OpenThread overlay:

```
CONFIG_SHELL=y
CONFIG_OPENTHREAD_SHELL=y

CONFIG_USB=y
CONFIG_USB_DEVICE_STACK=y
CONFIG_USB_CDC_ACM=y
CONFIG_USB_DEVICE_MANUFACTURER="Nordic Semiconductor"
CONFIG_USB_DEVICE_PRODUCT="Zephyr OpenThread device"

CONFIG_UART_SHELL_ON_DEV_NAME="CDC_ACM_0"
```

   and adding a couple of lines to the `src/echo-client.c` file.
   Somewhere near the top of the file, add:

```
#include <usb/usb_device.h>
```

   and in the `init_app` function, just after the first log message,
   add:

```
usb_enable(NULL);
```

3. Build the server example for OpenThread on the nRF52840 DK and
   flash (I've moved all the OT overlay stuff into the main `prj.conf`
   file):

```
cd ~/code/echo_server
west build -b nrf52840dk_nrf52840 .
west flash
```

4. Connect to the development kit with a terminal emulator (I use
   `tio`). On Arch Linux, the first device connected appears as
   `/dev/ttyACM0`. This will give you a Zephyr shell on the
   development kit, where you'll also be able to see the log messages
   from the OpenThread echo server. You can use OpenThread CLI
   commands via the Zephyr shell by preceding them with "`ot`".

5. Build the client example for OpenThread on the nRF52840 dongle and
   flash (press the RESET button on the dongle to enable the
   bootloader):

```
cd ~/code/echo_client
west build -b nrf52840dongle_nrf52840 . -- -DCONF_FILE="prj.conf overlay-ot.conf"
nrfutil pkg generate --hw-version 52 --sd-req=0x00 \
        --application build/zephyr/zephyr.hex \
        --application-version 1 echo_client.zip
nrfutil dfu usb-serial -pkg echo_client.zip -p /dev/ttyACM1
```

6. After flashing, remove the dongle, plug it back in and connect to
   the USB serial port with a terminal program (on Arch Linux, this
   one will probably be `/dev/ttyACM1`).

7. You'll see the client sending messages and the server receiving and
   echoing them.


# Zephyr OpenThread echo server + OpenThread CLI example

1. Flash the OT CLI image to an nRF52840 dongle. I used the nRF
   Connect desktop application for this, using the hex file at
   `.../examples/thread/cli/ftd/usb/hex/nrf52840_xxaa_mbr_pca10059.hex`
   in the nRF5 SDK distribution.

2. Connect to both the development kit (running the echo server) and
   the dongle (running the OT CLI) with a terminal emulator (I use
   `tio`). On Arch Linux, the first device connected appears as
   `/dev/ttyACM0` and the second as `/dev/ttyACM1`.

3. On the development kit, find the OT network setup information:

```
ot channel
ot networkname
ot panid
ot masterkey
```

4. Set up the same information on the dongle and bring up the network
   stack:

```
channel 26
networkname ot_zephyr
panid 0xabcd
masterkey 00112233445566778899aabbccddeeff
ifconfig up
thread start
```

5. After a short time, you should be able to ping between the two
   devices. Use `rloc16` to identify the mesh local 16-bit address of
   a device, `ipaddr` to find the corresponding IP address, and `ping`
   to send a single ICMPv6 packet. Or just do `ping ff03::1` to do a
   mesh-local multicast ping to all devices on the network.

6. Now you can send UDP messages from the CLI. The echo server uses
   port 4242, so do the following in the CLI on the dongle:

```
udp open
udp connect <ip-address> 4242
udp send hello-there
```

  You should see messages on both the server and client recording the
  message being received and echoed.


# Zephyr OpenThread CoAP server + OpenThread CLI

This is a lot more complicated than the earlier examples! The CoAP
server example in the main Zephyr tree is for Ethernet, not
OpenThread, and the CoAP server example in the Nordic nRF-Connect SDK
uses Nordic's own CoAP API rather than the Zephyr API. So some
bricolage is required.

I'm doing this by looking at the Zephyr CoAP server example, the
Zephyr OpenThread socket echo server example, and the [CoAP
RFC](https://tools.ietf.org/html/rfc7252).

 - I started from the Zephyr `coap_server` example.

 - I removed all the CoAP resources except for one, and set this up to
   set and return a single boolean value under the `/led` path. (This
   was mostly mechanical, but getting the response codes right for the
   `POST` method needed a bit of looking in the CoAP protocol
   definition.)

 - I copied the initialisation scheme from the echo server: this uses
   the Zephyr network connection management API to detect when the
   OpenThread network becomes available, and only starts the
   application-level code at that point. It uses a couple of
   semaphores to control this (and an application exit command added
   into the command shell).

 - I've now separated out the code into individual modules that
   hopefully make it easier to understand, and I've written fairly
   extensive comments within the code to explain what's going on.

**Next steps:**

 - Wire LED state up to real LED.
 - Get running on dongle as well as dev kit.


# References

 - [RFC 7252: The Constrained Application Protocol (CoAP)](https://tools.ietf.org/html/rfc7252)
 - [RFC 6690: Constrained RESTful Environments (CoRE) Link Format](https://tools.ietf.org/html/rfc6690)
 - [RFC 7959: Block-Wise Transfers in the Constrained Application Protocol (CoAP)](https://tools.ietf.org/html/rfc7959)
 - [RFC 7641: Observing Resources in the Constrained Application Protocol (CoAP)](https://tools.ietf.org/html/rfc7641)


# Debugging

Looking at messages sent by the OT CLI...

(CoAP payload marker is 0xFF.)

## CoAP message for `GET led`

```
52 01 52 92   55 7f   b3 6c 65 64
                          l  e  d
```

Header:

 - 52 = [01][01][0010]
    * CoAP Version = 1
    * Type = 1 (non-confirmable)
    * Token length = 2
 - 01 = Code 0.01 (GET)
 - 5292 = sequence number (21138)

Token: 557F (for request/response correlation)

Options:
 - B3 = Option 11 (Uri-Path), length 3 => "led"



## CoAP message for `POST led on`

```
42 02 5b 6a   38 8b   b3 6c 65 64   ff 6f 6e
                          l  e  d       o  n
```

Header:

 - 42 = [01][00][0010]
    * CoAP Version = 1
    * Type = 1 (confirmable)
    * Token length = 2
 - 02 = Code 0.02 (POST)
 - 5B6A = sequence number

Token: 388B (for request/response correlation)

Options:
 - B3 = Option 11 (Uri-Path), length 3 => "led"

Payload: "on"
