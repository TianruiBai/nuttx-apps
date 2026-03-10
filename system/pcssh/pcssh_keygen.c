/****************************************************************************
 * apps/system/pcssh/pcssh_keygen.c
 *
 * SSH key generator for PicoCalc.
 *
 * Usage:
 *   ssh-keygen [-t ed25519|rsa] [-b bits] [-f keyfile] [-q]
 *
 * Features:
 *   - Ed25519 key generation (default, using wolfCrypt)
 *   - RSA key generation (2048/4096 bit)
 *   - Saves private key and public key (.pub)
 *   - Displays public key as QR code for easy scanning
 *   - Default save location: /data/.ssh/ or /mnt/sd/.ssh/
 *
 * Requires CONFIG_CRYPTO_WOLFSSL for wolfCrypt key generation.
 * Without wolfCrypt, uses /dev/urandom for a basic keypair.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#include "qrencode.h"

#ifdef CONFIG_CRYPTO_WOLFSSL
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/rsa.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/coding.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DEFAULT_KEY_DIR_FLASH   "/data/.ssh"
#define DEFAULT_KEY_DIR_SD      "/mnt/sd/.ssh"
#define DEFAULT_ED25519_FILE    "id_ed25519"
#define DEFAULT_RSA_FILE        "id_rsa"
#define MAX_PUBKEY_LINE         1024
#define MAX_PRIVKEY_BUF         4096

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum keygen_type_e
{
  KEYGEN_ED25519 = 0,
  KEYGEN_RSA     = 1,
};

struct keygen_args_s
{
  enum keygen_type_e type;
  int                bits;        /* RSA bits (2048/4096) */
  char               outfile[256];
  bool               quiet;
  bool               show_qr;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void keygen_usage(void)
{
  fprintf(stderr,
    "Usage: ssh-keygen [-t type] [-b bits] [-f keyfile] [-q]\n"
    "\n"
    "Options:\n"
    "  -t type      Key type: ed25519 (default) or rsa\n"
    "  -b bits      RSA key size: 2048 (default) or 4096\n"
    "  -f keyfile   Output file path\n"
    "  -q           Quiet mode (no QR code output)\n"
    "\n"
    "Keys are saved to:\n"
    "  %s/  (internal flash, preferred)\n"
    "  %s/  (SD card, fallback)\n"
    "\n"
    "The public key (.pub) is also displayed as a QR code\n"
    "for easy transfer to authorized_keys on servers.\n"
    "\n"
#ifdef CONFIG_CRYPTO_WOLFSSL
    "Crypto engine: wolfCrypt (Ed25519 + RSA)\n"
#else
    "WARNING: wolfCrypt not available.\n"
    "  Key generation requires CONFIG_CRYPTO_WOLFSSL=y.\n"
#endif
    , DEFAULT_KEY_DIR_FLASH, DEFAULT_KEY_DIR_SD
  );
}

/****************************************************************************
 * Name: ensure_ssh_dir
 *
 * Description:
 *   Create .ssh directory in the preferred location.
 *   Returns the directory path, or NULL if neither works.
 *
 ****************************************************************************/

static const char *ensure_ssh_dir(void)
{
  struct stat st;

  /* Try internal flash first */

  if (stat("/data", &st) == 0 && S_ISDIR(st.st_mode))
    {
      mkdir(DEFAULT_KEY_DIR_FLASH, 0700);
      return DEFAULT_KEY_DIR_FLASH;
    }

  /* Fall back to SD card */

  if (stat("/mnt/sd", &st) == 0 && S_ISDIR(st.st_mode))
    {
      mkdir(DEFAULT_KEY_DIR_SD, 0700);
      return DEFAULT_KEY_DIR_SD;
    }

  return NULL;
}

/****************************************************************************
 * Name: base64_encode_simple
 *
 * Description:
 *   Simple base64 encoder for public key output.
 *
 ****************************************************************************/

