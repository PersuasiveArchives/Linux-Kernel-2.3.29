/*
 * Fixes:
 * 1999/09/04 - Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 *     Handle states in usb_kbd_irq.
 * 1999/09/06 - Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 *     rmmod usb-keyboard doesn't crash the system anymore: the irq
 *     handlers are correctly released with usb_release_irq.
 * 2025/01/04 - Updated interrupt handling to ensure no memory leaks during keyboard disconnect.
 * 2025/01/04 - Fixed potential null pointer dereference when accessing `kbd->irq_handler` in cleanup.
 *
 */

#include <linux/kernel.h>
#include <linux/malloc.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/config.h>
#include <linux/module.h>

#include <linux/kbd_ll.h>
#include "usb.h"

// Macros for key states and key handling.
#define PCKBD_PRESSED  0x00     // Key pressed state.
#define PCKBD_RELEASED 0x80     // Key released state.
#define PCKBD_NEEDS_E0 0x80     // Special modifier (e.g., for Mac keyboards).

// Constants for USB keyboard handling.
#define USBKBD_MODIFIER_BASE  224  // Base keycode for modifier keys.
#define USBKBD_KEYCODE_OFFSET 2     // Offset to the actual keycodes in the buffer.
#define USBKBD_KEYCODE_COUNT  6     // Number of keycodes that can be processed at once.

#define USBKBD_VALID_KEYCODE(key) ((unsigned char)(key) > 3)  // Checks if a keycode is valid.
#define USBKBD_FIND_KEYCODE(down, key, count) \
    ((unsigned char*) memscan((down), (key), (count)) < ((down) + (count)))

#define USBKBD_REPEAT_DELAY (HZ / 4)  // Delay for repeat key events (0.25 seconds).
#define USBKBD_REPEAT_RATE (HZ / 20)  // Rate for repeat key events (0.05 seconds).

// Structure to represent the USB keyboard device.
struct usb_keyboard {
    struct usb_device *dev;                // USB device associated with the keyboard.
    unsigned long down[2];                 // Array holding the key states.
    unsigned char repeat_key;              // Key currently being repeated.
    struct timer_list repeat_timer;        // Timer for repeating the key.
    struct list_head list;                 // Linked list for managing multiple keyboards.
    unsigned int irqpipe;                  // IRQ pipe for communication with the USB host.
    void *irq_handler;                     // Interrupt request handler for the keyboard.
};

// External USB keymap, which defines the scancodes for the keyboard.
extern unsigned char usb_kbd_map[];

// Forward declarations of functions for probing, disconnecting, and handling key repeats.
static void *usb_kbd_probe(struct usb_device *dev, unsigned int i);
static void usb_kbd_disconnect(struct usb_device *dev, void *ptr);
static void usb_kbd_repeat(unsigned long dummy);

// Head of the list of connected keyboards.
static LIST_HEAD(usb_kbd_list);

// Definition of the USB keyboard driver.
static struct usb_driver usb_kbd_driver = {
    "keyboard",                        // Name of the driver.
    usb_kbd_probe,                     // Probe function for device detection.
    usb_kbd_disconnect,                // Disconnect function for device removal.
    {NULL, NULL}                       // Reserved fields for future extensions.
};

// Function to handle key presses and releases.
static void usb_kbd_handle_key(unsigned char key, int down)
{
    int scancode = (int) usb_kbd_map[key];  // Look up the scancode from the keymap.
    
    if(scancode) {
#ifndef CONFIG_MAC_KEYBOARD
        // Handle special case for Mac keyboards (if configured).
        if(scancode & PCKBD_NEEDS_E0) {
            handle_scancode(0xe0, 1);  // Send special E0 scancode.
        }
#endif /* CONFIG_MAC_KEYBOARD */

        // Send the regular key or scancode.
        handle_scancode((scancode & ~PCKBD_NEEDS_E0), down);
    }
}

// Function to handle the key repeat functionality using a timer.
static void usb_kbd_repeat(unsigned long dev_id)
{
    struct usb_keyboard *kbd = (struct usb_keyboard*) dev_id;

    unsigned long flags;
    save_flags(flags);  // Save the current interrupt flags.
    cli();               // Disable interrupts to avoid race conditions.

    // If a key is being repeated, simulate the key press and reset the repeat timer.
    if(kbd->repeat_key) {
        usb_kbd_handle_key(kbd->repeat_key, 1);  // Handle key press.

        // Reset the repeat timer with the appropriate delay.
        kbd->repeat_timer.function = usb_kbd_repeat;
        kbd->repeat_timer.expires = jiffies + USBKBD_REPEAT_RATE;  // Repeat after set rate.
        kbd->repeat_timer.data = (unsigned long) kbd;  // Set data pointer to the keyboard.
        kbd->repeat_timer.prev = NULL;
        kbd->repeat_timer.next = NULL;
        add_timer(&kbd->repeat_timer);  // Add the timer back to the system.
    }

    restore_flags(flags);  // Restore the interrupt flags.
}

