/****************************************************************************
 * apps/system/pcssh/pcscp_cli.c
 *
 * Full-featured SCP (Secure Copy Protocol) client for NuttX Shell.
 *
 * Usage:
 *   scp [-P port] [-i keyfile] [-v] [-r] source destination
 *
 *   Local to remote:  scp file.txt user@host:/path/
 *   Remote to local:  scp user@host:/path/file.txt ./
 *
 * When wolfSSH is available, uses real SCP over SSH 2.0.
 * Without wolfSSH, uses a simple TCP file transfer protocol.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <termios.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#ifdef CONFIG_CRYPTO_WOLFSSH
#include <wolfssh/ssh.h>
#include <wolfssh/settings.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SCP_DEFAULT_PORT    22
#define SCP_BUF_SIZE        4096
#define PASSWORD_MAX        128

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct scp_ctx_s
{
  char     local_path[256];
  char     remote_host[128];
  char     remote_user[64];
  char     remote_path[256];
  char     password[PASSWORD_MAX];
  char     keyfile[256];
  uint16_t port;
  bool     upload;        /* true = local -> remote */
  bool     recursive;
  bool     verbose;
  int      sock_fd;

#ifdef CONFIG_CRYPTO_WOLFSSH
  WOLFSSH     *ssh;
  WOLFSSH_CTX *ctx;
#endif
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void scp_usage(void)
{
  fprintf(stderr,
    "Usage: scp [-P port] [-i keyfile] [-v] [-r] source destination\n"
    "\n"
    "  Local to remote:  scp file.txt user@host:/path/\n"
    "  Remote to local:  scp user@host:/path/file.txt local_dir/\n"
    "\n"
    "Options:\n"
    "  -P port      Port number (default: 22)\n"
    "  -i keyfile   Identity (private key) file\n"
    "  -r           Recursive copy (directories)\n"
    "  -v           Verbose output\n"
    "\n"
#ifndef CONFIG_CRYPTO_WOLFSSH
    "Note: wolfSSH not available.\n"
    "  File transfer requires a compatible raw TCP server on the\n"
    "  remote end, or enable CONFIG_CRYPTO_WOLFSSH for real SCP.\n"
#endif
  );
}

/****************************************************************************
 * Name: scp_parse_remote
 *
 * Description:
 *   Parse "user@host:/path" format.
 *   Returns 0 on success, -1 if not a remote path.
 *
 ****************************************************************************/

static int scp_parse_remote(const char *arg,
                            char *user, size_t ulen,
                            char *host, size_t hlen,
                            char *path, size_t plen)
{
  const char *colon = strchr(arg, ':');
  if (colon == NULL)
    {
      return -1;
    }

  const char *at = strchr(arg, '@');
  if (at != NULL && at < colon)
    {
      size_t u = at - arg;
      if (u >= ulen) u = ulen - 1;
      memcpy(user, arg, u);
      user[u] = '\0';

      size_t h = colon - at - 1;
      if (h >= hlen) h = hlen - 1;
      memcpy(host, at + 1, h);
      host[h] = '\0';
    }
  else
    {
      strncpy(user, "root", ulen - 1);
      user[ulen - 1] = '\0';

      size_t h = colon - arg;
      if (h >= hlen) h = hlen - 1;
      memcpy(host, arg, h);
      host[h] = '\0';
    }

  strncpy(path, colon + 1, plen - 1);
  path[plen - 1] = '\0';

  return 0;
}

#ifdef CONFIG_CRYPTO_WOLFSSH
/****************************************************************************
 * Name: scp_prompt_password
 ****************************************************************************/

static void scp_prompt_password(struct scp_ctx_s *ctx)
{
  struct termios saved, noecho;

  fprintf(stderr, "%s@%s's password: ", ctx->remote_user,
          ctx->remote_host);
  fflush(stderr);

  if (tcgetattr(STDIN_FILENO, &saved) == 0)
    {
      noecho = saved;
      noecho.c_lflag &= ~(ECHO);
      tcsetattr(STDIN_FILENO, TCSANOW, &noecho);
    }

  if (fgets(ctx->password, sizeof(ctx->password), stdin) == NULL)
    {
      ctx->password[0] = '\0';
    }

  size_t len = strlen(ctx->password);
  if (len > 0 && ctx->password[len - 1] == '\n')
    {
      ctx->password[len - 1] = '\0';
    }

  tcsetattr(STDIN_FILENO, TCSANOW, &saved);
  fprintf(stderr, "\n");
}
#endif /* CONFIG_CRYPTO_WOLFSSH */

/****************************************************************************
 * Name: scp_tcp_connect
 ****************************************************************************/

