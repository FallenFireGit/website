/*
 *--------------------------------------
 * Program Name: WIFI
 * Description:  Front end for the internal ESP32-C3 Wi-Fi bridge.
 *               The calculator is USB host; the C3 is a CDC serial device
 *               wired to the USB pads inside the case. See ../README.md.
 *--------------------------------------
 */

#include <srldrvce.h>
#include <usbdrvce.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ti/getcsc.h>
#include <ti/screen.h>

#define SCREEN_ROWS 10
#define SCREEN_COLS 26
#define CYCLES_PER_MS 48000u

typedef enum { RESP_OK, RESP_ERR, RESP_TIMEOUT, RESP_ABORT, RESP_LOST } resp_t;

static srl_device_t srl;
static bool has_srl_device = false;
static uint8_t srl_buf[512];

static uint8_t cur_row = 0;

/* ---------- USB plumbing (from the toolchain's srl_echo example) ---------- */

static usb_error_t handle_usb_event(usb_event_t event, void *event_data,
                                    usb_callback_data_t *callback_data) {
    usb_error_t err;
    if ((err = srl_UsbEventCallback(event, event_data, callback_data)) != USB_SUCCESS)
        return err;

    if (event == USB_DEVICE_CONNECTED_EVENT && !(usb_GetRole() & USB_ROLE_DEVICE)) {
        usb_ResetDevice(event_data);
    }

    if (event == USB_HOST_CONFIGURE_EVENT ||
        (event == USB_DEVICE_ENABLED_EVENT && !(usb_GetRole() & USB_ROLE_DEVICE))) {
        if (has_srl_device) return USB_SUCCESS;

        usb_device_t device;
        if (event == USB_HOST_CONFIGURE_EVENT) {
            device = usb_FindDevice(NULL, NULL, USB_SKIP_HUBS);
            if (device == NULL) return USB_SUCCESS;
        } else {
            device = event_data;
        }

        /* Baud rate is ignored by the C3's USB-Serial-JTAG but required here. */
        if (srl_Open(&srl, device, srl_buf, sizeof srl_buf, SRL_INTERFACE_ANY, 115200) == SRL_SUCCESS) {
            has_srl_device = true;
        }
    }

    if (event == USB_DEVICE_DISCONNECTED_EVENT && has_srl_device && event_data == srl.dev) {
        srl_Close(&srl);
        has_srl_device = false;
    }

    return USB_SUCCESS;
}

static bool elapsed(uint32_t start, uint32_t ms) {
    return usb_GetCycleCounter() - start >= ms * CYCLES_PER_MS;
}

/* ---------- Homescreen output with paging ---------- */

static void screen_clear(void) {
    os_ClrHome();
    cur_row = 0;
}

static void wait_key(void) {
    while (!os_GetCSC()) usb_HandleEvents();
}

static void put_row(const char *text, size_t len) {
    char chunk[SCREEN_COLS + 1];

    if (cur_row >= SCREEN_ROWS - 1) {
        os_SetCursorPos(SCREEN_ROWS - 1, 0);
        os_PutStrFull("-- more --");
        wait_key();
        screen_clear();
    }
    memcpy(chunk, text, len);
    chunk[len] = '\0';
    os_SetCursorPos(cur_row++, 0);
    os_PutStrFull(chunk);
}

static void print_line(const char *s) {
    size_t len = strlen(s);
    if (len == 0) {
        put_row("", 0);
        return;
    }
    while (len > 0) {
        size_t n = len > SCREEN_COLS ? SCREEN_COLS : len;
        put_row(s, n);
        s += n;
        len -= n;
    }
}

/* ---------- Protocol: send "CMD args\n", read lines until "OK..." or "ERR..." ---------- */

static void send_line(const char *a, const char *b) {
    srl_Write(&srl, a, strlen(a));
    if (b) srl_Write(&srl, b, strlen(b));
    srl_Write(&srl, "\n", 1);
}

static resp_t read_response(uint16_t timeout_ms, bool show) {
    static char line[256];
    size_t len = 0;
    uint32_t start = usb_GetCycleCounter();

    while (!elapsed(start, timeout_ms)) {
        usb_HandleEvents();
        if (!has_srl_device) return RESP_LOST;
        if (os_GetCSC() == sk_Clear) return RESP_ABORT;

        char c;
        int n;
        while ((n = srl_Read(&srl, &c, 1)) == 1) {
            if (c == '\r') continue;
            if (c != '\n') {
                if (len < sizeof line - 1) line[len++] = c;
                continue;
            }
            line[len] = '\0';
            len = 0;
            start = usb_GetCycleCounter();  /* data is flowing; restart timeout */

            bool is_ok = strncmp(line, "OK", 2) == 0;
            bool is_err = strncmp(line, "ERR", 3) == 0;
            if (show) print_line(line[0] == ' ' ? line + 1 : line);
            if (is_ok) return RESP_OK;
            if (is_err) return RESP_ERR;
        }
        if (n < 0) return RESP_LOST;
    }
    return RESP_TIMEOUT;
}

