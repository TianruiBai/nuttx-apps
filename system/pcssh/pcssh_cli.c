/****************************************************************************
 * apps/system/pcssh/pcssh_cli.c
 *
 * Full-featured SSH / telnet client for NuttX Shell.
 *
 * Usage:
 *   ssh [user@]host [-p port] [-l user] [-i keyfile] [-v]
 *
 * Features:
 *   - wolfSSH encrypted sessions (when CONFIG_CRYPTO_WOLFSSH)
 *   - Raw TCP fallback (telnet-like) when wolfSSH is absent
 *   - Password authentication with interactive prompt
 *   - Public-key authentication (-i flag)
 *   - Terminal raw mode (character-at-a-time I/O)
 *   - Escape sequences: ~. disconnect, ~? help
 *   - TCP keep-alive
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <poll.h>
#include <termios.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
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

#define SSH_DEFAULT_PORT     22
#define RECV_BUF_SIZE        1024
#define SEND_BUF_SIZE        256
#define PASSWORD_MAX         128

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ssh_session_s
{
  int              sock_fd;
  char             host[128];
  char             user[64];
  char             password[PASSWORD_MAX];
  char             keyfile[256];
  uint16_t         port;
  bool             verbose;
  volatile bool    running;
  struct termios   orig_termios;
  bool             termios_saved;

#ifdef CONFIG_CRYPTO_WOLFSSH
  WOLFSSH         *ssh;
  WOLFSSH_CTX     *ctx;
#endif
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void ssh_usage(void)
{
  fprintf(stderr,
    "Usage: ssh [user@]host [-p port] [-l user] [-i keyfile] [-v]\n"
    "\n"
    "Options:\n"
    "  -p port      Port number (default: 22)\n"
    "  -l user      Login name\n"
    "  -i keyfile   Identity (private key) file\n"
    "  -v           Verbose output\n"
    "\n"
    "Escape sequences (after newline):\n"
    "  ~.           Disconnect\n"
    "  ~?           Display help\n"
    "  ~~           Send literal ~\n"
    "\n"
#ifdef CONFIG_CRYPTO_WOLFSSH
    "Encryption: wolfSSH (SSH 2.0)\n"
#else
    "Encryption: not available (raw TCP mode)\n"
    "  Enable CONFIG_CRYPTO_WOLFSSH for encrypted SSH.\n"
#endif
  );
}

/****************************************************************************
 * Name: ssh_parse_args
 ****************************************************************************/

static int ssh_parse_args(int argc, char *argv[],
                          struct ssh_session_s *s)
{
  memset(s, 0, sizeof(*s));
  s->port    = SSH_DEFAULT_PORT;
  s->sock_fd = -1;

  if (argc < 2)
    {
      ssh_usage();
      return -1;
    }

  int i = 1;
  char *host_arg = NULL;

  while (i < argc)
    {
      if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
        {
          s->port = (uint16_t)atoi(argv[++i]);
        }
      else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
        {
          strncpy(s->user, argv[++i], sizeof(s->user) - 1);
        }
      else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
        {
          strncpy(s->keyfile, argv[++i], sizeof(s->keyfile) - 1);
        }
      else if (strcmp(argv[i], "-v") == 0)
        {
          s->verbose = true;
        }
      else if (strcmp(argv[i], "-h") == 0 ||
               strcmp(argv[i], "--help") == 0)
        {
          ssh_usage();
          return -1;
        }
      else if (argv[i][0] != '-')
        {
          host_arg = argv[i];
        }
      else
        {
          fprintf(stderr, "ssh: unknown option: %s\n", argv[i]);
          return -1;
        }

      i++;
    }

  if (host_arg == NULL)
    {
      fprintf(stderr, "ssh: no host specified\n");
      return -1;
    }

  /* Parse user@host */

  char *at = strchr(host_arg, '@');
  if (at != NULL)
    {
      *at = '\0';
      strncpy(s->user, host_arg, sizeof(s->user) - 1);
      strncpy(s->host, at + 1, sizeof(s->host) - 1);
    }
  else
    {
      strncpy(s->host, host_arg, sizeof(s->host) - 1);
    }

  if (s->user[0] == '\0')
    {
      strncpy(s->user, "root", sizeof(s->user) - 1);
    }

