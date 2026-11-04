/*
 * PROJECT:     efi-emulation
 * LICENSE:     MIT OR MPL-2.0 (https://spdx.org/licenses/MIT, https://spdx.org/licenses/MPL-2.0)
 * PURPOSE:     Console input and timer events
 * COPYRIGHT:   Copyright 2026 Jonathan McAvin <mcavinjohn@gmail.com>
 */

#include "efi_emulation.h"
#include <efilib.h>

/* Simple text input and the few event services a loader polls with: a key
 * event for ConIn->WaitForKey and plain timer events. Keys come from COM1 and,
 * on x86, the PS/2 controller, so a menu works headless and in a window. */

#define EFIEMU_MAX_EVENTS 16
#define EFIEMU_EVENT_KEY 0x4b455900u

typedef struct {
  UINT32 type;
  BOOLEAN used;
  BOOLEAN signaled;
  BOOLEAN periodic;
  UINT64 deadline;
  UINT64 period;
} EfiEmuEvent;

static EfiEmuEvent events[EFIEMU_MAX_EVENTS];
static EFI_BOOT_SERVICES *bs;
static SIMPLE_INPUT_INTERFACE console_in;
/* virtual clock in 100 ns units, advanced by the waits below */
static UINT64 now_100ns;

static EFI_INPUT_KEY pending[16];
static UINTN pending_head, pending_tail;

static BOOLEAN queue_empty(void) { return pending_head == pending_tail; }

static void queue_key(UINT16 scan, CHAR16 ch) {
  UINTN next = (pending_tail + 1) % 16;
  if (next == pending_head)
    return;
  pending[pending_tail].ScanCode = scan;
  pending[pending_tail].UnicodeChar = ch;
  pending_tail = next;
}

#if defined(__x86_64__) || defined(__i386__)
static UINT8 inb(UINT16 port) {
  UINT8 v;
  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}

#define COM1 0x3f8

/* ESC sequences from a terminal, kept until a whole one arrived */
static UINT8 esc_buf[8];
static UINTN esc_len;

static void serial_key(UINT8 c) {
  if (esc_len == 0 && c != 0x1b) {
    if (c == '\r' || c == '\n')
      queue_key(SCAN_NULL, CHAR_CARRIAGE_RETURN);
    else if (c == 0x7f || c == 0x08)
      queue_key(SCAN_NULL, CHAR_BACKSPACE);
    else
      queue_key(SCAN_NULL, c);
    return;
  }
  esc_buf[esc_len++] = c;
  if (esc_len == 2 && c != '[' && c != 'O') {
    queue_key(SCAN_ESC, 0);
    esc_len = 0;
    serial_key(c);
    return;
  }
  if (esc_len < 3)
    return;
  UINT8 last = esc_buf[esc_len - 1];
  if (last >= '0' && last <= '9' && esc_len < sizeof(esc_buf))
    return;
  UINT16 scan = SCAN_NULL;
  if (esc_len == 3) {
    switch (last) {
    case 'A': scan = SCAN_UP; break;
    case 'B': scan = SCAN_DOWN; break;
    case 'C': scan = SCAN_RIGHT; break;
    case 'D': scan = SCAN_LEFT; break;
    case 'H': scan = SCAN_HOME; break;
    case 'F': scan = SCAN_END; break;
    }
  } else if (last == '~') {
    /* ESC [ n ~ with n the vt key number */
    UINTN n = 0;
    for (UINTN i = 2; i + 1 < esc_len; ++i)
      n = n * 10 + (esc_buf[i] - '0');
    switch (n) {
    case 3: scan = SCAN_DELETE; break;
    case 5: scan = SCAN_PAGE_UP; break;
    case 6: scan = SCAN_PAGE_DOWN; break;
    case 19: scan = SCAN_F8; break;
    case 21: scan = SCAN_F10; break;
    }
  }
  if (scan != SCAN_NULL)
    queue_key(scan, 0);
  esc_len = 0;
}

/* scan code set 1 make codes for the keys a boot menu needs */
static void ps2_key(UINT8 code, BOOLEAN extended) {
  static const CHAR8 plain[0x3a] =
      "\0\0001234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
  if (code & 0x80)
    return;
  if (extended || code >= 0x3b) {
    switch (code) {
    case 0x48: queue_key(SCAN_UP, 0); break;
    case 0x50: queue_key(SCAN_DOWN, 0); break;
    case 0x4b: queue_key(SCAN_LEFT, 0); break;
    case 0x4d: queue_key(SCAN_RIGHT, 0); break;
    case 0x47: queue_key(SCAN_HOME, 0); break;
    case 0x4f: queue_key(SCAN_END, 0); break;
    case 0x49: queue_key(SCAN_PAGE_UP, 0); break;
    case 0x51: queue_key(SCAN_PAGE_DOWN, 0); break;
    case 0x53: queue_key(SCAN_DELETE, 0); break;
    case 0x42: queue_key(SCAN_F8, 0); break;
    case 0x44: queue_key(SCAN_F10, 0); break;
    case 0x1c: queue_key(SCAN_NULL, CHAR_CARRIAGE_RETURN); break;
    }
    return;
  }
  if (code == 0x01) {
    queue_key(SCAN_ESC, 0);
    return;
  }
  if (code < sizeof(plain) && plain[code])
    queue_key(SCAN_NULL, (CHAR16)(UINT8)plain[code]);
}