static resp_t command(const char *cmd, const char *arg, uint16_t timeout_ms) {
    send_line(cmd, arg);
    resp_t r = read_response(timeout_ms, true);
    if (r == RESP_TIMEOUT) print_line("Timed out.");
    if (r == RESP_ABORT) print_line("Aborted.");
    if (r == RESP_LOST) print_line("ESP32 disconnected.");
    return r;
}

/* ---------- UI ---------- */

static void input(const char *prompt, char *buf, size_t size) {
    os_SetCursorPos(cur_row, 0);
    os_GetStringInput(prompt, buf, size);
    cur_row++;
}

static void do_join(void) {
    static char ssid[33];
    static char pass[65];
    static char arg[100];

    print_line("Lowercase: [alpha][alpha]");
    input("SSID:", ssid, sizeof ssid);
    input("Pass:", pass, sizeof pass);
    strcpy(arg, ssid);
    strcat(arg, "\t");
    strcat(arg, pass);
    print_line("Connecting...");
    command("JOIN ", arg, 20000);
}

static void do_fetch(void) {
    static char url[200];
    static char full[210];

    input("URL:", url, sizeof url);
    if (!url[0]) return;
    if (strstr(url, "://")) {
        strcpy(full, url);
    } else {
        strcpy(full, "http://");
        strcat(full, url);
    }
    print_line("Fetching...");
    command("GET ", full, 30000);
}

static void show_menu(void) {
    screen_clear();
    print_line("TI-84 CE Wi-Fi");
    print_line("1:Status");
    print_line("2:Scan networks");
    print_line("3:Join network");
    print_line("4:Fetch URL");
    print_line("5:Time");
    print_line("6:Setup via phone");
    print_line("7:Forget network");
    print_line("[clear]:Quit");
}

static bool connect_bridge(void) {
    uint32_t start = usb_GetCycleCounter();

    print_line("Looking for ESP32...");
    while (!has_srl_device) {
        usb_HandleEvents();
        if (elapsed(start, 10000) || os_GetCSC() == sk_Clear) return false;
    }
    /* Give the C3 a moment after enumeration, then handshake. */
    start = usb_GetCycleCounter();
    while (!elapsed(start, 200)) usb_HandleEvents();
    send_line("HELLO", NULL);
    return read_response(3000, true) == RESP_OK;
}

int main(void) {
    screen_clear();

    usb_error_t usb_error = usb_Init(handle_usb_event, NULL, srl_GetCDCStandardDescriptors(),
                                     USB_DEFAULT_INIT_FLAGS);
    if (usb_error != USB_SUCCESS) {
        usb_Cleanup();
        print_line("USB init failed.");
        wait_key();
        return 1;
    }

    if (!connect_bridge()) {
        print_line("No bridge found.");
        print_line("Check wiring/power,");
        print_line("unplug USB cable.");
        wait_key();
        usb_Cleanup();
        return 1;
    }

    bool running = true;
    show_menu();
    while (running) {
        uint8_t key = os_GetCSC();
        usb_HandleEvents();
        if (!key) continue;

        if (key == sk_Clear) break;

        screen_clear();
        switch (key) {
            case sk_1: command("STATUS", NULL, 3000); break;
            case sk_2: print_line("Scanning..."); command("SCAN", NULL, 15000); break;
            case sk_3: do_join(); break;
            case sk_4: do_fetch(); break;
            case sk_5: command("TIME", NULL, 15000); break;
            case sk_6:
                command("SETUP", NULL, 3000);
                print_line("On your phone, join");
                print_line("Wi-Fi \"TI84-Setup\"");
                print_line("and pick a network.");
                break;
            case sk_7: command("FORGET", NULL, 3000); break;
            default: show_menu(); continue;
        }
        if (!has_srl_device) running = false;
        put_row("[any key]", 9);
        wait_key();
        show_menu();
    }

    if (has_srl_device) {
        send_line("BYE", NULL);
        read_response(500, false);
    }
    usb_Cleanup();
    os_ClrHome();
    return 0;
}