  return 0;
}

#ifdef CONFIG_CRYPTO_WOLFSSH
/****************************************************************************
 * Name: ssh_prompt_password
 ****************************************************************************/

static int ssh_prompt_password(struct ssh_session_s *s)
{
  struct termios saved, noecho;

  fprintf(stderr, "%s@%s's password: ", s->user, s->host);
  fflush(stderr);

  /* Disable echo */

  if (tcgetattr(STDIN_FILENO, &saved) == 0)
    {
      noecho = saved;
      noecho.c_lflag &= ~(ECHO);
      tcsetattr(STDIN_FILENO, TCSANOW, &noecho);
    }

  if (fgets(s->password, sizeof(s->password), stdin) == NULL)
    {
      s->password[0] = '\0';
    }

  /* Strip trailing newline */

  size_t len = strlen(s->password);
  if (len > 0 && s->password[len - 1] == '\n')
    {
      s->password[len - 1] = '\0';
    }

  /* Restore echo */

  tcsetattr(STDIN_FILENO, TCSANOW, &saved);
  fprintf(stderr, "\n");

  return (s->password[0] != '\0') ? 0 : -1;
}
#endif /* CONFIG_CRYPTO_WOLFSSH */

/****************************************************************************
 * Name: ssh_enter_raw_mode
 ****************************************************************************/

static void ssh_enter_raw_mode(struct ssh_session_s *s)
{
  struct termios raw;

  if (tcgetattr(STDIN_FILENO, &s->orig_termios) == 0)
    {
      s->termios_saved = true;
      raw = s->orig_termios;
      raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
      raw.c_oflag &= ~(OPOST);
      raw.c_cflag |= (CS8);
      raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
      raw.c_cc[VMIN]  = 1;
      raw.c_cc[VTIME] = 0;
      tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
}

/****************************************************************************
 * Name: ssh_restore_terminal
 ****************************************************************************/

static void ssh_restore_terminal(struct ssh_session_s *s)
{
  if (s->termios_saved)
    {
      tcsetattr(STDIN_FILENO, TCSANOW, &s->orig_termios);
      s->termios_saved = false;
    }
}

/****************************************************************************
 * Name: ssh_tcp_connect
 ****************************************************************************/

static int ssh_tcp_connect(struct ssh_session_s *s)
{
  struct addrinfo hints, *res;
  char port_str[8];
  int ret;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  snprintf(port_str, sizeof(port_str), "%d", s->port);

  if (s->verbose)
    {
      fprintf(stderr, "Resolving %s...\n", s->host);
    }

  ret = getaddrinfo(s->host, port_str, &hints, &res);
  if (ret != 0)
    {
      fprintf(stderr, "ssh: cannot resolve '%s'\n", s->host);
      return -1;
    }

  s->sock_fd = socket(res->ai_family, res->ai_socktype,
                       res->ai_protocol);
  if (s->sock_fd < 0)
    {
      fprintf(stderr, "ssh: socket: %s\n", strerror(errno));
      freeaddrinfo(res);
      return -1;
    }

  /* Enable TCP keep-alive */

  int ka = 1;
  setsockopt(s->sock_fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));

  if (connect(s->sock_fd, res->ai_addr, res->ai_addrlen) < 0)
    {
      fprintf(stderr, "ssh: connect to %s:%d failed: %s\n",
              s->host, s->port, strerror(errno));
      close(s->sock_fd);
      s->sock_fd = -1;
      freeaddrinfo(res);
      return -1;
    }

  freeaddrinfo(res);
  return 0;
}

/****************************************************************************
 * Name: ssh_connect
 ****************************************************************************/

