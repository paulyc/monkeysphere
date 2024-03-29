#define _GNU_SOURCE
#include <stdio.h>
#include <assuan.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pwd.h>
#include <gcrypt.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/select.h>

#include "ssh-agent-proto.h"

#define KEYGRIP_LENGTH 40
#define KEYWRAP_ALGO GCRY_CIPHER_AES128
#define KEYWRAP_ALGO_MODE GCRY_CIPHER_MODE_AESWRAP


int custom_log (assuan_context_t ctx, void *hook, unsigned int cat, const char *msg) {
  fprintf (stderr, "assuan (cat %d), %s\n", cat, msg);
  return 1;
}

/* Count octets required after trimming whitespace off the end of
   STRING and unescaping it.  Note that this will never be larger than
   strlen (STRING).  This count does not include any trailing null
   byte. */
static size_t
count_trimmed_unescaped (const char *string)
{
  size_t n = 0;
  size_t last_non_whitespace = 0;

  while (*string)
    {
      n++;
      if (*string == '%' &&
          string[1] && isxdigit(string[1]) &&
          string[2] && isxdigit(string[2]))
        {
          string++;
          string++;
        }
      else if (!isspace(*string))
        {
          last_non_whitespace = n;
        }
      string++;
    }

  return last_non_whitespace;
}

#define xtoi_1(p)   (*(p) <= '9'? (*(p)- '0'): \
                     *(p) <= 'F'? (*(p)-'A'+10):(*(p)-'a'+10))
#define xtoi_2(p)   ((xtoi_1(p) * 16) + xtoi_1((p)+1))

/* Trim whitespace off the right of STRING, unescape it, and return a
   malloc'ed buffer of the correct size.  returns NULL on failure */
static char *
trim_and_unescape (const char *string)
{
  size_t sz = count_trimmed_unescaped (string);
  char *p = malloc(sz+1);

  if (!p)
    return NULL;
    
  p[sz] = '\0';

  for (int i = 0; i < sz; i++)
    {
      if (*string == '%' &&
          string[1] && isxdigit(string[1]) &&
          string[2] && isxdigit(string[2]))
        {
          string++;
          p[i] = xtoi_2 (string);
          string++;
        }
      else
        p[i] = *string;
      string++;
    }

  return (p);
}

#ifdef PATH_MAX
#define BUFSIZE PATH_MAX
#else
#define BUFSIZE 4096
#endif

char* gpg_agent_sockname () {
  FILE *f;
  size_t bytecount, pos;
  char buf[BUFSIZE];
  int pipefd[2], wstatus;
  pid_t pid, waited = 0;

  if (pipe(pipefd)) {
    fprintf (stderr, "Could not pipe (%d) %s\n", errno, strerror (errno));
    return NULL;
  }
  pid = fork();
  if (pid == 0) {
    if (dup2 (pipefd[1], 1) == -1) {
      fprintf (stderr, "failed to dup2 (%d) %s", errno, strerror (errno));
      exit (1);
    }
    close (pipefd[0]);
    /* FIXME: should we close other open file descriptors? gpgconf is
       supposed to do that for us, but if we wanted to be defensive we
       might want to do it here too. */
    if (execlp ("gpgconf", "gpgconf", "--list-dirs", "agent-socket", NULL)) {
      fprintf (stderr, "failed to execl (%d) %s", errno, strerror (errno));
      exit (1);
    }
  }
  close (pipefd[1]);
  waited = waitpid (pid, &wstatus, 0);
  if (waited != pid) {
    fprintf (stderr, "waitpid failed (%d) %s\n", errno, strerror (errno));
    close (pipefd[0]);
    return NULL;
  }
  if (!WIFEXITED(wstatus)) {
    fprintf (stderr, "'gpgconf --list-dirs agent-socket' did not exit cleanly!\n");
    close (pipefd[0]);
    return NULL;
  }
  if (WEXITSTATUS(wstatus)) {
    fprintf (stderr, "'gpgconf --list-dirs agent-socket' exited with non-zero return code %d\n", WEXITSTATUS(wstatus));
    close (pipefd[0]);
    return NULL;
  }
  f = fdopen (pipefd[0], "r");
  if (f == NULL) {
    fprintf (stderr, "failed to get readable pipe (%d) %s\n", errno, strerror (errno));
    close (pipefd[0]);
    return NULL;
  }
  pos = 0;
  while (!feof(f))
    {
      bytecount = fread(buf + pos, 1, sizeof(buf) - pos, f);
      if (ferror(f)) {
        fclose (f);
        return NULL;
      }
      pos += bytecount;
      if (pos >= sizeof(buf)) {/* too much data! */
        fclose (f);
        return NULL;
      }
    }
  fclose (f);
  buf[pos] = '\0';
  return trim_and_unescape(buf);
}