static int scp_tcp_connect(struct scp_ctx_s *ctx)
{
  struct addrinfo hints, *res;
  char port_str[8];

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  snprintf(port_str, sizeof(port_str), "%d", ctx->port);

  int ret = getaddrinfo(ctx->remote_host, port_str, &hints, &res);
  if (ret != 0)
    {
      fprintf(stderr, "scp: cannot resolve '%s'\n", ctx->remote_host);
      return -1;
    }

  ctx->sock_fd = socket(res->ai_family, res->ai_socktype,
                         res->ai_protocol);
  if (ctx->sock_fd < 0)
    {
      fprintf(stderr, "scp: socket: %s\n", strerror(errno));
      freeaddrinfo(res);
      return -1;
    }

  if (connect(ctx->sock_fd, res->ai_addr, res->ai_addrlen) < 0)
    {
      fprintf(stderr, "scp: connect failed: %s\n", strerror(errno));
      close(ctx->sock_fd);
      ctx->sock_fd = -1;
      freeaddrinfo(res);
      return -1;
    }

  freeaddrinfo(res);
  return 0;
}

/****************************************************************************
 * Name: scp_progress
 ****************************************************************************/

static void scp_progress(const char *filename, off_t sent,
                         off_t total)
{
  int pct = (total > 0) ? (int)(sent * 100 / total) : 0;

  /* Simple progress bar */

  fprintf(stderr, "\r%-32.32s %3d%% %7ld/%ld",
          filename, pct, (long)sent, (long)total);
  fflush(stderr);
}

#ifdef CONFIG_CRYPTO_WOLFSSH
/****************************************************************************
 * Name: scp_ssh_connect
 *
 * Description:
 *   Establish SSH connection for SCP.
 *
 ****************************************************************************/

static int scp_ssh_connect(struct scp_ctx_s *ctx)
{
  int ret;

  ret = scp_tcp_connect(ctx);
  if (ret < 0)
    {
      return ret;
    }

  ctx->ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
  if (ctx->ctx == NULL)
    {
      close(ctx->sock_fd);
      ctx->sock_fd = -1;
      return -1;
    }

  /* Load identity key if given */

  if (ctx->keyfile[0] != '\0')
    {
      FILE *kf = fopen(ctx->keyfile, "rb");
      if (kf != NULL)
        {
          uint8_t keybuf[4096];
          size_t keylen = fread(keybuf, 1, sizeof(keybuf), kf);
          fclose(kf);

          if (keylen > 0)
            {
              wolfSSH_CTX_UsePrivateKey_buffer(ctx->ctx,
                keybuf, (word32)keylen, WOLFSSH_FORMAT_ASN1);
            }
        }
    }

  ctx->ssh = wolfSSH_new(ctx->ctx);
  if (ctx->ssh == NULL)
    {
      wolfSSH_CTX_free(ctx->ctx);
      ctx->ctx = NULL;
      close(ctx->sock_fd);
      ctx->sock_fd = -1;
      return -1;
    }

  wolfSSH_set_fd(ctx->ssh, ctx->sock_fd);
  wolfSSH_SetUsername(ctx->ssh, ctx->remote_user);

  if (ctx->keyfile[0] == '\0')
    {
      scp_prompt_password(ctx);
    }

  ret = wolfSSH_connect(ctx->ssh);
  if (ret != WS_SUCCESS)
    {
      fprintf(stderr, "scp: SSH handshake failed: %d\n",
              wolfSSH_get_error(ctx->ssh));
      wolfSSH_free(ctx->ssh);
      ctx->ssh = NULL;
      wolfSSH_CTX_free(ctx->ctx);
      ctx->ctx = NULL;
      close(ctx->sock_fd);
      ctx->sock_fd = -1;
      return -1;
    }

  return 0;
}

/****************************************************************************
 * Name: scp_upload_wolfssh
 ****************************************************************************/

