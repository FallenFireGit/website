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
#include <ti/flags.h>
#include <ti/getcsc.h>
#include <ti/screen.h>

#define SCREEN_ROWS 10
#define SCREEN_COLS 26
#define CYCLES_PER_MS 48000u

typedef enum { RESP_OK, RESP_ERR, RESP_TIMEOUT, RESP_ABORT, RESP_LOST } resp_t;

static srl_device_t srl;
static bool has_srl_device = false;  /* a bridge link (srldrvce or CH340) is open */
static bool link_is_ch340 = false;
static uint8_t srl_buf[512];

static uint8_t cur_row = 0;

/* ---------- CH340 / CH341 USB-serial (classic ESP32 dev boards) ----------
 * srldrvce supports CDC, FTDI and PL2303 only. This is the minimal CH34x
 * sequence from the Linux ch341 driver: init, 115200 8N1, DTR/RTS off. */

#define CH34X_VID     0x1A86
#define CH34X_REQ_INIT    0xA1
#define CH34X_REQ_WRITE   0x9A
#define CH34X_REQ_MODEM   0xA4
#define CH34X_RING_SIZE   1024

static usb_device_t ch_dev;
static usb_endpoint_t ch_in, ch_out;
static uint8_t ch_rx_xfer[32];
static uint8_t ch_ring[CH34X_RING_SIZE];
static volatile uint16_t ch_head, ch_tail;

static usb_error_t ch_rx_done(usb_endpoint_t endpoint, usb_transfer_status_t status,
                              size_t transferred, usb_transfer_data_t *data) {
    (void)data;
    if (status & (USB_TRANSFER_CANCELLED | USB_TRANSFER_NO_DEVICE)) return USB_SUCCESS;
    for (size_t i = 0; i < transferred; i++) {
        uint16_t next = (ch_head + 1) % CH34X_RING_SIZE;
        if (next == ch_tail) break;  /* full: drop; replies are capped at 2 KB and drained fast */
        ch_ring[ch_head] = ch_rx_xfer[i];
        ch_head = next;
    }
    usb_ScheduleBulkTransfer(endpoint, ch_rx_xfer, sizeof ch_rx_xfer, ch_rx_done, NULL);
    return USB_SUCCESS;
}

static bool ch_ctrl(uint8_t request, uint16_t value, uint16_t index) {
    usb_control_setup_t setup = {0x40 /* vendor, host->device */, request, value, index, 0};
    return usb_DefaultControlTransfer(ch_dev, &setup, NULL, 3, NULL) == USB_SUCCESS;
}

static bool ch340_open(usb_device_t device) {
    static uint8_t config[64];
    usb_device_descriptor_t desc;
    size_t n;

    if (usb_GetDeviceDescriptor(device, &desc, sizeof desc, &n) != USB_SUCCESS) return false;
    if (desc.idVendor != CH34X_VID || (desc.idProduct != 0x7523 && desc.idProduct != 0x5523))
        return false;
    size_t len = usb_GetConfigurationDescriptorTotalLength(device, 0);
    if (len == 0 || len > sizeof config) return false;
    if (usb_GetConfigurationDescriptor(device, 0, (usb_configuration_descriptor_t *)config, len, &n) != USB_SUCCESS)
        return false;
    if (usb_SetConfiguration(device, (usb_configuration_descriptor_t *)config, len) != USB_SUCCESS)
        return false;

    ch_dev = device;
    if (!ch_ctrl(CH34X_REQ_INIT, 0, 0)) return false;
    /* 115200 baud; bit 7 = deliver short packets instead of waiting for 32 bytes. */
    if (!ch_ctrl(CH34X_REQ_WRITE, 0x1312, 0xCC83)) return false;
    if (!ch_ctrl(CH34X_REQ_WRITE, 0x2518, 0x00C3)) return false;  /* 8N1, RX and TX on */
    /* DTR and RTS both off (inverted register): a dev board's auto-reset stays idle. */
    if (!ch_ctrl(CH34X_REQ_MODEM, 0xFFFF, 0)) return false;

    ch_out = usb_GetDeviceEndpoint(device, 0x02);
    ch_in = usb_GetDeviceEndpoint(device, 0x82);
    if (!ch_out || !ch_in) return false;
    ch_head = ch_tail = 0;
    return usb_ScheduleBulkTransfer(ch_in, ch_rx_xfer, sizeof ch_rx_xfer, ch_rx_done, NULL) == USB_SUCCESS;
}

