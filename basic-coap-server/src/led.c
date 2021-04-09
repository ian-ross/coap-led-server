// Basic OpenThread CoAP server: LED control.

#include <logging/log.h>
LOG_MODULE_DECLARE(basic_coap_server, LOG_LEVEL_DBG);

#include <zephyr.h>
#include <device.h>
#include <devicetree.h>
#include <drivers/gpio.h>

#include "led.h"

// The devicetree node identifier for the "led0" alias.
#define LED0_NODE DT_ALIAS(led0)

#if DT_NODE_HAS_STATUS(LED0_NODE, okay)
#define LED0  DT_GPIO_LABEL(LED0_NODE, gpios)
#define PIN DT_GPIO_PIN(LED0_NODE, gpios)
#define FLAGS DT_GPIO_FLAGS(LED0_NODE, gpios)
#else
// A build error here means your board isn't set up to blink an LED.
#error "Unsupported board: led0 devicetree alias is not defined"
#define LED0  ""
#define PIN 0
#define FLAGS 0
#endif


const static struct device *dev;

bool init_led(void) {
  dev = device_get_binding(LED0);
  if (dev == NULL) return false;

  int ret = gpio_pin_configure(dev, PIN, GPIO_OUTPUT_ACTIVE | FLAGS);
  if (ret < 0) return false;

  led_off();
  return true;
}

void led_on(void) {
  LOG_DBG("===> LED ON");
  gpio_pin_set(dev, PIN, true);
}

void led_off(void) {
  LOG_DBG("===> LED OFF");
  gpio_pin_set(dev, PIN, false);
}