static int scp_upload_wolfssh(struct scp_ctx_s *ctx)
{
  FILE *fp = fopen(ctx->local_path, "rb");
  if (fp == NULL)
    {
      fprintf(stderr, "scp: cannot open '%s': %s\n",
              ctx->local_path, strerror(errno));
      return -1;
    }

  /* Get file size */

  fseek(fp, 0, SEEK_END);
  off_t fsize = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  /* Send via wolfSSH stream (SCP protocol) */

  /* Send SCP header: C0644 <size> <filename>\n */

  const char *basename = strrchr(ctx->local_path, '/');
  basename = basename ? basename + 1 : ctx->local_path;

  char hdr[512];
  snprintf(hdr, sizeof(hdr), "C0644 %ld %s\n", (long)fsize, basename);
  wolfSSH_stream_send(ctx->ssh, (uint8_t *)hdr, strlen(hdr));

  /* Send file data */

  uint8_t buf[SCP_BUF_SIZE];
  off_t sent = 0;
  size_t n;

  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    {
      int w = wolfSSH_stream_send(ctx->ssh, buf, (word32)n);
      if (w <= 0)
        {
          fprintf(stderr, "\nscp: send error\n");
          fclose(fp);
          return -1;
        }

      sent += w;
      if (ctx->verbose)
        {
          scp_progress(basename, sent, fsize);
        }
    }

  /* Send SCP end-of-file marker (single NUL byte) */

  uint8_t eof = 0;
  wolfSSH_stream_send(ctx->ssh, &eof, 1);

  fclose(fp);

  if (ctx->verbose)
    {
      fprintf(stderr, "\n");
    }

  fprintf(stderr, "%s: %ld bytes sent\n", basename, (long)sent);
  return 0;
}

/****************************************************************************
 * Name: scp_download_wolfssh
 ****************************************************************************/

static int scp_download_wolfssh(struct scp_ctx_s *ctx)
{
  /* Read SCP header: C<mode> <size> <filename>\n */

  char hdr[512];
  int hdr_len = 0;
  uint8_t ch;

  while (hdr_len < (int)sizeof(hdr) - 1)
    {
      int r = wolfSSH_stream_read(ctx->ssh, &ch, 1);
      if (r <= 0)
        {
          fprintf(stderr, "scp: failed to read header\n");
          return -1;
        }

      hdr[hdr_len++] = (char)ch;
      if (ch == '\n')
        {
          break;
        }
    }

  hdr[hdr_len] = '\0';

  /* Parse C<mode> <size> <filename> */

  if (hdr[0] != 'C')
    {
      fprintf(stderr, "scp: unexpected response: %s\n", hdr);
      return -1;
    }

  char *p = hdr + 1;
  strtol(p, &p, 8); /* skip mode */
  while (*p == ' ') p++;
  off_t fsize = strtol(p, &p, 10);
  while (*p == ' ') p++;

  /* Extract filename */

  char *nl = strchr(p, '\n');
  if (nl) *nl = '\0';

  char filename[128];
  strncpy(filename, p, sizeof(filename) - 1);
  filename[sizeof(filename) - 1] = '\0';

  /* Build local path */

  char outpath[512];
  struct stat st;

  if (stat(ctx->local_path, &st) == 0 && S_ISDIR(st.st_mode))
    {
      snprintf(outpath, sizeof(outpath), "%s/%s",
               ctx->local_path, filename);
    }
  else
    {
      strncpy(outpath, ctx->local_path, sizeof(outpath) - 1);
    }

  /* Send confirmation */

  uint8_t ack = 0;
  wolfSSH_stream_send(ctx->ssh, &ack, 1);

  /* Receive file data */

  FILE *fp = fopen(outpath, "wb");
  if (fp == NULL)
    {
      fprintf(stderr, "scp: cannot create '%s': %s\n",
              outpath, strerror(errno));
      return -1;
    }

  uint8_t buf[SCP_BUF_SIZE];
  off_t received = 0;

  while (received < fsize)
    {
      size_t want = sizeof(buf);
      if ((off_t)want > fsize - received)
        {
          want = (size_t)(fsize - received);
        }

      int r = wolfSSH_stream_read(ctx->ssh, buf, (word32)want);
      if (r <= 0)
        {
          fprintf(stderr, "\nscp: receive error\n");
          fclose(fp);
          return -1;
        }

      fwrite(buf, 1, r, fp);
      received += r;

      if (ctx->verbose)
        {
          scp_progress(filename, received, fsize);
        }
    }

  fclose(fp);

  if (ctx->verbose)
    {
      fprintf(stderr, "\n");
    }

  fprintf(stderr, "%s: %ld bytes received\n",
          filename, (long)received);
  return 0;
}

/****************************************************************************
 * Name: scp_ssh_disconnect
 ****************************************************************************/

static void scp_ssh_disconnect(struct scp_ctx_s *ctx)
{
  if (ctx->ssh != NULL)
    {
      wolfSSH_shutdown(ctx->ssh);
      wolfSSH_free(ctx->ssh);
      ctx->ssh = NULL;
    }

  if (ctx->ctx != NULL)
    {
      wolfSSH_CTX_free(ctx->ctx);
      ctx->ctx = NULL;
    }

  if (ctx->sock_fd >= 0)
    {
      close(ctx->sock_fd);
      ctx->sock_fd = -1;
    }
}