static void poll_input(void) {
  static BOOLEAN ps2_extended;
  while (inb(COM1 + 5) & 1)
    serial_key(inb(COM1));
  /* output buffer full and the byte is not from the aux (mouse) port */
  while ((inb(0x64) & 0x21) == 0x01) {
    UINT8 code = inb(0x60);
    if (code == 0xe0) {
      ps2_extended = TRUE;
      continue;
    }
    ps2_key(code, ps2_extended);
    ps2_extended = FALSE;
  }
}
#else
static void poll_input(void) {}
#endif

static EfiEmuEvent *event_of(EFI_EVENT e) {
  UINTN i = (UINTN)e - 1;
  return (i < EFIEMU_MAX_EVENTS && events[i].used) ? &events[i] : NULL;
}

static BOOLEAN event_ready(EfiEmuEvent *ev) {
  if (ev->type == EFIEMU_EVENT_KEY) {
    poll_input();
    return !queue_empty();
  }
  if (!ev->signaled && ev->deadline && now_100ns >= ev->deadline) {
    ev->signaled = TRUE;
    ev->deadline = ev->periodic ? now_100ns + ev->period : 0;
  }
  return ev->signaled;
}

/* the clock only moves while somebody waits, 100 us per step */
static void tick(void) {
  bs->Stall(100);
  now_100ns += 1000;
}

static EFI_STATUS EFIAPI create_event(UINT32 type, EFI_TPL tpl, EFI_EVENT_NOTIFY notify,
                                      VOID *context, EFI_EVENT *out) {
  (void)tpl;
  (void)notify;
  (void)context;
  if (!out)
    return EFI_INVALID_PARAMETER;
  for (UINTN i = 0; i < EFIEMU_MAX_EVENTS; ++i) {
    if (!events[i].used) {
      SetMem(&events[i], sizeof(events[i]), 0);
      events[i].used = TRUE;
      events[i].type = type;
      *out = (EFI_EVENT)(i + 1);
      return EFI_SUCCESS;
    }
  }
  return EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS EFIAPI set_timer(EFI_EVENT event, EFI_TIMER_DELAY kind, UINT64 delay) {
  EfiEmuEvent *ev = event_of(event);
  if (!ev)
    return EFI_INVALID_PARAMETER;
  ev->signaled = FALSE;
  ev->periodic = FALSE;
  ev->deadline = 0;
  if (kind == TimerCancel)
    return EFI_SUCCESS;
  if (delay == 0)
    delay = 1;
  ev->deadline = now_100ns + delay;
  if (kind == TimerPeriodic) {
    ev->periodic = TRUE;
    ev->period = delay;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI wait_for_event(UINTN count, EFI_EVENT *list, UINTN *index) {
  if (!count || !list)
    return EFI_INVALID_PARAMETER;
  for (;;) {
    for (UINTN i = 0; i < count; ++i) {
      EfiEmuEvent *ev = event_of(list[i]);
      if (!ev)
        return EFI_INVALID_PARAMETER;
      if (event_ready(ev)) {
        if (ev->type != EFIEMU_EVENT_KEY)
          ev->signaled = FALSE;
        if (index)
          *index = i;
        return EFI_SUCCESS;
      }
    }
    tick();
  }
}

static EFI_STATUS EFIAPI check_event(EFI_EVENT event) {
  EfiEmuEvent *ev = event_of(event);
  if (!ev)
    return EFI_INVALID_PARAMETER;
  if (!event_ready(ev))
    return EFI_NOT_READY;
  if (ev->type != EFIEMU_EVENT_KEY)
    ev->signaled = FALSE;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI signal_event(EFI_EVENT event) {
  EfiEmuEvent *ev = event_of(event);
  if (!ev)
    return EFI_INVALID_PARAMETER;
  ev->signaled = TRUE;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI close_event(EFI_EVENT event) {
  EfiEmuEvent *ev = event_of(event);
  if (!ev)
    return EFI_INVALID_PARAMETER;
  ev->used = FALSE;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI input_reset(SIMPLE_INPUT_INTERFACE *self, BOOLEAN extended) {
  (void)self;
  (void)extended;
  pending_head = pending_tail = 0;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI read_key(SIMPLE_INPUT_INTERFACE *self, EFI_INPUT_KEY *key) {
  (void)self;
  if (!key)
    return EFI_INVALID_PARAMETER;
  poll_input();
  if (queue_empty())
    return EFI_NOT_READY;
  *key = pending[pending_head];
  pending_head = (pending_head + 1) % 16;
  return EFI_SUCCESS;
}

void efiemu_input_install(EFI_SYSTEM_TABLE *table) {
  bs = table->BootServices;
  bs->CreateEvent = create_event;
  bs->SetTimer = set_timer;
  bs->WaitForEvent = wait_for_event;
  bs->CheckEvent = check_event;
  bs->SignalEvent = signal_event;
  bs->CloseEvent = close_event;

  console_in.Reset = input_reset;
  console_in.ReadKeyStroke = read_key;
  EFI_EVENT key_event = NULL;
  if (!EFI_ERROR(create_event(0, TPL_NOTIFY, NULL, NULL, &key_event)))
    event_of(key_event)->type = EFIEMU_EVENT_KEY;
  console_in.WaitForKey = key_event;
  table->ConIn = &console_in;
  table->ConsoleInHandle = (EFI_HANDLE)(UINTN)0x434f4e49;
}
