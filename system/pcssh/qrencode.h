/****************************************************************************
 * apps/system/pcssh/qrencode.h
 *
 * Minimal QR code encoder for SSH public key display.
 * Generates QR codes version 1-10 in alphanumeric/byte mode.
 * Output as ASCII art or raw bitmap.
 *
 ****************************************************************************/

#ifndef PCSSH_QRENCODE_H
#define PCSSH_QRENCODE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Maximum QR version supported (10 = 57x57 modules) */

#define QR_MAX_VERSION    10
#define QR_MAX_SIZE       (17 + 4 * QR_MAX_VERSION)  /* 57 */
#define QR_MAX_MODULES    (QR_MAX_SIZE * QR_MAX_SIZE)

/* Error correction levels */

#define QR_ECL_LOW        0
#define QR_ECL_MEDIUM     1
#define QR_ECL_QUARTILE   2
#define QR_ECL_HIGH       3

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct qrcode_s
{
  uint8_t version;
  uint8_t size;                             /* modules per side */
  uint8_t modules[QR_MAX_SIZE][QR_MAX_SIZE]; /* 0=white, 1=black */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: qrencode
 *
 * Description:
 *   Encode data into a QR code bitmap.
 *
 * Input Parameters:
 *   data      - Input data (binary safe)
 *   data_len  - Length of input data
 *   ecl       - Error correction level (QR_ECL_*)
 *   qr        - Output QR code structure
 *
 * Returned Value:
 *   0 on success, -1 if data too large for max version.
 *
 ****************************************************************************/

int qrencode(const uint8_t *data, size_t data_len, int ecl,
             struct qrcode_s *qr);

/****************************************************************************
 * Name: qr_print_ascii
 *
 * Description:
 *   Print QR code as ASCII art to a FILE stream.
 *   Uses Unicode block characters for compact display:
 *     upper-half + lower-half block = 2 rows per line.
 *
 ****************************************************************************/

void qr_print_ascii(FILE *fp, const struct qrcode_s *qr);

/****************************************************************************
 * Name: qr_print_simple
 *
 * Description:
 *   Print QR code using simple ASCII (## for black, spaces for white).
 *   One row per line — larger output but works on all terminals.
 *
 ****************************************************************************/

void qr_print_simple(FILE *fp, const struct qrcode_s *qr);

#endif /* PCSSH_QRENCODE_H */