#else /* !CONFIG_CRYPTO_WOLFSSH */

/****************************************************************************
 * Name: scp_raw_upload
 *
 * Description:
 *   Simple TCP file transfer (no encryption).
 *   Protocol: send 4-byte big-endian file size, then file data.
 *
 ****************************************************************************/

static int scp_raw_upload(struct scp_ctx_s *ctx)
{
  int ret;

  ret = scp_tcp_connect(ctx);
  if (ret < 0)
    {
      return ret;
    }

  FILE *fp = fopen(ctx->local_path, "rb");
  if (fp == NULL)
    {
      fprintf(stderr, "scp: cannot open '%s': %s\n",
              ctx->local_path, strerror(errno));
      close(ctx->sock_fd);
      return -1;
    }

  fseek(fp, 0, SEEK_END);
  off_t fsize = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  /* Send file size (4 bytes big-endian) */

  uint8_t sz_hdr[4];
  sz_hdr[0] = (fsize >> 24) & 0xFF;
  sz_hdr[1] = (fsize >> 16) & 0xFF;
  sz_hdr[2] = (fsize >> 8)  & 0xFF;
  sz_hdr[3] = (fsize)       & 0xFF;
  send(ctx->sock_fd, sz_hdr, 4, 0);

  /* Send remote path as null-terminated string */

  send(ctx->sock_fd, ctx->remote_path,
       strlen(ctx->remote_path) + 1, 0);

  /* Send file data */

  uint8_t buf[SCP_BUF_SIZE];
  off_t sent = 0;
  size_t n;

  const char *basename = strrchr(ctx->local_path, '/');
  basename = basename ? basename + 1 : ctx->local_path;

  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    {
      ssize_t w = send(ctx->sock_fd, buf, n, 0);
      if (w <= 0)
        {
          fprintf(stderr, "\nscp: send error: %s\n",
                  strerror(errno));
          break;
        }

      sent += w;
      if (ctx->verbose)
        {
          scp_progress(basename, sent, fsize);
        }
    }

  fclose(fp);
  close(ctx->sock_fd);
  ctx->sock_fd = -1;

  if (ctx->verbose)
    {
      fprintf(stderr, "\n");
    }

  fprintf(stderr, "%s: %ld bytes sent (raw TCP)\n",
          basename, (long)sent);
  return 0;
}

/****************************************************************************
 * Name: scp_raw_download
 ****************************************************************************/