static int ssh_connect(struct ssh_session_s *s)
{
  int ret;

  fprintf(stderr, "Connecting to %s@%s port %d...\n",
          s->user, s->host, s->port);

  ret = ssh_tcp_connect(s);
  if (ret < 0)
    {
      return ret;
    }

#ifdef CONFIG_CRYPTO_WOLFSSH
  /* --- wolfSSH handshake --- */

  s->ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
  if (s->ctx == NULL)
    {
      fprintf(stderr, "ssh: wolfSSH CTX creation failed\n");
      close(s->sock_fd);
      s->sock_fd = -1;
      return -1;
    }

  /* Load identity key if specified */

  if (s->keyfile[0] != '\0')
    {
      FILE *kf = fopen(s->keyfile, "rb");
      if (kf != NULL)
        {
          uint8_t keybuf[4096];
          size_t  keylen = fread(keybuf, 1, sizeof(keybuf), kf);
          fclose(kf);

          if (keylen > 0)
            {
              ret = wolfSSH_CTX_UsePrivateKey_buffer(s->ctx,
                      keybuf, (word32)keylen, WOLFSSH_FORMAT_ASN1);
              if (ret != WS_SUCCESS && s->verbose)
                {
                  fprintf(stderr, "ssh: key load warning: %d\n", ret);
                }
              else if (s->verbose)
                {
                  fprintf(stderr, "Loaded identity from %s\n", s->keyfile);
                }
            }
        }
      else
        {
          fprintf(stderr, "ssh: cannot open key: %s\n", s->keyfile);
        }
    }

  s->ssh = wolfSSH_new(s->ctx);
  if (s->ssh == NULL)
    {
      fprintf(stderr, "ssh: session creation failed\n");
      wolfSSH_CTX_free(s->ctx);
      s->ctx = NULL;
      close(s->sock_fd);
      s->sock_fd = -1;
      return -1;
    }

  wolfSSH_set_fd(s->ssh, s->sock_fd);
  wolfSSH_SetUsername(s->ssh, s->user);

  /* Prompt password if no key */

  if (s->keyfile[0] == '\0')
    {
      ssh_prompt_password(s);

      if (s->password[0] != '\0')
        {
          wolfSSH_SetUserAuth(s->ctx, NULL);  /* use default auth */
        }
    }

  if (s->verbose)
    {
      fprintf(stderr, "Starting SSH handshake...\n");
    }

  ret = wolfSSH_connect(s->ssh);
  if (ret != WS_SUCCESS)
    {
      fprintf(stderr, "ssh: handshake failed (error %d)\n",
              wolfSSH_get_error(s->ssh));
      wolfSSH_free(s->ssh);
      s->ssh = NULL;
      wolfSSH_CTX_free(s->ctx);
      s->ctx = NULL;
      close(s->sock_fd);
      s->sock_fd = -1;
      return -1;
    }

  fprintf(stderr, "Connected (SSH encrypted).\n");

#else
  /* Raw TCP fallback */

  fprintf(stderr, "Connected (raw TCP - no encryption).\n");
  fprintf(stderr, "WARNING: all data is sent in plain text.\n");
#endif

  return 0;
}

/****************************************************************************
 * Name: ssh_recv_thread
 ****************************************************************************/

