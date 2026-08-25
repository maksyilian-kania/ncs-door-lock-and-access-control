# Aliro NFC User Device POC

Proof-of-concept firmware for an Aliro **User Device** on the nRF54LM20B DK, using the board's built-in NFC antenna and NFCT peripheral.

The application initializes NFC in Type 4 Tag listen mode, enters **System OFF** to minimize power, wakes when an Aliro reader's NFC field is detected, and logs the first C-APDU received from the reader.

## Requirements

- nRF54LM20B DK (PCA10184) with built-in NFC tag antenna
- An Aliro-capable NFC reader (for example, the [Aliro Access Control Application](../aliro-access-control-app/))
- Serial console (UART) for log output

## Build and flash

From the repository root:

```bash
ncs west build -p -b nrf54lm20dk/nrf54lm20b/cpuapp applications/aliro-nfc-user-device
ncs west flash
```

Connect a serial terminal to the DK console UART.

## Expected behavior

1. After boot or flash, the device starts NFC listen mode and schedules System OFF in 3 seconds.
2. If no reader is present, the device powers down. Console output stops.
3. When you tap the DK on an Aliro reader, the device wakes (full reset), re-initializes NFC, and logs the wake reason.
4. The first complete C-APDU from the reader is logged as a hex dump. If it is an Aliro expedited-phase SELECT, the log notes the AID `A000000909ACCE5501`.
5. When the reader is removed, the device returns to System OFF after 3 seconds.

Example log output after a successful tap:

```
[inf] aliro_nfc_ud: Wake-up cause: NFC field detect
[inf] aliro_nfc_ud: NFC field detected
[inf] aliro_nfc_ud: First message from Aliro reader (C-APDU):
[inf] aliro_nfc_ud:   00 a4 04 00 09 a0 00 00 09 09 ac ce 55 01 ...
[inf] aliro_nfc_ud: C-APDU header: CLA=0x00 INS=0xa4 P1=0x04 P2=0x00
[inf] aliro_nfc_ud: Detected Aliro expedited phase SELECT AID
```

## Scope and limitations

This POC does **not** implement the Aliro Access Protocol. It only:

- Runs NFC-A listen mode with ISO-DEP (T4T raw mode)
- Handles System OFF / NFC field wake
- Logs the first reader message
- Sends a placeholder R-APDU (`6A82`, file not found) so the ISO-DEP exchange can complete

Full Aliro authentication (AUTH0, AUTH1, and so on) is out of scope for this initial version.

## Further reading

For architecture, protocol layering, and design rationale, see [docs/architecture.md](docs/architecture.md).