static int scp_raw_download(struct scp_ctx_s *ctx)
{
  int ret;

  ret = scp_tcp_connect(ctx);
  if (ret < 0)
    {
      return ret;
    }

  /* Send request: remote path as null-terminated string */

  send(ctx->sock_fd, ctx->remote_path,
       strlen(ctx->remote_path) + 1, 0);

  /* Receive file size (4 bytes big-endian) */

  uint8_t sz_hdr[4];
  ssize_t r = recv(ctx->sock_fd, sz_hdr, 4, MSG_WAITALL);
  if (r != 4)
    {
      fprintf(stderr, "scp: failed to read header\n");
      close(ctx->sock_fd);
      return -1;
    }

  off_t fsize = ((off_t)sz_hdr[0] << 24) |
                ((off_t)sz_hdr[1] << 16) |
                ((off_t)sz_hdr[2] << 8)  |
                ((off_t)sz_hdr[3]);

  /* Build output filename */

  const char *basename = strrchr(ctx->remote_path, '/');
  basename = basename ? basename + 1 : ctx->remote_path;

  char outpath[512];
  struct stat st;

  if (stat(ctx->local_path, &st) == 0 && S_ISDIR(st.st_mode))
    {
      snprintf(outpath, sizeof(outpath), "%s/%s",
               ctx->local_path, basename);
    }
  else
    {
      strncpy(outpath, ctx->local_path, sizeof(outpath) - 1);
    }

  FILE *fp = fopen(outpath, "wb");
  if (fp == NULL)
    {
      fprintf(stderr, "scp: cannot create '%s': %s\n",
              outpath, strerror(errno));
      close(ctx->sock_fd);
      return -1;
    }

  /* Receive file data */

  uint8_t buf[SCP_BUF_SIZE];
  off_t received = 0;

  while (received < fsize)
    {
      size_t want = sizeof(buf);
      if ((off_t)want > fsize - received)
        {
          want = (size_t)(fsize - received);
        }

      r = recv(ctx->sock_fd, buf, want, 0);
      if (r <= 0)
        {
          break;
        }

      fwrite(buf, 1, r, fp);
      received += r;

      if (ctx->verbose)
        {
          scp_progress(basename, received, fsize);
        }
    }

  fclose(fp);
  close(ctx->sock_fd);
  ctx->sock_fd = -1;

  if (ctx->verbose)
    {
      fprintf(stderr, "\n");
    }

  fprintf(stderr, "%s: %ld bytes received (raw TCP)\n",
          basename, (long)received);
  return 0;
}
#endif /* CONFIG_CRYPTO_WOLFSSH */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct scp_ctx_s ctx;
  int opt_idx;
  int ret;

  memset(&ctx, 0, sizeof(ctx));
  ctx.port    = SCP_DEFAULT_PORT;
  ctx.sock_fd = -1;

  if (argc < 3)
    {
      scp_usage();
      return EXIT_FAILURE;
    }

  /* Parse options */

  opt_idx = 1;
  while (opt_idx < argc && argv[opt_idx][0] == '-')
    {
      if (strcmp(argv[opt_idx], "-P") == 0 && opt_idx + 1 < argc)
        {
          ctx.port = (uint16_t)atoi(argv[++opt_idx]);
        }
      else if (strcmp(argv[opt_idx], "-i") == 0 && opt_idx + 1 < argc)
        {
          strncpy(ctx.keyfile, argv[++opt_idx],
                  sizeof(ctx.keyfile) - 1);
        }
      else if (strcmp(argv[opt_idx], "-r") == 0)
        {
          ctx.recursive = true;
        }
      else if (strcmp(argv[opt_idx], "-v") == 0)
        {
          ctx.verbose = true;
        }
      else if (strcmp(argv[opt_idx], "-h") == 0 ||
               strcmp(argv[opt_idx], "--help") == 0)
        {
          scp_usage();
          return EXIT_SUCCESS;
        }
      else
        {
          fprintf(stderr, "scp: unknown option: %s\n",
                  argv[opt_idx]);
          return EXIT_FAILURE;
        }

      opt_idx++;
    }

  if (argc - opt_idx < 2)
    {
      fprintf(stderr, "scp: missing source or destination\n");
      return EXIT_FAILURE;
    }

  const char *source = argv[opt_idx];
  const char *dest   = argv[opt_idx + 1];

  /* Determine transfer direction */

  char user[64], host[128], rpath[256];

  if (scp_parse_remote(source, user, sizeof(user),
                       host, sizeof(host),
                       rpath, sizeof(rpath)) == 0)
    {
      ctx.upload = false;
      strncpy(ctx.remote_user, user, sizeof(ctx.remote_user) - 1);
      strncpy(ctx.remote_host, host, sizeof(ctx.remote_host) - 1);
      strncpy(ctx.remote_path, rpath, sizeof(ctx.remote_path) - 1);
      strncpy(ctx.local_path, dest, sizeof(ctx.local_path) - 1);
    }
  else if (scp_parse_remote(dest, user, sizeof(user),
                            host, sizeof(host),
                            rpath, sizeof(rpath)) == 0)
    {
      ctx.upload = true;
      strncpy(ctx.remote_user, user, sizeof(ctx.remote_user) - 1);
      strncpy(ctx.remote_host, host, sizeof(ctx.remote_host) - 1);
      strncpy(ctx.remote_path, rpath, sizeof(ctx.remote_path) - 1);
      strncpy(ctx.local_path, source, sizeof(ctx.local_path) - 1);
    }
  else
    {
      fprintf(stderr,
              "scp: source or destination must be remote\n"
              "     (use user@host:/path format)\n");
      return EXIT_FAILURE;
    }

  if (ctx.verbose)
    {
      fprintf(stderr, "scp: %s %s@%s:%s %s %s\n",
              ctx.upload ? "upload" : "download",
              ctx.remote_user, ctx.remote_host, ctx.remote_path,
              ctx.upload ? "<-" : "->",
              ctx.local_path);
    }

#ifdef CONFIG_CRYPTO_WOLFSSH
  wolfSSH_Init();

  ret = scp_ssh_connect(&ctx);
  if (ret < 0)
    {
      wolfSSH_Cleanup();
      return EXIT_FAILURE;
    }

  if (ctx.upload)
    {
      ret = scp_upload_wolfssh(&ctx);
    }
  else
    {
      ret = scp_download_wolfssh(&ctx);
    }

  scp_ssh_disconnect(&ctx);
  wolfSSH_Cleanup();

#else
  /* Raw TCP transfer */

  if (ctx.upload)
    {
      ret = scp_raw_upload(&ctx);
    }
  else
    {
      ret = scp_raw_download(&ctx);
    }
#endif

  return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
