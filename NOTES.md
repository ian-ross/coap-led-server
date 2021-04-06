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
   flash:

```
cd ~/code/echo_server
west build -b nrf52840dk_nrf52840 . -- -DCONF_FILE="prj.conf overlay-ot.conf"
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