typedef enum { kt_unknown = 0,
               kt_rsa,
               kt_ed25519
} key_type;

struct exporter {
  assuan_context_t ctx;
  gcry_cipher_hd_t wrap_cipher;
  unsigned char *wrapped_key;
  size_t wrapped_len;
  unsigned char *unwrapped_key;
  size_t unwrapped_len;
  key_type ktype;
  gcry_sexp_t sexp;
  gcry_mpi_t n;
  gcry_mpi_t e;
  gcry_mpi_t d;
  gcry_mpi_t p;
  gcry_mpi_t q;
  gcry_mpi_t iqmp;
  gcry_mpi_t curve;
  gcry_mpi_t flags;
};

/* percent_plus_escape is copyright Free Software Foundation */
/* taken from common/percent.c in gnupg */
/* Create a newly allocated string from STRING with all spaces and
   control characters converted to plus signs or %xx sequences.  The
   function returns the new string or NULL in case of a malloc
   failure.

   Note that we also escape the quote character to work around a bug
   in the mingw32 runtime which does not correctly handle command line
   quoting.  We correctly double the quote mark when calling a program
   (i.e. gpg-protect-tool), but the pre-main code does not notice the
   double quote as an escaped quote.  We do this also on POSIX systems
   for consistency.  */
char *
percent_plus_escape (const char *string)
{
  char *buffer, *p;
  const char *s;
  size_t length;

  for (length=1, s=string; *s; s++)
    {
      if (*s == '+' || *s == '\"' || *s == '%'
          || *(const unsigned char *)s < 0x20)
        length += 3;
      else
        length++;
    }

  buffer = p = malloc (length);
  if (!buffer)
    return NULL;

  for (s=string; *s; s++)
    {
      if (*s == '+' || *s == '\"' || *s == '%'
          || *(const unsigned char *)s < 0x20)
        {
          snprintf (p, 4, "%%%02X", *(unsigned char *)s);
          p += 3;
        }
      else if (*s == ' ')
        *p++ = '+';
      else
        *p++ = *s;
    }
  *p = 0;

  return buffer;

}



gpg_error_t extend_wrapped_key (struct exporter *e, const void *data, size_t data_sz) {
  size_t newsz = e->wrapped_len + data_sz;
  unsigned char *wknew = realloc (e->wrapped_key, newsz);
  if (!wknew)
    return GPG_ERR_ENOMEM;
  memcpy (wknew + e->wrapped_len, data, data_sz);
  e->wrapped_key = wknew;
  e->wrapped_len = newsz;
  return GPG_ERR_NO_ERROR;
}


gpg_error_t unwrap_rsa_key (struct exporter *e) {
  gpg_error_t ret;
  e->iqmp = gcry_mpi_new(0);
  ret = gcry_mpi_invm (e->iqmp, e->q, e->p);

  if (!ret) {
    fprintf (stderr, "Could not calculate the (inverse of q) mod p\n");
    return GPG_ERR_GENERAL;
  } else {
    e->ktype = kt_rsa;
    return GPG_ERR_NO_ERROR;
  }
}


gpg_error_t unwrap_ed25519_key (struct exporter *e) {
  unsigned int sz;
  const char * data;

#define opaque_compare(val, str, err)  {   \
    data = gcry_mpi_get_opaque (val, &sz); \
    if ((sz != strlen (str)*8) || !data || \
        memcmp (data, str, strlen(str)))   \
      return gpg_error (err); }
  
  /* verify that curve matches "Ed25519" */
  opaque_compare (e->curve, "Ed25519", GPG_ERR_UNKNOWN_CURVE);

  /* verify that flags contains "eddsa" */
  /* FIXME: what if there are other flags besides eddsa? */
  opaque_compare (e->flags, "eddsa", GPG_ERR_UNKNOWN_FLAG);
  
  /* verify that q starts with 0x40 and is 33 octets long */
  data = gcry_mpi_get_opaque (e->q, &sz);
  if (sz != 33*8 || !data || data[0] != 0x40)
    return gpg_error (GPG_ERR_INV_CURVE);
    /* verify that d is 32 octets long */
  data = gcry_mpi_get_opaque (e->d, &sz);
  if (sz < 32*8)
    return gpg_error (GPG_ERR_TOO_SHORT);
  if (sz > 32*8)
    return gpg_error (GPG_ERR_TOO_LARGE);
  if (!data)
    return gpg_error (GPG_ERR_NO_OBJ);
  
  e->ktype = kt_ed25519;
  return GPG_ERR_NO_ERROR;
}