static const char b64_table[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode_simple(const uint8_t *in, size_t in_len,
                                char *out, size_t out_max)
{
  size_t o = 0;
  size_t i = 0;

  while (i < in_len)
    {
      uint32_t a = (i < in_len) ? in[i++] : 0;
      uint32_t b = (i < in_len) ? in[i++] : 0;
      uint32_t c = (i < in_len) ? in[i++] : 0;

      uint32_t triple = (a << 16) | (b << 8) | c;

      if (o + 4 >= out_max) break;

      out[o++] = b64_table[(triple >> 18) & 0x3F];
      out[o++] = b64_table[(triple >> 12) & 0x3F];
      out[o++] = (i > in_len + 1) ? '=' :
                 b64_table[(triple >> 6) & 0x3F];
      out[o++] = (i > in_len) ? '=' :
                 b64_table[triple & 0x3F];
    }

  out[o] = '\0';
  return (int)o;
}

/****************************************************************************
 * Name: write_openssh_pubkey
 *
 * Description:
 *   Format and write an OpenSSH-format public key line.
 *   Format: "ssh-ed25519 AAAA...base64... user@picocalc\n"
 *
 ****************************************************************************/

static int write_openssh_pubkey(const char *path, const char *keytype,
                                const uint8_t *pubkey, size_t pubkey_len)
{
  FILE *fp = fopen(path, "w");
  if (fp == NULL)
    {
      fprintf(stderr, "ssh-keygen: cannot create %s: %s\n",
              path, strerror(errno));
      return -1;
    }

  /* Build SSH wire format: string(keytype) + string(pubkey_data) */

  uint8_t wire[512];
  int wire_len = 0;
  size_t kt_len = strlen(keytype);

  /* Key type length (4 bytes big-endian) */

  wire[wire_len++] = (kt_len >> 24) & 0xFF;
  wire[wire_len++] = (kt_len >> 16) & 0xFF;
  wire[wire_len++] = (kt_len >> 8)  & 0xFF;
  wire[wire_len++] = (kt_len)       & 0xFF;

  /* Key type string */

  memcpy(&wire[wire_len], keytype, kt_len);
  wire_len += kt_len;

  /* Public key data length */

  wire[wire_len++] = (pubkey_len >> 24) & 0xFF;
  wire[wire_len++] = (pubkey_len >> 16) & 0xFF;
  wire[wire_len++] = (pubkey_len >> 8)  & 0xFF;
  wire[wire_len++] = (pubkey_len)       & 0xFF;

  /* Public key data */

  memcpy(&wire[wire_len], pubkey, pubkey_len);
  wire_len += pubkey_len;

  /* Base64 encode */

  char b64[MAX_PUBKEY_LINE];
  base64_encode_simple(wire, wire_len, b64, sizeof(b64));

  fprintf(fp, "%s %s picocalc@picocalc\n", keytype, b64);
  fclose(fp);

  return 0;
}

/****************************************************************************
 * Name: display_pubkey_qr
 *
 * Description:
 *   Show the public key as a QR code on the terminal.
 *
 ****************************************************************************/

static void display_pubkey_qr(const char *pubkey_path)
{
  FILE *fp = fopen(pubkey_path, "r");
  if (fp == NULL) return;

  char line[MAX_PUBKEY_LINE];
  if (fgets(line, sizeof(line), fp) == NULL)
    {
      fclose(fp);
      return;
    }

  fclose(fp);

  /* Remove trailing newline */

  size_t len = strlen(line);
  if (len > 0 && line[len - 1] == '\n')
    {
      line[--len] = '\0';
    }

  /* Generate QR code */

  struct qrcode_s *qr = (struct qrcode_s *)malloc(sizeof(struct qrcode_s));
  if (qr == NULL)
    {
      fprintf(stderr, "ssh-keygen: out of memory for QR\n");
      return;
    }

  int ret = qrencode((const uint8_t *)line, len, QR_ECL_LOW, qr);
  if (ret < 0)
    {
      fprintf(stderr, "ssh-keygen: public key too long for QR code\n");
      free(qr);
      return;
    }

  fprintf(stderr, "\nPublic key QR code (scan to copy):\n\n");
  qr_print_simple(stderr, qr);
  fprintf(stderr, "\n");

  free(qr);
}

#ifdef CONFIG_CRYPTO_WOLFSSL

/****************************************************************************
 * Name: keygen_ed25519
 ****************************************************************************/

static int keygen_ed25519(const char *privpath, const char *pubpath,
                          bool quiet)
{
  WC_RNG rng;
  ed25519_key key;
  int ret;

  ret = wc_InitRng(&rng);
  if (ret != 0)
    {
      fprintf(stderr, "ssh-keygen: RNG init failed: %d\n", ret);
      return -1;
    }

  ret = wc_ed25519_init(&key);
  if (ret != 0)
    {
      wc_FreeRng(&rng);
      fprintf(stderr, "ssh-keygen: key init failed: %d\n", ret);
      return -1;
    }

  if (!quiet)
    {
      fprintf(stderr, "Generating Ed25519 key pair...\n");
    }

  ret = wc_ed25519_make_key(&rng, ED25519_KEY_SIZE, &key);
  if (ret != 0)
    {
      fprintf(stderr, "ssh-keygen: key generation failed: %d\n", ret);
      wc_ed25519_free(&key);
      wc_FreeRng(&rng);
      return -1;
    }

  /* Export private key (DER format) */

  uint8_t privder[256];
  word32  privder_len = sizeof(privder);
  ret = wc_Ed25519KeyToDer(&key, privder, &privder_len);
  if (ret > 0)
    {
      privder_len = ret;
    }
  else
    {
      /* Fallback: export raw private key */

      privder_len = ED25519_KEY_SIZE;
      ret = wc_ed25519_export_private_only(&key, privder, &privder_len);
    }

  /* Write private key with PEM-like wrapper */

  FILE *fp = fopen(privpath, "wb");
  if (fp != NULL)
    {
      char b64[MAX_PRIVKEY_BUF];
      base64_encode_simple(privder, privder_len, b64, sizeof(b64));

      fprintf(fp, "-----BEGIN OPENSSH PRIVATE KEY-----\n");

      /* Wrap at 70 chars */

      int b64len = strlen(b64);
      for (int i = 0; i < b64len; i += 70)
        {
          int chunk = b64len - i;
          if (chunk > 70) chunk = 70;
          fprintf(fp, "%.*s\n", chunk, b64 + i);
        }

      fprintf(fp, "-----END OPENSSH PRIVATE KEY-----\n");
      fclose(fp);
      chmod(privpath, 0600);
    }

  /* Export public key */

  uint8_t pubraw[ED25519_PUB_KEY_SIZE];
  word32  pubraw_len = sizeof(pubraw);
  wc_ed25519_export_public(&key, pubraw, &pubraw_len);

  write_openssh_pubkey(pubpath, "ssh-ed25519", pubraw, pubraw_len);

  wc_ed25519_free(&key);
  wc_FreeRng(&rng);

  if (!quiet)
    {
      fprintf(stderr, "Your identification has been saved in %s\n",
              privpath);
      fprintf(stderr, "Your public key has been saved in %s\n",
              pubpath);
    }

  return 0;
}

/****************************************************************************
 * Name: keygen_rsa
 ****************************************************************************/

static int keygen_rsa(const char *privpath, const char *pubpath,
                      int bits, bool quiet)
{
  WC_RNG rng;
  RsaKey key;
  int ret;

  ret = wc_InitRng(&rng);
  if (ret != 0)
    {
      fprintf(stderr, "ssh-keygen: RNG init failed: %d\n", ret);
      return -1;
    }

  ret = wc_InitRsaKey(&key, NULL);
  if (ret != 0)
    {
      wc_FreeRng(&rng);
      return -1;
    }

  if (!quiet)
    {
      fprintf(stderr, "Generating RSA key (%d bit)...\n", bits);
      fprintf(stderr, "This may take a moment...\n");
    }

  ret = wc_MakeRsaKey(&key, bits, 65537, &rng);
  if (ret != 0)
    {
      fprintf(stderr, "ssh-keygen: RSA keygen failed: %d\n", ret);
      wc_FreeRsaKey(&key);
      wc_FreeRng(&rng);
      return -1;
    }

  /* Export private key (DER) */

  uint8_t privder[MAX_PRIVKEY_BUF];
  int privder_len = wc_RsaKeyToDer(&key, privder, sizeof(privder));
  if (privder_len < 0)
    {
      fprintf(stderr, "ssh-keygen: private key export failed\n");
      wc_FreeRsaKey(&key);
      wc_FreeRng(&rng);
      return -1;
    }

  /* Write private key */

  FILE *fp = fopen(privpath, "wb");
  if (fp != NULL)
    {
      char b64[MAX_PRIVKEY_BUF * 2];
      base64_encode_simple(privder, privder_len, b64, sizeof(b64));

      fprintf(fp, "-----BEGIN RSA PRIVATE KEY-----\n");
      int b64len = strlen(b64);
      for (int i = 0; i < b64len; i += 64)
        {
          int chunk = b64len - i;
          if (chunk > 64) chunk = 64;
          fprintf(fp, "%.*s\n", chunk, b64 + i);
        }

      fprintf(fp, "-----END RSA PRIVATE KEY-----\n");
      fclose(fp);
      chmod(privpath, 0600);
    }

  /* Export public key: RSA public key is (e, n) */

  uint8_t n_buf[512], e_buf[8];
  word32 n_len = sizeof(n_buf), e_len = sizeof(e_buf);

  ret = wc_RsaFlattenPublicKey(&key, e_buf, &e_len, n_buf, &n_len);
  if (ret == 0)
    {
      /* Build SSH wire format for ssh-rsa: string(e) + string(n) */

      uint8_t wire[1024];
      int wlen = 0;

      /* The write_openssh_pubkey builds the whole wire format,
       * but for RSA we need a custom one.
       * Format: string("ssh-rsa") + mpint(e) + mpint(n)
       */

      /* Key type */

      const char *kt = "ssh-rsa";
      size_t kt_len = strlen(kt);
      wire[wlen++] = (kt_len >> 24) & 0xFF;
      wire[wlen++] = (kt_len >> 16) & 0xFF;
      wire[wlen++] = (kt_len >> 8)  & 0xFF;
      wire[wlen++] = (kt_len)       & 0xFF;
      memcpy(&wire[wlen], kt, kt_len);
      wlen += kt_len;

      /* e as mpint */

      wire[wlen++] = (e_len >> 24) & 0xFF;
      wire[wlen++] = (e_len >> 16) & 0xFF;
      wire[wlen++] = (e_len >> 8)  & 0xFF;
      wire[wlen++] = (e_len)       & 0xFF;
      memcpy(&wire[wlen], e_buf, e_len);
      wlen += e_len;

      /* n as mpint (add leading zero if MSB set) */

      bool need_pad = (n_buf[0] & 0x80) != 0;
      word32 n_wire_len = n_len + (need_pad ? 1 : 0);
      wire[wlen++] = (n_wire_len >> 24) & 0xFF;
      wire[wlen++] = (n_wire_len >> 16) & 0xFF;
      wire[wlen++] = (n_wire_len >> 8)  & 0xFF;
      wire[wlen++] = (n_wire_len)       & 0xFF;
      if (need_pad) wire[wlen++] = 0;
      memcpy(&wire[wlen], n_buf, n_len);
      wlen += n_len;

      /* Base64 and write */

      char b64[MAX_PUBKEY_LINE];
      base64_encode_simple(wire, wlen, b64, sizeof(b64));

      fp = fopen(pubpath, "w");
      if (fp != NULL)
        {
          fprintf(fp, "ssh-rsa %s picocalc@picocalc\n", b64);
          fclose(fp);
        }
    }

  wc_FreeRsaKey(&key);
  wc_FreeRng(&rng);

  if (!quiet)
    {
      fprintf(stderr, "Your identification has been saved in %s\n",
              privpath);
      fprintf(stderr, "Your public key has been saved in %s\n",
              pubpath);
    }

  return 0;
}

#else /* !CONFIG_CRYPTO_WOLFSSL */

/****************************************************************************
 * Name: keygen_random_bytes
 *
 * Description:
 *   Generate random bytes from /dev/urandom.
 *
 ****************************************************************************/

static int keygen_random_bytes(uint8_t *buf, size_t len)
{
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
    {
      /* Fallback: use time-based seed */

      srand((unsigned)time(NULL));
      for (size_t i = 0; i < len; i++)
        {
          buf[i] = (uint8_t)(rand() & 0xFF);
        }

      return 0;
    }

  ssize_t n = read(fd, buf, len);
  close(fd);

  return (n == (ssize_t)len) ? 0 : -1;
}

/****************************************************************************
 * Name: keygen_simple
 *
 * Description:
 *   Generate a basic 256-bit random key (not real Ed25519).
 *   This is only useful for identification, not crypto security.
 *   wolfCrypt is needed for real key generation.
 *
 ****************************************************************************/

static int keygen_simple(const char *privpath, const char *pubpath,
                         bool quiet)
{
  uint8_t privraw[32];
  uint8_t pubraw[32];

  if (!quiet)
    {
      fprintf(stderr,
        "WARNING: wolfCrypt not available.\n"
        "  Generating random identity key (NOT secure Ed25519).\n"
        "  Enable CONFIG_CRYPTO_WOLFSSL for real key generation.\n\n"
        "Generating random keypair...\n");
    }

  keygen_random_bytes(privraw, 32);

  /* "Public key" = hash of private (simple XOR fold) */

  for (int i = 0; i < 32; i++)
    {
      pubraw[i] = privraw[i] ^ privraw[(i + 13) % 32] ^
                  privraw[(i + 7) % 32];
    }

  /* Write private key */

  FILE *fp = fopen(privpath, "wb");
  if (fp != NULL)
    {
      char b64[256];
      base64_encode_simple(privraw, 32, b64, sizeof(b64));
      fprintf(fp, "-----BEGIN PICOCALC PRIVATE KEY-----\n");
      fprintf(fp, "%s\n", b64);
      fprintf(fp, "-----END PICOCALC PRIVATE KEY-----\n");
      fclose(fp);
      chmod(privpath, 0600);
    }

  /* Write public key */

  write_openssh_pubkey(pubpath, "ssh-ed25519", pubraw, 32);

  if (!quiet)
    {
      fprintf(stderr, "Saved: %s (private)\n", privpath);
      fprintf(stderr, "Saved: %s (public)\n", pubpath);
    }

  return 0;
}

#endif /* CONFIG_CRYPTO_WOLFSSL */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct keygen_args_s args;
  int ret;

  memset(&args, 0, sizeof(args));
  args.type    = KEYGEN_ED25519;
  args.bits    = 2048;
  args.show_qr = true;

  /* Parse arguments */

  int i = 1;
  while (i < argc)
    {
      if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
        {
          i++;
          if (strcmp(argv[i], "rsa") == 0)
            {
              args.type = KEYGEN_RSA;
            }
          else if (strcmp(argv[i], "ed25519") == 0)
            {
              args.type = KEYGEN_ED25519;
            }
          else
            {
              fprintf(stderr, "ssh-keygen: unknown type '%s'\n",
                      argv[i]);
              return EXIT_FAILURE;
            }
        }
      else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
        {
          args.bits = atoi(argv[++i]);
          if (args.bits != 2048 && args.bits != 4096)
            {
              fprintf(stderr,
                "ssh-keygen: RSA bits must be 2048 or 4096\n");
              return EXIT_FAILURE;
            }
        }
      else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
        {
          strncpy(args.outfile, argv[++i],
                  sizeof(args.outfile) - 1);
        }
      else if (strcmp(argv[i], "-q") == 0)
        {
          args.quiet = true;
          args.show_qr = false;
        }
      else if (strcmp(argv[i], "-h") == 0 ||
               strcmp(argv[i], "--help") == 0)
        {
          keygen_usage();
          return EXIT_SUCCESS;
        }
      else
        {
          fprintf(stderr, "ssh-keygen: unknown option: %s\n",
                  argv[i]);
          return EXIT_FAILURE;
        }

      i++;
    }

  /* Determine output file path */

  char privpath[280];
  char pubpath[280];

  if (args.outfile[0] != '\0')
    {
      snprintf(privpath, sizeof(privpath), "%s", args.outfile);
    }
  else
    {
      const char *ssh_dir = ensure_ssh_dir();
      if (ssh_dir == NULL)
        {
          fprintf(stderr,
            "ssh-keygen: no writable storage found.\n"
            "  Mount SD card at /mnt/sd or internal flash at /data\n");
          return EXIT_FAILURE;
        }

      const char *basename = (args.type == KEYGEN_RSA) ?
                             DEFAULT_RSA_FILE : DEFAULT_ED25519_FILE;
      snprintf(privpath, sizeof(privpath), "%s/%s", ssh_dir, basename);
    }

  snprintf(pubpath, sizeof(pubpath), "%s.pub", privpath);

  /* Check if key already exists */

  struct stat st;
  if (stat(privpath, &st) == 0)
    {
      fprintf(stderr, "%s already exists.\n", privpath);
      fprintf(stderr, "Overwrite (y/n)? ");
      fflush(stderr);

      char answer[4];
      if (fgets(answer, sizeof(answer), stdin) == NULL ||
          (answer[0] != 'y' && answer[0] != 'Y'))
        {
          fprintf(stderr, "Aborted.\n");
          return EXIT_SUCCESS;
        }
    }

  /* Generate the key */

#ifdef CONFIG_CRYPTO_WOLFSSL
  if (args.type == KEYGEN_RSA)
    {
      ret = keygen_rsa(privpath, pubpath, args.bits, args.quiet);
    }
  else
    {
      ret = keygen_ed25519(privpath, pubpath, args.quiet);
    }
#else
  ret = keygen_simple(privpath, pubpath, args.quiet);
#endif

  if (ret != 0)
    {
      return EXIT_FAILURE;
    }

  /* Display public key */

  if (!args.quiet)
    {
      fprintf(stderr, "\nPublic key:\n");

      FILE *fp = fopen(pubpath, "r");
      if (fp != NULL)
        {
          char line[MAX_PUBKEY_LINE];
          if (fgets(line, sizeof(line), fp) != NULL)
            {
              fprintf(stderr, "  %s", line);
            }

          fclose(fp);
        }
    }

  /* Display QR code */

  if (args.show_qr)
    {
      display_pubkey_qr(pubpath);
    }

  return EXIT_SUCCESS;
}
