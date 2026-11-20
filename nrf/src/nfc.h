// NFC tag emulation on the XIAO's two NFC pads (P0.09 / P0.10, so those two pins are not GPIO).
#pragma once
void nfc_tag_init(void);
int nfc_field_count(void);       // times a phone's field was seen
int nfc_read_count(void);        // times a phone read the tag
void nfc_tapped(void);           // implemented in main.c: called from a work item after a read