/* ---------- Link I/O: same calls whichever driver is open ---------- */

static void link_write(const void *data, size_t len) {
    if (link_is_ch340) usb_BulkTransfer(ch_out, (void *)data, len, 3, NULL);
    else srl_Write(&srl, data, len);
}

/* Returns 1 with a byte, 0 if none waiting, < 0 if the link failed. */
static int link_read_byte(char *c) {
    if (!link_is_ch340) return srl_Read(&srl, c, 1);
    if (ch_tail == ch_head) return 0;
    *c = (char)ch_ring[ch_tail];
    ch_tail = (ch_tail + 1) % CH34X_RING_SIZE;
    return 1;
}

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
            link_is_ch340 = false;
        } else if (ch340_open(device)) {
            has_srl_device = true;
            link_is_ch340 = true;
        }
    }

    if (event == USB_DEVICE_DISCONNECTED_EVENT && has_srl_device) {
        if (link_is_ch340 && event_data == ch_dev) {
            ch_dev = NULL;
            has_srl_device = false;
        } else if (!link_is_ch340 && event_data == srl.dev) {
            srl_Close(&srl);
            has_srl_device = false;
        }
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

/* ---------- Reply buffer, shown afterwards by view_lines() ---------- */

#define MAX_LINES 100  /* the C3 caps replies at 2 KB, about 80 screen rows */

static char lines[MAX_LINES][SCREEN_COLS + 1];
static uint8_t n_lines = 0;

static void add_line(const char *s) {
    size_t len = strlen(s);
    do {
        if (n_lines == MAX_LINES) {
            strcpy(lines[MAX_LINES - 1], "...(cut)");
            return;
        }
        size_t n = len > SCREEN_COLS ? SCREEN_COLS : len;
        memcpy(lines[n_lines], s, n);
        lines[n_lines++][n] = '\0';
        s += n;
        len -= n;
    } while (len > 0);
}

/* ---------- Protocol: send "CMD args\n", read lines until "OK..." or "ERR..." ---------- */

static void send_line(const char *a, const char *b) {
    link_write(a, strlen(a));
    if (b) link_write(b, strlen(b));
    link_write("\n", 1);
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
        while ((n = link_read_byte(&c)) == 1) {
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
            if (show) {
                if (is_ok) {
                    if (line[2]) add_line(line + 3);  /* "OK 5 found" -> "5 found"; bare OK hidden */
                } else if (is_err) {
                    add_line("Error:");
                    add_line(line[3] ? line + 4 : "unknown");
                } else {
                    add_line(line[0] == ' ' ? line + 1 : line);
                }
            }
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
    if (r == RESP_TIMEOUT) add_line("Timed out.");
    if (r == RESP_ABORT) add_line("Aborted.");
    if (r == RESP_LOST) add_line("ESP32 disconnected.");
    return r;
}

/* Scrollable view of the collected reply: [up]/[down] scroll, [clear]/[enter] back. */
static void view_lines(void) {
    const uint8_t page = SCREEN_ROWS - 1;  /* last row is the help line */
    uint8_t top = 0;

    for (;;) {
        os_ClrHome();
        for (uint8_t r = 0; r < page && top + r < n_lines; r++) {
            os_SetCursorPos(r, 0);
            os_PutStrFull(lines[top + r]);
        }
        bool more_up = top > 0;
        bool more_down = top + page < n_lines;
        os_SetCursorPos(SCREEN_ROWS - 1, 0);
        /* <= 25 chars: writing the last cell of the last row scrolls the screen. */
        os_PutStrFull(more_up && more_down ? "[up][down]more [clear]ok"
                      : more_down          ? "[down]more     [clear]ok"
                      : more_up            ? "[up]back       [clear]ok"
                                           : "               [clear]ok");

        uint8_t key;
        while (!(key = os_GetCSC())) usb_HandleEvents();
        if (key == sk_Clear || key == sk_Enter) return;
        if (key == sk_Down && more_down) top++;
        if (key == sk_Up && more_up) top--;
        if (key == sk_Right && more_down) top = top + page < n_lines - page ? top + page : n_lines - page;
        if (key == sk_Left && more_up) top = top > page ? top - page : 0;
    }
}

/* ---------- UI ---------- */

/* ---------- Line editor (os_GetStringInput can't move the cursor or delete) ---------- */

typedef enum { MODE_LOWER, MODE_UPPER, MODE_NUM, MODE_SYM, MODE_COUNT } edit_mode_t;
static const char *const mode_names[MODE_COUNT] = {"abc", "ABC", "123", "SYM"};

#define EDIT_FIRST_ROW 1
#define EDIT_ROWS      8  /* rows 1-8 hold text; row 9 is the help line */

/* Letters as printed in green above the keys; [alpha][0] is space as in the OS. */
static char key_letter(uint8_t key) {
    switch (key) {
        case sk_Math: return 'A';   case sk_Apps: return 'B';   case sk_Prgm: return 'C';
        case sk_Recip: return 'D';  case sk_Sin: return 'E';    case sk_Cos: return 'F';
        case sk_Tan: return 'G';    case sk_Power: return 'H';  case sk_Square: return 'I';
        case sk_Comma: return 'J';  case sk_LParen: return 'K'; case sk_RParen: return 'L';
        case sk_Div: return 'M';    case sk_Log: return 'N';    case sk_7: return 'O';
        case sk_8: return 'P';      case sk_9: return 'Q';      case sk_Mul: return 'R';
        case sk_Ln: return 'S';     case sk_4: return 'T';      case sk_5: return 'U';
        case sk_6: return 'V';      case sk_Sub: return 'W';    case sk_Store: return 'X';
        case sk_1: return 'Y';      case sk_2: return 'Z';      case sk_0: return ' ';
        case sk_DecPnt: return ':'; case sk_Chs: return '?';    case sk_Add: return '"';
        default: return 0;
    }
}

static const char *key_num(uint8_t key) {
    switch (key) {
        case sk_0: return "0"; case sk_1: return "1"; case sk_2: return "2"; case sk_3: return "3";
        case sk_4: return "4"; case sk_5: return "5"; case sk_6: return "6"; case sk_7: return "7";
        case sk_8: return "8"; case sk_9: return "9";
        case sk_DecPnt: return "."; case sk_Chs: return "-";  case sk_Comma: return ",";
        case sk_Add: return "+";    case sk_Sub: return "-";  case sk_Mul: return "*";
        case sk_Div: return "/";    case sk_Power: return "^"; case sk_LParen: return "(";
        case sk_RParen: return ")"; case sk_Square: return "^2"; case sk_Recip: return "^-1";
        case sk_Sin: return "sin(";  case sk_Cos: return "cos(";  case sk_Tan: return "tan(";
        case sk_Log: return "log(";  case sk_Ln: return "ln(";    case sk_Math: return "sqrt(";
        case sk_Store: return "=";
        default: return NULL;
    }
}

/* Shifted number row like a PC keyboard, for passwords and URLs. */
static char key_sym(uint8_t key) {
    switch (key) {
        case sk_1: return '!'; case sk_2: return '@'; case sk_3: return '#'; case sk_4: return '$';
        case sk_5: return '%'; case sk_6: return '^'; case sk_7: return '&'; case sk_8: return '*';
        case sk_9: return '('; case sk_0: return ')';
        case sk_DecPnt: return '.'; case sk_Comma: return ';'; case sk_Chs: return '_';
        case sk_Add: return '=';    case sk_Sub: return '-';   case sk_Mul: return '*';
        case sk_Div: return '/';    case sk_LParen: return '['; case sk_RParen: return ']';
        case sk_Store: return '>';  case sk_Math: return '\''; case sk_Power: return '~';
        default: return 0;
    }
}

static void edit_draw(const char *title, edit_mode_t mode, const char *buf, size_t len, size_t cur) {
    char row[SCREEN_COLS + 1];

    os_ClrHome();
    os_SetCursorPos(0, 0);
    os_PutStrFull(title);
    os_SetCursorPos(0, SCREEN_COLS - 5);
    os_PutStrFull("[");
    os_PutStrFull(mode_names[mode]);
    os_PutStrFull("]");

    for (uint8_t r = 0; r < EDIT_ROWS && (size_t)r * SCREEN_COLS < len; r++) {
        size_t start = (size_t)r * SCREEN_COLS;
        size_t n = len - start > SCREEN_COLS ? SCREEN_COLS : len - start;
        memcpy(row, buf + start, n);
        row[n] = '\0';
        os_SetCursorPos(EDIT_FIRST_ROW + r, 0);
        os_PutStrFull(row);
    }

    /* Cursor: the character under it, drawn inverted. */
    char under[2] = {cur < len ? buf[cur] : ' ', '\0'};
    os_SetCursorPos(EDIT_FIRST_ROW + cur / SCREEN_COLS, cur % SCREEN_COLS);
    /* Homescreen text ignores the draw colors; the OS inverse-text flag works. */
    os_SetFlag(TEXT, INVERSE);
    os_PutStrFull(under);
    os_ResetFlag(TEXT, INVERSE);

    os_SetCursorPos(SCREEN_ROWS - 1, 0);
    os_PutStrFull("[alpha]mode [del]bksp");
}

/* Full-screen editor. Returns false if cancelled ([clear] on an empty line);
 * [enter] on an empty line returns true with buf = "". */
static bool edit_line(const char *title, char *buf, size_t size) {
    size_t max = size - 1;
    if (max > EDIT_ROWS * SCREEN_COLS - 1) max = EDIT_ROWS * SCREEN_COLS - 1;
    size_t len = 0, cur = 0;
    edit_mode_t mode = MODE_LOWER;
    bool ok = false;

    buf[0] = '\0';
    edit_draw(title, mode, buf, len, cur);
    for (;;) {
        uint8_t key = os_GetCSC();
        usb_HandleEvents();
        if (!key) continue;

        char one[2] = {0, 0};
        const char *ins = NULL;

        if (key == sk_Enter) { ok = true; break; }
        if (key == sk_Clear) {
            if (len == 0) break;
            len = cur = 0;
        } else if (key == sk_Alpha) {
            mode = (mode + 1) % MODE_COUNT;
        } else if (key == sk_Left) {
            if (cur > 0) cur--;
        } else if (key == sk_Right) {
            if (cur < len) cur++;
        } else if (key == sk_Up) {
            cur = cur >= SCREEN_COLS ? cur - SCREEN_COLS : 0;
        } else if (key == sk_Down) {
            cur = cur + SCREEN_COLS <= len ? cur + SCREEN_COLS : len;
        } else if (key == sk_Del) {
            if (cur > 0) {
                memmove(buf + cur - 1, buf + cur, len - cur);
                cur--;
                len--;
            }
        } else if (mode == MODE_NUM) {
            ins = key_num(key);
        } else if (mode == MODE_SYM) {
            one[0] = key_sym(key);
            if (one[0]) ins = one;
        } else {
            one[0] = key_letter(key);
            if (mode == MODE_LOWER && one[0] >= 'A' && one[0] <= 'Z') one[0] += 'a' - 'A';
            if (one[0]) ins = one;
        }

        if (ins) {
            size_t n = strlen(ins);
            if (len + n <= max) {
                memmove(buf + cur + n, buf + cur, len - cur);
                memcpy(buf + cur, ins, n);
                cur += n;
                len += n;
            }
        }
        buf[len] = '\0';
        edit_draw(title, mode, buf, len, cur);
    }

    buf[len] = '\0';
    screen_clear();
    return ok;
}

static void do_join(void) {
    static char ssid[33];
    static char pass[65];
    static char arg[100];

    if (!edit_line("Network name:", ssid, sizeof ssid) || !ssid[0]) return;
    if (!edit_line("Password:", pass, sizeof pass)) return;  /* empty = open network */
    strcpy(arg, ssid);
    strcat(arg, "\t");
    strcat(arg, pass);
    print_line("Connecting...");
    command("JOIN ", arg, 20000);
}

static void do_fetch(void) {
    static char url[200];
    static char full[210];

    if (!edit_line("URL:", url, sizeof url) || !url[0]) return;
    if (strstr(url, "://")) {
        strcpy(full, url);
    } else {
        strcpy(full, "http://");
        strcat(full, url);
    }
    print_line("Fetching...");
    command("GET ", full, 30000);
}

static void do_ask(void) {
    static char question[200];

    if (!edit_line("Ask Gemini:", question, sizeof question) || !question[0]) return;
    print_line("Thinking...");
    command("ASK ", question, 60000);
}

/* Photo on the Pi camera -> Gemini. An empty prompt uses the Pi's default. */
static void do_camera(void) {
    static char prompt[200];

    if (!edit_line("Camera prompt:", prompt, sizeof prompt)) return;
    print_line("Taking photo...");
    command("SNAP ", prompt, 65000);
}

/* Settings stored on the C3, so they can be changed after it's sealed inside. */
static void do_settings(void) {
    static char value[200];

    print_line("Settings");
    print_line("1:Gemini API key");
    print_line("2:Gemini model");
    print_line("3:Pi address");
    print_line("");
    print_line("Empty model/address =");
    print_line("back to default.");
    print_line("[clear]:Back");

    uint8_t key;
    while (!(key = os_GetCSC())) usb_HandleEvents();
    screen_clear();
    switch (key) {
        case sk_1:
            if (edit_line("New Gemini key:", value, sizeof value) && value[0]) command("KEY ", value, 3000);
            break;
        case sk_2:
            if (edit_line("Gemini model:", value, sizeof value)) command("MODEL ", value, 3000);
            break;
        case sk_3:
            if (edit_line("Pi host:port:", value, sizeof value)) command("PI ", value, 3000);
            break;
        default:
            break;
    }
}

static void show_menu(void) {
    screen_clear();
    /* 9 rows max: the 10th row triggers "-- more --" paging. */
    print_line("TI-84 CE Wi-Fi [clear]Quit");
    print_line("1:Status     2:Scan");
    print_line("3:Join       4:Fetch URL");
    print_line("5:Time       6:Phone setup");
    print_line("7:Forget     8:Ask Gemini");
    print_line("9:Camera     0:Settings");
}

static bool connect_bridge(void) {
    uint32_t start = usb_GetCycleCounter();

    print_line("Looking for ESP32...");
    /* A C3 powered by this port boots when usb_Init turns VBUS on: it enumerates
     * briefly, detaches as its firmware starts (RELEASED), then re-attaches about
     * a second later (ARMED). Keep handshaking until one attach answers HELLO. */
    while (!elapsed(start, 12000)) {
        while (!has_srl_device) {
            usb_HandleEvents();
            if (elapsed(start, 12000) || os_GetCSC() == sk_Clear) return false;
        }
        uint32_t settle = usb_GetCycleCounter();
        while (!elapsed(settle, 200)) usb_HandleEvents();
        if (!has_srl_device) continue;
        send_line("HELLO", NULL);
        resp_t r = read_response(3000, false);
        if (r == RESP_OK) return true;
        if (r == RESP_ABORT) return false;
    }
    return false;
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
        n_lines = 0;
        switch (key) {
            case sk_1: command("STATUS", NULL, 3000); break;
            case sk_2: print_line("Scanning..."); command("SCAN", NULL, 15000); break;
            case sk_3: do_join(); break;
            case sk_4: do_fetch(); break;
            case sk_5: command("TIME", NULL, 15000); break;
            case sk_6:
                command("SETUP", NULL, 3000);
                add_line("On your phone, join");
                add_line("Wi-Fi \"TI84-Setup\"");
                add_line("and pick a network.");
                break;
            case sk_7: command("FORGET", NULL, 3000); break;
            case sk_8: do_ask(); break;
            case sk_9: do_camera(); break;
            case sk_0: do_settings(); break;
            default: show_menu(); continue;
        }
        if (!has_srl_device) running = false;
        if (n_lines) view_lines();  /* nothing to show if the user cancelled an editor */
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