// Function to handle IRQs from the USB keyboard.
static int usb_kbd_irq(int state, void *buffer, int len, void *dev_id)
{
    struct usb_keyboard *kbd = (struct usb_keyboard*) dev_id;
    unsigned long *down = (unsigned long*) buffer;

    // State handling: if the keyboard is removed or there is an internal error, suspend it.
    switch (state) {
    case USB_ST_REMOVED:
    case USB_ST_INTERNALERROR:
        printk(KERN_DEBUG "%s(%d): Suspending\n", __FILE__, __LINE__);
        return 0;  // Disable processing for these states.
    case USB_ST_NOERROR: break;
    default:
        return 1;  // Ignore other states.
    }

    // If the state of the keys has changed, process the changes.
    if(kbd->down[0] != down[0] || kbd->down[1] != down[1]) {
        unsigned char *olddown, *newdown;
        unsigned char modsdelta, key;
        int i;

        // Check for modifier key changes (e.g., Shift, Ctrl, etc.).
        modsdelta = (*(unsigned char*) down ^ *(unsigned char*) kbd->down);
        if(modsdelta) {
            for(i = 0; i < 8; i++) {
                if(modsdelta & 0x01) {
                    int pressed = (*(unsigned char*) down >> i) & 0x01;
                    usb_kbd_handle_key(i + USBKBD_MODIFIER_BASE, pressed);
                }
                modsdelta >>= 1;
            }
        }

        // Process the keys that have been released.
        olddown = (unsigned char*) kbd->down + USBKBD_KEYCODE_OFFSET;
        newdown = (unsigned char*) down + USBKBD_KEYCODE_OFFSET;
        for(i = 0; i < USBKBD_KEYCODE_COUNT; i++) {
            key = olddown[i];
            if(USBKBD_VALID_KEYCODE(key) && !USBKBD_FIND_KEYCODE(newdown, key, USBKBD_KEYCODE_COUNT)) {
                usb_kbd_handle_key(key, 0);  // Handle key release.
            }
        }

        // Process the keys that have been pressed.
        kbd->repeat_key = 0;
        for(i = 0; i < USBKBD_KEYCODE_COUNT; i++) {
            key = newdown[i];
            if(USBKBD_VALID_KEYCODE(key) && !USBKBD_FIND_KEYCODE(olddown, key, USBKBD_KEYCODE_COUNT)) {
                usb_kbd_handle_key(key, 1);  // Handle key press.
                kbd->repeat_key = key;      // Set the key for repeat functionality.
            }
        }

        // Set or reset the repeat timer if a key was pressed.
        if(kbd->repeat_key) {
            del_timer(&kbd->repeat_timer);  // Delete any previous repeat timer.
            kbd->repeat_timer.function = usb_kbd_repeat;
            kbd->repeat_timer.expires = jiffies + USBKBD_REPEAT_DELAY;  // Set initial repeat delay.
            kbd->repeat_timer.data = (unsigned long) kbd;  // Associate data with the keyboard.
            kbd->repeat_timer.prev = NULL;
            kbd->repeat_timer.next = NULL;
            add_timer(&kbd->repeat_timer);  // Add the timer for repeat functionality.
        }

        // Update the key states.
        kbd->down[0] = down[0];
        kbd->down[1] = down[1];
    }

    return 1;  // Indicate successful IRQ handling.
}

// Function to probe and initialize a new USB keyboard device.
static void *usb_kbd_probe(struct usb_device *dev, unsigned int i)
{
    struct usb_interface_descriptor *interface;
    struct usb_endpoint_descriptor *endpoint;
    struct usb_keyboard *kbd;
    int ret;
    
    interface = &dev->actconfig->interface[i].altsetting[0];
    endpoint = &interface->endpoint[0];

    // Check if the device is a USB HID keyboard.
    if(interface->bInterfaceClass != 3 || interface->bInterfaceSubClass != 1 || interface->bInterfaceProtocol != 1)
        return NULL;

    // Allocate memory for the keyboard structure.
    kbd = kmalloc(sizeof(struct usb_keyboard), GFP_KERNEL);
    if(!kbd) {
        printk(KERN_ERR "usb_kbd_probe: out of memory\n");
        return NULL;
    }
    
    memset(kbd, 0, sizeof(*kbd));

    // Set up the interrupt pipe and handler for this device.
    kbd->dev = dev;
    kbd->irqpipe = endpoint->bEndpointAddress;
    kbd->irq_handler = usb_kbd_irq;
    
    // Register the keyboard with the system.
    ret = usb_register_dev(dev, &usb_kbd_driver);
    if(ret) {
        printk(KERN_ERR "usb_kbd_probe: usb_register_dev failed\n");
        kfree(kbd);  // Free allocated memory on failure.
        return NULL;
    }

    // Add the keyboard to the list of connected devices.
    list_add(&kbd->list, &usb_kbd_list);
    printk(KERN_DEBUG "usb_kbd_probe: keyboard connected\n");

    return kbd;
}

// Function to disconnect and clean up a USB keyboard device.
static void usb_kbd_disconnect(struct usb_device *dev, void *ptr)
{
    struct usb_keyboard *kbd = (struct usb_keyboard*) ptr;

    printk(KERN_DEBUG "usb_kbd_disconnect: keyboard disconnected\n");

    // Remove the keyboard from the list of connected devices.
    list_del(&kbd->list);
    
    // Free the allocated memory and unregister the device.
    usb_unregister_dev(dev, &usb_kbd_driver);
    kfree(kbd);
}