gpg_error_t unwrap_key (struct exporter *e) {
  unsigned char *out = NULL;
  gpg_error_t ret;
  const size_t sz_diff = 8;
  /* need 8 octets less:

     'GCRY_CIPHER_MODE_AESWRAP'
     This mode is used to implement the AES-Wrap algorithm according to
     RFC-3394.  It may be used with any 128 bit block length algorithm,
     however the specs require one of the 3 AES algorithms.  These
     special conditions apply: If 'gcry_cipher_setiv' has not been used
     the standard IV is used; if it has been used the lower 64 bit of
     the IV are used as the Alternative Initial Value.  On encryption
     the provided output buffer must be 64 bit (8 byte) larger than the
     input buffer; in-place encryption is still allowed.  On decryption
     the output buffer may be specified 64 bit (8 byte) shorter than
     then input buffer.  As per specs the input length must be at least
     128 bits and the length must be a multiple of 64 bits. */

  if ((e->ctx == NULL) ||
      (e->wrap_cipher == NULL) ||
      (e->wrapped_key == NULL) ||
      (e->wrapped_len < 1))
    return GPG_ERR_GENERAL; /* this exporter is not in the right state */


  out = realloc (e->unwrapped_key, e->wrapped_len - sz_diff);
  if (!out)
    return GPG_ERR_ENOMEM;
  e->unwrapped_key = out;
  e->unwrapped_len = e->wrapped_len - sz_diff;

  ret = gcry_cipher_decrypt (e->wrap_cipher,
                             e->unwrapped_key, e->unwrapped_len,
                             e->wrapped_key, e->wrapped_len);

  if (ret)
    return ret;
  ret = gcry_sexp_new(&e->sexp, e->unwrapped_key, e->unwrapped_len, 0);
  if (ret)
    return ret;

  /* RSA has: n, e, d, p, q */
  ret = gcry_sexp_extract_param (e->sexp, "private-key!rsa", "nedpq",
                                 &e->n, &e->e, &e->d, &e->p, &e->q, NULL);
  if (!ret)
    return unwrap_rsa_key (e);
  
  if (gpg_err_code (ret) == GPG_ERR_NOT_FOUND) {
    /* check whether it's ed25519 */
    /* EdDSA has: curve, flags, q, d */
    ret = gcry_sexp_extract_param (e->sexp, "private-key!ecc", "/'curve''flags'qd",
                                   &e->curve, &e->flags, &e->q, &e->d, NULL);
    if (!ret)
      return unwrap_ed25519_key (e);
  }
  return ret;
}

gpg_error_t data_cb (void *arg, const void *data, size_t data_sz) {
  struct exporter *e = (struct exporter*)arg;
  gpg_error_t ret;

  if (e->wrap_cipher == NULL) {
    size_t cipher_keylen = gcry_cipher_get_algo_keylen(KEYWRAP_ALGO);
    if (data_sz != cipher_keylen) {
      fprintf (stderr, "wrong number of bytes in keywrap key (expected %zu, got %zu)\n",
               cipher_keylen, data_sz);
      return GPG_ERR_INV_KEYLEN;
    }
    ret = gcry_cipher_open (&(e->wrap_cipher), KEYWRAP_ALGO, KEYWRAP_ALGO_MODE, 0);
    if (ret)
      return ret;
    ret = gcry_cipher_setkey (e->wrap_cipher, data, data_sz);
    if (ret)
      return ret;
  } else {
    return extend_wrapped_key (e, data, data_sz);
  }
  return 0;
}
gpg_error_t inquire_cb (void *arg, const char *prompt) {
  fprintf (stderr, "inquire: %s\n", prompt);
  return 0;
}
gpg_error_t status_cb (void *arg, const char *status) {
  fprintf (stderr, "status: %s\n", status);
  return 0;
}