static void *ssh_recv_thread(void *arg)
{
  struct ssh_session_s *s = (struct ssh_session_s *)arg;
  uint8_t buf[RECV_BUF_SIZE];
  int n;

  while (s->running)
    {
#ifdef CONFIG_CRYPTO_WOLFSSH
      if (s->ssh != NULL)
        {
          n = wolfSSH_stream_read(s->ssh, buf, sizeof(buf));
          if (n == WS_WANT_READ || n == WS_WANT_WRITE)
            {
              usleep(10000);
              continue;
            }
        }
      else
#endif
        {
          struct pollfd pfd;
          pfd.fd     = s->sock_fd;
          pfd.events = POLLIN;

          int pret = poll(&pfd, 1, 1000);
          if (pret <= 0)
            {
              continue;
            }

          n = recv(s->sock_fd, buf, sizeof(buf), 0);
        }

      if (n > 0)
        {
          write(STDOUT_FILENO, buf, n);
        }
      else if (n == 0)
        {
          fprintf(stderr, "\r\nConnection closed by remote host.\r\n");
          s->running = false;
          break;
        }
      else
        {
          if (errno == EINTR || errno == EAGAIN)
            {
              usleep(10000);
              continue;
            }

          fprintf(stderr, "\r\nConnection lost: %s\r\n",
                  strerror(errno));
          s->running = false;
          break;
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: ssh_send_data
 ****************************************************************************/

static int ssh_send_data(struct ssh_session_s *s,
                         const void *data, size_t len)
{
#ifdef CONFIG_CRYPTO_WOLFSSH
  if (s->ssh != NULL)
    {
      return wolfSSH_stream_send(s->ssh, (uint8_t *)data,
                                 (word32)len);
    }
#endif

  return send(s->sock_fd, data, len, 0);
}

/****************************************************************************
 * Name: ssh_disconnect
 ****************************************************************************/

static void ssh_disconnect(struct ssh_session_s *s)
{
#ifdef CONFIG_CRYPTO_WOLFSSH
  if (s->ssh != NULL)
    {
      wolfSSH_shutdown(s->ssh);
      wolfSSH_free(s->ssh);
      s->ssh = NULL;
    }

  if (s->ctx != NULL)
    {
      wolfSSH_CTX_free(s->ctx);
      s->ctx = NULL;
    }
#endif

  if (s->sock_fd >= 0)
    {
      close(s->sock_fd);
      s->sock_fd = -1;
    }
}

/****************************************************************************
 * Name: ssh_handle_escape
 *
 * Description:
 *   Returns: 0 = send char, 1 = filter, -1 = disconnect
 *
 ****************************************************************************/

static int ssh_handle_escape(char ch, bool *after_nl, bool *tilde)
{
  if (*tilde)
    {
      *tilde = false;
      switch (ch)
        {
          case '.':
            fprintf(stderr, "\r\nDisconnected.\r\n");
            return -1;

          case '?':
            fprintf(stderr,
              "\r\n  ~.  Disconnect\r\n"
              "  ~?  Help\r\n"
              "  ~~  Send ~\r\n\r\n");
            return 1;

          case '~':
            return 0;  /* send literal ~ */

          default:
            return 0;
        }
    }

  if (*after_nl && ch == '~')
    {
      *tilde = true;
      *after_nl = false;
      return 1;
    }

  *after_nl = (ch == '\r' || ch == '\n');
  return 0;
}

/****************************************************************************
 * Public Entry Point
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct ssh_session_s sess;
  pthread_t recv_tid;
  int ret;

#ifdef CONFIG_CRYPTO_WOLFSSH
  wolfSSH_Init();
#endif

  ret = ssh_parse_args(argc, argv, &sess);
  if (ret < 0)
    {
      return EXIT_FAILURE;
    }

  ret = ssh_connect(&sess);
  if (ret < 0)
    {
#ifdef CONFIG_CRYPTO_WOLFSSH
      wolfSSH_Cleanup();
#endif
      return EXIT_FAILURE;
    }

  /* Raw terminal mode */

  ssh_enter_raw_mode(&sess);
  sess.running = true;

  /* Start receiver thread */

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4096);
  pthread_create(&recv_tid, &attr, ssh_recv_thread, &sess);
  pthread_attr_destroy(&attr);

  fprintf(stderr, "Escape character is '~.'\r\n");

  /* Sender loop */

  char sendbuf[SEND_BUF_SIZE];
  uint8_t outbuf[SEND_BUF_SIZE];
  bool after_nl = true;
  bool tilde = false;

  while (sess.running)
    {
      struct pollfd pfd;
      pfd.fd     = STDIN_FILENO;
      pfd.events = POLLIN;

      ret = poll(&pfd, 1, 100);
      if (ret <= 0)
        {
          continue;
        }

      ssize_t n = read(STDIN_FILENO, sendbuf, sizeof(sendbuf));
      if (n <= 0)
        {
          fprintf(stderr, "\r\nDisconnecting...\r\n");
          break;
        }

      /* Process escape sequences */

      int outlen = 0;
      for (ssize_t i = 0; i < n; i++)
        {
          int esc = ssh_handle_escape(sendbuf[i], &after_nl, &tilde);
          if (esc < 0)
            {
              sess.running = false;
              goto done;
            }
          else if (esc == 0)
            {
              outbuf[outlen++] = sendbuf[i];
            }
        }

      if (outlen > 0)
        {
          ssh_send_data(&sess, outbuf, outlen);
        }
    }

done:
  sess.running = false;
  ssh_restore_terminal(&sess);
  pthread_join(recv_tid, NULL);
  ssh_disconnect(&sess);

#ifdef CONFIG_CRYPTO_WOLFSSH
  wolfSSH_Cleanup();
#endif

  return EXIT_SUCCESS;
}