gpg_error_t transact (struct exporter *e, const char *command) {
  return assuan_transact (e->ctx, command, data_cb, e, inquire_cb, e, status_cb, e);
}


gpg_error_t sendenv (struct exporter *e, const char *env, const char *val, const char *option_name) {
  char *str = NULL;
  gpg_error_t ret;
  int r;
  if (!val)
    val = getenv(env);

  /* skip env vars that are unset */
  if (!val)
    return GPG_ERR_NO_ERROR;
  if (option_name)
    r = asprintf (&str, "OPTION %s=%s", option_name, val);
  else
    r = asprintf (&str, "OPTION putenv=%s=%s", env, val);

  if (r <= 0)
    return GPG_ERR_ENOMEM;
  ret = transact (e, str);
  free (str);
  return ret;
}

size_t get_ssh_sz (gcry_mpi_t mpi) {
  size_t wid;
  gcry_mpi_print (GCRYMPI_FMT_SSH, NULL, 0, &wid, mpi);
  return wid;
}

int send_to_ssh_agent(struct exporter *e, int fd, unsigned int seconds, int confirm, const char *comment) {
  const char *key_type;
  int ret;
  size_t len, mpilen;
  off_t offset;
  unsigned char *msgbuf = NULL;
  uint32_t tmp;
  size_t slen;
  ssize_t written, bytesread;
  unsigned char resp;

  if (e->ktype != kt_rsa && e->ktype != kt_ed25519) {
    fprintf (stderr, "key is neither RSA nor Ed25519, cannot handle it.\n");
    return -1;
  }

  if (e->ktype == kt_rsa) {
    key_type = "ssh-rsa";
    mpilen = get_ssh_sz (e->n) +
      get_ssh_sz (e->e) +
      get_ssh_sz (e->d) +
      get_ssh_sz (e->iqmp) +
      get_ssh_sz (e->p) +
      get_ssh_sz (e->q);
  } else if (e->ktype == kt_ed25519) {
    key_type = "ssh-ed25519";
    mpilen = 4 + 32 + /* ENC(A) */
      4 + 64; /* k || ENC(A) */
  }

  len = 1 + /* request byte */
    4 + strlen(key_type) + /* type of key */
    mpilen +
    4 + (comment ? strlen (comment) : 0) +
    (confirm ? 1 : 0) +
    (seconds ? 5 : 0);

  msgbuf = malloc (4 + len);
  if (msgbuf == NULL) {
    fprintf (stderr, "could not allocate %zu bytes for the message to ssh-agent\n", 4 + len);
    return -1;
  }

#define w32(a) { tmp = htonl(a); memcpy(msgbuf + offset, &tmp, sizeof(tmp)); offset += sizeof(tmp); }
#define wstr(a) { slen = (a ? strlen (a) : 0); w32 (slen); if (a) memcpy (msgbuf + offset, a, slen); offset += slen; }
#define wbyte(x) { msgbuf[offset] = (x); offset += 1; }
#define wmpi(n) { ret = gcry_mpi_print (GCRYMPI_FMT_SSH, msgbuf + offset, get_ssh_sz (n), &slen, n); \
    if (ret) { fprintf (stderr, "failed writing ssh mpi " #n "\n"); free (msgbuf); return -1; }; offset += slen; }

  offset = 0;
  
  w32 (len);
  wbyte (seconds || confirm ? SSH2_AGENTC_ADD_ID_CONSTRAINED : SSH2_AGENTC_ADD_IDENTITY);
  wstr (key_type);

  if (e->ktype == kt_rsa) {
    wmpi (e->n);
    wmpi (e->e);
    wmpi (e->d);
    wmpi (e->iqmp);
    wmpi (e->p);
    wmpi (e->q);
  } else if (e->ktype == kt_ed25519) {
    unsigned int dsz, qsz;
    const char *ddata, *qdata;
    qdata = gcry_mpi_get_opaque (e->q, &qsz);
    ddata = gcry_mpi_get_opaque (e->d, &dsz);
    if (qsz != 33*8 || dsz != 32*8 || !qdata || !ddata) {
      fprintf (stderr, "Ed25519 key did not have the expected components (q: %d %p, d: %d %p)\n",
               qsz, qdata, dsz, ddata);
      return -1;
    }

    /* ENC(A) (aka q)*/
    w32 (32);
    memcpy (msgbuf + offset, qdata+1, 32); offset += 32;
    /* k || ENC(A) (aka d || q) */
    w32 (64);
    memcpy (msgbuf + offset, ddata, 32); offset += 32;
    memcpy (msgbuf + offset, qdata+1, 32); offset += 32;
  }
  wstr (comment);
  if (confirm)
    wbyte (SSH_AGENT_CONSTRAIN_CONFIRM);
  if (seconds) {
    wbyte (SSH_AGENT_CONSTRAIN_LIFETIME);
    w32 (seconds);
  }
  written = write (fd, msgbuf, 4+len);
  if (written != 4 + len) {
    fprintf (stderr, "failed writing message to ssh agent socket (%zd) (errno: %d)\n", written, errno);
    free (msgbuf);
    return -1;
  }
  free (msgbuf);

  /* FIXME: this could actually be done in a select loop if we think the
     ssh-agent will dribble out its response or not respond immediately.*/
  bytesread = read (fd, &tmp, sizeof (tmp));
  if (bytesread != sizeof (tmp)) {
    fprintf (stderr, "failed to get %zu bytes from ssh-agent (got %zd)\n", sizeof (tmp), bytesread);
    return -1;
  }
  slen = ntohl (tmp);
  if (slen != sizeof(resp)) {
    fprintf (stderr, "ssh-agent response was wrong size (expected: %zu; got %zu)\n", sizeof(resp), slen);
    return -1;
  }
  bytesread = read (fd, &resp, sizeof (resp));
  if (bytesread != sizeof (resp)) {
    fprintf (stderr, "failed to get %zu bytes from ssh-agent (got %zd)\n", sizeof (resp), bytesread);
    return -1;
  }
  if (resp != SSH_AGENT_SUCCESS) {
    fprintf (stderr, "ssh-agent did not claim success (expected: %d; got %d)\n",
             SSH_AGENT_SUCCESS, resp);
    return -1;
  }    
    
  return 0;
}

void free_exporter (struct exporter *e) {
  assuan_release (e->ctx);
  if (e->wrap_cipher)
    gcry_cipher_close (e->wrap_cipher);
  free (e->wrapped_key);
  free (e->unwrapped_key);
  gcry_mpi_release(e->n);
  gcry_mpi_release(e->d);
  gcry_mpi_release(e->e);
  gcry_mpi_release(e->p);
  gcry_mpi_release(e->q);
  gcry_mpi_release(e->iqmp);
  gcry_mpi_release(e->curve);
  gcry_mpi_release(e->flags);
  gcry_sexp_release (e->sexp);
}

void usage (FILE *f) {
  fprintf (f, "Usage: agent-transfer [options] KEYGRIP [COMMENT]\n"
           "\n"
           "Extracts a secret key from the GnuPG agent (by keygrip),\n"
           "and sends it to the running SSH agent.\n"
           "\n"
           "  KEYGRIP should be a GnuPG keygrip\n"
           "    (e.g. try \"gpg --with-keygrip --list-secret-keys\")\n"
           "  COMMENT (optional) can be any string\n"
           "    (must not start with a \"-\")\n"
           "\n"
           "Options:\n"
           " -t SECONDS  lifetime (in seconds) for the key to live in ssh-agent\n"
           " -c          require confirmation when using the key in ssh-agent\n"
           " -h          print this help\n"
           );
}

int get_ssh_auth_sock_fd() {
  char *sock_name = getenv("SSH_AUTH_SOCK");
  struct sockaddr_un sockaddr;
  int ret = -1;
  if (sock_name == NULL) {
    fprintf (stderr, "SSH_AUTH_SOCK is not set, cannot talk to agent.\n");
    return -1;
  }
  if (strlen(sock_name) + 1 > sizeof(sockaddr.sun_path)) {
    fprintf (stderr, "SSH_AUTH_SOCK (%s) is larger than the maximum allowed socket path (%zu)\n",
             sock_name, sizeof(sockaddr.sun_path));
    return -1;
  }
  sockaddr.sun_family = AF_UNIX;
  strncpy(sockaddr.sun_path, sock_name, sizeof(sockaddr.sun_path) - 1);
  sockaddr.sun_path[sizeof(sockaddr.sun_path) - 1] = '\0';
  ret = socket (AF_UNIX, SOCK_STREAM, 0);
  if (ret == -1) {
    fprintf (stderr, "Could not open a socket file descriptor\n");
    return ret;
  }
  if (-1 == connect (ret, (const struct sockaddr*)(&sockaddr),
                     sizeof(sockaddr))) {
    fprintf (stderr, "Failed to connect to ssh agent socket %s\n", sock_name);
    close (ret);
    return -1;
  }

  return ret;
}

struct args {
  int seconds;
  int confirm;
  const char *comment;
  const char *keygrip;
  int help;
};

int parse_args (int argc, const char **argv, struct args *args) {
  int ptr = 1;
  int idx = 0;

  while (ptr < argc) {
    if (argv[ptr][0] == '-') {
      int looking_for_seconds = 0;
      const char *x = argv[ptr] + 1;
      while (*x != '\0') {
        switch (*x) {
        case 'c':
          args->confirm = 1;
          break;
        case 't':
          looking_for_seconds = 1;
          break;
        case 'h':
          args->help = 1;
          break;
        default:
          fprintf (stderr, "flag not recognized: %c\n", *x);
          return 1;
        }
        x++;
      }
      if (looking_for_seconds) {
        if (argc <= ptr + 1) {
          fprintf (stderr, "lifetime (-t) needs an argument (number of seconds)\n");
          return 1;
        }
        args->seconds = atoi (argv[ptr + 1]);
        if (args->seconds <= 0) {
          fprintf (stderr, "lifetime (seconds) must be > 0\n");
          return 1;
        }
        ptr += 1;
      }
    } else {
      if (args->keygrip == NULL) {
        if (strlen (argv[ptr]) != KEYGRIP_LENGTH) {
          fprintf (stderr, "keygrip must be 40 hexadecimal digits\n");
          return 1;
        }
        
        for (idx = 0; idx < KEYGRIP_LENGTH; idx++) {
          if (!isxdigit(argv[ptr][idx])) {
            fprintf (stderr, "keygrip must be 40 hexadecimal digits\n");
            return 1;
          }
        }
        args->keygrip = argv[ptr];
      } else {
        if (args->comment == NULL) {
          args->comment = argv[ptr];
        } else {
          fprintf (stderr, "unrecognized argument %s\n", argv[ptr]);
          return 1;
        }
      }
    }
    ptr += 1;
  };
  
  return 0;
}

int main (int argc, const char* argv[]) {
  gpg_error_t err;
  char *gpg_agent_socket = NULL;
  int ssh_sock_fd = 0;
  char *get_key = NULL, *desc_prompt = NULL;
  int idx = 0, ret = 0;
  struct exporter e = { .wrapped_key = NULL };
  /* ssh agent constraints: */
  struct args args = { .keygrip = NULL };
  char *escaped_comment = NULL;
  char *alt_comment = NULL;
  
  if (!gcry_check_version (GCRYPT_VERSION)) {
    fprintf (stderr, "libgcrypt version mismatch\n");
    return 1;
  }
  gcry_control (GCRYCTL_DISABLE_SECMEM, 0);
  gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
  
  if (parse_args(argc, argv, &args)) {
    usage (stderr);
    return 1;
  }

  if (args.help) {
    usage (stdout);
    return 0;
  }

  if (asprintf (&get_key, "EXPORT_KEY %s", args.keygrip) < 0) {
    fprintf (stderr, "failed to generate key export string\n");
    return 1;
  }

  if (args.comment &&
      (escaped_comment = percent_plus_escape (args.comment), escaped_comment)) {
    ret = asprintf (&desc_prompt,
                    "SETKEYDESC Sending+key+for+'%s'+"
                    "from+gpg-agent+to+ssh-agent...%%0a"
                    "(keygrip:+%s)", escaped_comment, args.keygrip);
    free (escaped_comment);
  } else {
    ret = asprintf (&desc_prompt,
                    "SETKEYDESC Sending+key+from+gpg-agent+to+ssh-agent...%%0a"
                    "(keygrip:+%s)", args.keygrip);
  }
  
  if (ret < 0) {
    fprintf (stderr, "failed to generate prompt description\n");
    return 1;
  }

  ssh_sock_fd = get_ssh_auth_sock_fd();
  if (ssh_sock_fd == -1)
    return 1;
  
  err = assuan_new (&(e.ctx));
  if (err) {
    fprintf (stderr, "failed to create assuan context (%d) (%s)\n", err, gpg_strerror (err));
    return 1;
  }
  gpg_agent_socket = gpg_agent_sockname();
  if (gpg_agent_socket == NULL) {
    fprintf (stderr, "failed to get gpg-agent socket name!\n");
    return 1;
  }
  
  /* launch gpg-agent if it is not already connected */
  err = assuan_socket_connect (e.ctx, gpg_agent_socket,
                               ASSUAN_INVALID_PID, ASSUAN_SOCKET_CONNECT_FDPASSING);
  if (err) {
    if (gpg_err_code (err) != GPG_ERR_ASS_CONNECT_FAILED) {
      fprintf (stderr, "failed to connect to gpg-agent socket (%d) (%s)\n",
               err, gpg_strerror (err));
    } else {
      fprintf (stderr, "could not find gpg-agent, trying to launch it...\n");
      int r = system ("gpgconf --launch gpg-agent");
      if (r) {
        fprintf (stderr, "failed to launch gpg-agent\n");
        return 1;
      }
      /* try to connect again: */
      err = assuan_socket_connect (e.ctx, gpg_agent_socket,
                               ASSUAN_INVALID_PID, ASSUAN_SOCKET_CONNECT_FDPASSING);
      if (err) {
        fprintf (stderr, "failed to connect to gpg-agent after launching (%d) (%s)\n",
                 err, gpg_strerror (err));
        return 1;
      }
    }
  }

  /* FIXME: what do we do if "getinfo std_env_names" includes something new? */
  struct { const char *env; const char *val; const char *opt; } vars[] = {
    { .env = "GPG_TTY", .val = ttyname(0), .opt = "ttyname" },
    { .env = "TERM", .opt = "ttytype" },
    { .env = "DISPLAY", .opt = "display" },
    { .env = "XAUTHORITY", .opt = "xauthority" },
    { .env = "GTK_IM_MODULE" },
    { .env = "DBUS_SESSION_BUS_ADDRESS" },
    { .env = "LANG", .opt = "lc-ctype" },
    { .env = "LANG", .opt = "lc-messages" } };
  for (idx = 0; idx < sizeof(vars)/sizeof(vars[0]); idx++) {
    if (err = sendenv (&e, vars[idx].env, vars[idx].val, vars[idx].opt), err) {
      fprintf (stderr, "failed to set %s (%s)\n", vars[idx].opt ? vars[idx].opt : vars[idx].env,
               gpg_strerror(err));
    }
  }
  err = transact (&e, "keywrap_key --export");
  if (err) {
    fprintf (stderr, "failed to export keywrap key (%d), %s\n", err, gpg_strerror(err));
    return 1;
  }
  err = transact (&e, desc_prompt);
  if (err) {
    fprintf (stderr, "failed to set the description prompt (%d), %s\n", err, gpg_strerror(err));
    return 1;
  }
  err = transact (&e, get_key);
  if (err) {
    fprintf (stderr, "failed to export secret key %s (%d), %s\n", args.keygrip, err, gpg_strerror(err));
    return 1;
  }
  err = unwrap_key (&e);
  if (err) {
    fprintf (stderr, "failed to unwrap secret key (%d), %s\n", err, gpg_strerror(err));
    return 1;
  }

  if (!args.comment) {
    int bytes_printed = asprintf (&alt_comment,
                                  "GnuPG keygrip %s",
                                  args.keygrip);
    if (bytes_printed < 0) {
      fprintf (stderr, "failed to generate key comment\n");
      return 1;
    }
  }
  
  err = send_to_ssh_agent (&e, ssh_sock_fd, args.seconds, args.confirm,
                           args.comment ? args.comment : alt_comment);
  if (err)
    return 1;
  
  /*  fwrite (e.unwrapped_key, e.unwrapped_len, 1, stdout); */

  close (ssh_sock_fd);
  free (gpg_agent_socket);
  free (get_key);
  free (desc_prompt);
  free (alt_comment);
  free_exporter (&e);
  return 0;
}
