/******************************************************************************\
 *  $Id: server-sock.c,v 1.13 2001/05/25 18:39:53 dun Exp $
 *    by Chris Dunlap <cdunlap@llnl.gov>
\******************************************************************************/


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "conman.h"
#include "errors.h"
#include "lex.h"
#include "server.h"
#include "util.h"


typedef struct request {
    int    sd;				/* client socket descriptor           */
    int    port;			/* port number in use by client host  */
    char  *ip;				/* ip addr string of client           */
    char  *host;			/* name of client host or ip addr str */
    char  *user;			/* name of client user                */
    cmd_t  command;			/* command to perform for client      */
    int    enableBroadcast;		/* true if b-casting to many consoles */
    int    enableForce;			/* true if forcing console connection */
    char  *program;			/* program name for EXECUTE cmd       */
    List   consoles;			/* list of consoles affected by cmd   */
} req_t;


static req_t * create_req(int sd);
static void destroy_req(req_t *req);
static int recv_greeting(req_t *req);
static int parse_greeting(Lex l, req_t *req);
static int recv_req(req_t *req);
static int parse_cmd_opts(Lex l, req_t *req);
static int query_consoles(server_conf_t *conf, req_t *req);
static int validate_req(req_t *req);
static int check_too_many_consoles(req_t *req);
static int check_busy_consoles(req_t *req);
static int send_rsp(req_t *req, int errnum, char *errmsg);
static void perform_query_cmd(req_t *req);
static void perform_monitor_cmd(req_t *req, server_conf_t *conf);
static void perform_connect_cmd(req_t *req, server_conf_t *conf);
static void perform_execute_cmd(req_t *req, server_conf_t *conf);


void process_client(client_arg_t *args)
{
/*  The thread responsible for accepting a client connection
 *    and processing the request.
 *  The QUERY cmd is processed entirely by this thread.
 *  The MONITOR and CONNECT cmds are setup and then placed
 *    in the conf->objs list to be handled by mux_io().
 *  The EXECUTE cmd still puzzles me somewhat...
 */
    int rc;
    int sd;
    server_conf_t *conf;
    req_t *req;

    assert(args);
    sd = args->sd;
    conf = args->conf;
    free(args);

    DPRINTF("Processing new client.\n");

    if ((rc = pthread_detach(pthread_self())) != 0)
        err_msg(rc, "pthread_detach() failed");

    if (!(req = create_req(sd))) {
        if (close(sd) < 0)
            err_msg(errno, "close(%d) failed", sd);
        return;
    }

    if (recv_greeting(req) < 0)
        goto err;
    if (recv_req(req) < 0)
        goto err;
    if (query_consoles(conf, req) < 0)
        goto err;
    if (validate_req(req) < 0)
        goto err;

    switch(req->command) {
    case CONNECT:
        perform_connect_cmd(req, conf);
        break;
    case EXECUTE:
        perform_execute_cmd(req, conf);
        break;
    case MONITOR:
        perform_monitor_cmd(req, conf);
        break;
    case QUERY:
        perform_query_cmd(req);
        break;
    default:
        /*  This should not happen, as invalid commands
         *    will be detected by validate_req().
         */
        log_msg(0, "INTERNAL ERROR: Received invalid command (%d) at %s:%d",
            req->command, __FILE__, __LINE__);
        break;
    }

    destroy_req(req);
    return;

err:
    /*  Only close the client's socket connection on error,
     *    since it may be further handled by mux_io().
     */
    if (req->sd > 0) {
        if (close(req->sd) < 0)
            log_msg(0, "Error closing connection from %s: %s",
                req->ip, strerror(errno));
        req->sd = -1;
    }
    destroy_req(req);
    return;
}


static req_t * create_req(int sd)
{
/*  Creates a request struct.
 *  Returns the new struct, or NULL or error.
 */
    req_t *req;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    char buf[MAX_LINE];

    assert(sd >= 0);

    if (getpeername(sd, &addr, &addrlen) < 0)
        err_msg(errno, "getpeername() failed");

    if (!inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)))
        err_msg(errno, "inet_ntop() failed");

    if (!(req = malloc(sizeof(req_t)))) {
        log_msg(0, "Request from %s terminated: out of memory", buf);
        return(NULL);
    }

    req->sd = sd;
    req->user = NULL;
    req->command = NONE;
    req->enableBroadcast = 0;
    req->enableForce = 0;
    req->program = NULL;
    /*
     *  The "consoles" list will initially contain strings received while
     *    parsing the client's request.  These strings will then be matched
     *    against the server's conf during query_consoles().
     *  The destroy_string() destructor is used here because the initial
     *    list of strings will be destroyed when query_consoles()
     *    replaces it with a list of console objects.
     */
    if (!(req->consoles = list_create((ListDelF) destroy_string)))
        goto err;

    req->port = ntohs(addr.sin_port);
    if (!(req->ip = strdup(buf)))
        goto err;

    /*  Attempt to resolve IP address.  If it succeeds, buf contains
     *    host string.  If it fails, buf is unchanged with IP addr string.
     *    Either way, copy buf to prevents having to code everything as
     *    (req->host ? req->host : req->ip).
     */
    get_hostname_via_addr(&addr.sin_addr, buf, sizeof(buf));
    if (!(req->host = strdup(buf)))
        goto err;

    return(req);

err:
    destroy_req(req);
    log_msg(0, "Request from %s terminated: out of memory", buf);
    return(NULL);
}


static void destroy_req(req_t *req)
{
/*  Destroys a request struct.
 *  The client's socket connection is not shutdown here
 *    since it may be further handled by mux_io().
 */
    DPRINTF("Destroyed request from %s.\n", req->host);

    if (req->ip)
        free(req->ip);
    if (req->host)
        free(req->host);
    if (req->user)
        free(req->user);
    if (req->program)
        free(req->program);
    if (req->consoles)
        list_destroy(req->consoles);
    free(req);
    return;
}


static int recv_greeting(req_t *req)
{
/*  Performs the initial handshake with the client
 *    (SOMEDAY including authentication & encryption, if needed).
 *  Returns 0 if the greeting is valid, or -1 on error.
 */
    int n;
    char buf[MAX_SOCK_LINE];
    Lex l;
    int done = 0;
    int tok;

    assert(req->sd >= 0);

    /*  Read greeting (ie, first line of request):
     *    HELLO USER='<str>'
     */
    if ((n = read_line(req->sd, buf, sizeof(buf))) < 0) {
        log_msg(0, "Error reading greeting from %s: %s",
            req->ip, strerror(errno));
        return(-1);
    }
    else if (n == 0) {
        log_msg(0, "Connection terminated by %s", req->ip);
        return(-1);
    }

    DPRINTF("Received greeting: %s", buf);

    if (!(l = lex_create(buf, proto_strs))) {
        send_rsp(req, CONMAN_ERR_NO_RESOURCES,
            "Insufficient resources to process request.");
        return(-1);
    }
    while (!done) {
        tok = lex_next(l);
        switch(tok) {
        case CONMAN_TOK_HELLO:
            done = parse_greeting(l, req);
            break;
        case LEX_EOF:
        case LEX_EOL:
            done = 1;
            break;
        default:
            break;
        }
    }
    lex_destroy(l);

    /*  Validate greeting.
     */
    if (done == -1) {
        send_rsp(req, CONMAN_ERR_NO_RESOURCES,
            "Insufficient resources to process request.");
        return(-1);
    }
    if (!req->user) {
        send_rsp(req, CONMAN_ERR_BAD_REQUEST, 
            "Invalid greeting: no user specified");
        return(-1);
    }
#ifdef NDEBUG
    if (strcmp(req->ip, "127.0.0.1")) {
        send_rsp(req, CONMAN_ERR_AUTHENTICATE,
            "Authentication required (but not yet implemented)");
        return(-1);
    }
#endif /* NDEBUG */

    DPRINTF("Received request from <%s@%s>.\n", req->user, req->host);

    /*  Send response to greeting.
     */
    return(send_rsp(req, CONMAN_ERR_NONE, NULL));
}


static int parse_greeting(Lex l, req_t *req)
{
/*  Parses the "HELLO" command from the client:
 *    HELLO USER='<str>'
 *  Returns 0 on success, or -1 on error (ie, out of memory).
 */
    int done = 0;
    int tok;

    while (!done) {
        tok = lex_next(l);
        switch(tok) {
        case CONMAN_TOK_USER:
            if ((lex_next(l) == '=') && (lex_next(l) == LEX_STR)
              && (*lex_text(l) != '\0')) {
                if (req->user)
                    free(req->user);
                if (!(req->user = lex_decode(strdup(lex_text(l)))))
                    return(-1);
            }
            break;
        case LEX_EOF:
        case LEX_EOL:
            done = 1;
            break;
        default:
            break;
        }
    }
    return(0);
}


static int recv_req(req_t *req)
{
/*  Receives the request from the client after the greeting has completed.
 *  Returns 0 if the request is read OK, or -1 on error.
 */
    int n;
    char buf[MAX_SOCK_LINE];
    Lex l;
    int done = 0;
    int tok;

    assert(req->sd >= 0);

    if ((n = read_line(req->sd, buf, sizeof(buf))) < 0) {
        log_msg(0, "Error reading request from %s: %s",
            req->ip, strerror(errno));
        return(-1);
    }
    else if (n == 0) {
        log_msg(0, "Connection terminated by %s", req->ip);
        return(-1);
    }

    DPRINTF("Received request: %s", buf);

    if (!(l = lex_create(buf, proto_strs))) {
        send_rsp(req, CONMAN_ERR_NO_RESOURCES,
            "Insufficient resources to process request.");
        return(-1);
    }
    while (!done) {
        tok = lex_next(l);
        switch(tok) {
        case CONMAN_TOK_CONNECT:
            req->command = CONNECT;
            done = parse_cmd_opts(l, req);
            break;
        case CONMAN_TOK_EXECUTE:
            req->command = EXECUTE;
            done = parse_cmd_opts(l, req);
            break;
        case CONMAN_TOK_MONITOR:
            req->command = MONITOR;
            done = parse_cmd_opts(l, req);
            break;
        case CONMAN_TOK_QUERY:
            req->command = QUERY;
            done = parse_cmd_opts(l, req);
            break;
        case LEX_EOF:
        case LEX_EOL:
            done = 1;
            break;
        default:
            break;
        }
    }
    lex_destroy(l);

    if (done == -1) {
        send_rsp(req, CONMAN_ERR_NO_RESOURCES,
            "Insufficient resources to process request.");
        return(-1);
    }
    return(0);
}


static int parse_cmd_opts(Lex l, req_t *req)
{
/*  Parses the command options for the given request.
 *  Returns 0 on success, or -1 on error (ie, out of memory).
 */
    int done = 0;
    int tok;
    char *str;

    while (!done) {
        tok = lex_next(l);
        switch(tok) {
        case CONMAN_TOK_CONSOLE:
            if ((lex_next(l) == '=') && (lex_next(l) == LEX_STR)
              && (*lex_text(l) != '\0')) {
                if (!(str = lex_decode(strdup(lex_text(l)))))
                    return(-1);
                if (!list_append(req->consoles, str)) {
                    free(str);
                    return(-1);
                }
            }
            break;
        case CONMAN_TOK_OPTION:
            if (lex_next(l) == '=') {
                if (lex_next(l) == CONMAN_TOK_FORCE)
                    req->enableForce = 1;
                else if (lex_prev(l) == CONMAN_TOK_BROADCAST)
                    req->enableBroadcast = 1;
            }
            break;
        case CONMAN_TOK_PROGRAM:
            if ((lex_next(l) == '=') && (lex_next(l) == LEX_STR)
              && (*lex_text(l) != '\0')) {
                if (req->program)
                    free(req->program);
                if (!(req->program = lex_decode(strdup(lex_text(l)))))
                    return(-1);
            }
            break;
        case LEX_EOF:
        case LEX_EOL:
            done = 1;
            break;
        default:
            break;
        }
    }
    return(0);
}


static int query_consoles(server_conf_t *conf, req_t *req)
{
/*  Queries the server's conf to resolve the console names
 *    specified in the client's request.
 *  The req->consoles list initially contains strings constructed
 *    while parsing the client's request in parse_command().
 *    These strings are combined into a regex pattern and then
 *    matched against the console objs in the conf->objs list.
 *  Returns 0 on success, or -1 on error.
 *    Upon a successful return, the req->consoles list is replaced
 *    with a list of console obj_t's.
 */
    ListIterator i;
    char buf[MAX_BUF_SIZE];
    char *p;
    int rc;
    regex_t rex;
    regmatch_t match;
    List matches;
    obj_t *obj;

    /*  An empty list for the QUERY command matches all consoles.
     */
    if (list_is_empty(req->consoles)) {
        if (req->command != QUERY)
            return(0);
        if (!(p = strdup(".*")))
            goto no_mem;
        if (!list_append(req->consoles, p)) {
            free(p);
            goto no_mem;
        }
    }

    /*  Combine console patterns via alternation to create single regex.
     */
    if (!(i = list_iterator_create(req->consoles)))
        goto no_mem;
    strlcpy(buf, list_next(i), sizeof(buf));
    while ((p = list_next(i))) {
        strlcat(buf, "|", sizeof(buf));
        strlcat(buf, p, sizeof(buf));
    }
    list_iterator_destroy(i);

    /*  Compile regex for searching server's console objs.
     */
    rc = regcomp(&rex, buf, REG_EXTENDED | REG_ICASE);
    if (rc != 0) {
        if (regerror(rc, &rex, buf, sizeof(buf)) > sizeof(buf))
            log_msg(10, "Buffer overflow during regerror()");
        regfree(&rex);
        send_rsp(req, CONMAN_ERR_BAD_REGEX, buf);
        return(-1);
    }

    /*  Search objs for consoles matching the regex.
     *  The NULL destructor is used here because the matches list will
     *    only contain references to objs contained in the conf->objs list.
     *    These objs will be destroyed when the conf->objs list is destroyed.
     */
    if (!(matches = list_create(NULL))) {
        regfree(&rex);
        goto no_mem;
    }
    if (!(i = list_iterator_create(conf->objs))) {
        list_destroy(matches);
        regfree(&rex);
        goto no_mem;
    }
    while ((obj = list_next(i))) {
        if (obj->type != CONSOLE)
            continue;
        if (!regexec(&rex, obj->name, 1, &match, 0)
          && (match.rm_so == 0)
          && (match.rm_eo == strlen(obj->name))) {
            if (!list_append(matches, obj)) {
                list_iterator_destroy(i);
                list_destroy(matches);
                regfree(&rex);
                goto no_mem;
            }
        }
    }
    list_iterator_destroy(i);
    regfree(&rex);

    /*  Sort the resulting obj_t list.
     */
    list_sort(matches, (ListCmpF) compare_objs);

    /*  Replace original consoles-string list with regex-matched obj_t list.
     */
    list_destroy(req->consoles);
    req->consoles = matches;

    return(0);

no_mem:
    send_rsp(req, CONMAN_ERR_NO_RESOURCES,
        "Insufficient resources to process request.");
    return(-1);
}


static int validate_req(req_t *req)
{
/*  Validates the given request.
 *  Returns 0 if the request is valid, or -1 on error.
 */
    if (list_is_empty(req->consoles)) {
        send_rsp(req, CONMAN_ERR_NO_CONSOLES, "Found no matching consoles.");
        return(-1);
    }
    if (check_too_many_consoles(req) < 0)
        return(-1);
    if (check_busy_consoles(req) < 0)
        return(-1);

    return(0);
}


static int check_too_many_consoles(req_t *req)
{
/*  Checks to see if the request matches too many consoles
 *    for the given command.
 *  A MONITOR command can only affect a single console, as can a
 *    CONNECT or EXECUTE command unless the broadcast option is enabled.
 *  Returns 0 if the request is valid, or -1 on error.
 */
    ListIterator i;
    obj_t *obj;
    char buf[MAX_SOCK_LINE];

    assert(!list_is_empty(req->consoles));

    if (req->command == QUERY)
        return(0);
    if (list_count(req->consoles) == 1)
        return(0);
    if (((req->command == CONNECT) || (req->command == EXECUTE))
      && (req->enableBroadcast))
        return(0);

    if (!(i = list_iterator_create(req->consoles))) {
        send_rsp(req, CONMAN_ERR_NO_RESOURCES,
            "Insufficient resources to process request.");
        return(-1);
    }
    snprintf(buf, sizeof(buf), "Found %d matching consoles.",
        list_count(req->consoles));
    send_rsp(req, CONMAN_ERR_TOO_MANY_CONSOLES, buf);

    /*  FIX_ME? Replace with single write_n()?
     */
    while ((obj = list_next(i))) {
        strlcpy(buf, obj->name, sizeof(buf));
        strlcat(buf, "\n", sizeof(buf));
        if (write_n(req->sd, buf, strlen(buf)) < 0) {
            log_msg(0, "Error writing to %s: %s", req->ip, strerror(errno));
            break;
        }
    }
    list_iterator_destroy(i);
    return(-1);
}


static int check_busy_consoles(req_t *req)
{
/*  Checks to see if a "writable" request affects any consoles
 *    that are currently busy (unless the force option is enabled).
 *  Returns 0 if the request is valid, or -1 on error.
 */
    List busy = NULL;
    ListIterator i = NULL;
    obj_t *obj;
    char buf[MAX_SOCK_LINE];
    int rc;
    time_t t;
    char *delta;

    assert(!list_is_empty(req->consoles));

    if ((req->command == QUERY) || (req->command == MONITOR))
        return(0);
    if (req->enableForce)
        return(0);

    if (!(busy = list_create(NULL)))
        goto err;
    if (!(i = list_iterator_create(req->consoles)))
        goto err;
    while ((obj = list_next(i))) {
        if (obj->writer != NULL)
            if (!list_append(busy, obj))
                goto err;
    }
    list_iterator_destroy(i);
    i = NULL;

    if (list_is_empty(busy)) {
        list_destroy(busy);
        return(0);
    }

    snprintf(buf, sizeof(buf), "Found %d console%s already in use.",
        list_count(busy), (list_count(busy) == 1) ? "" : "s");
    send_rsp(req, CONMAN_ERR_BUSY_CONSOLES, buf);

    /*  Note: the "busy" list contains object references,
     *    so they DO NOT get destroyed here when removed from the list.
     */
    while ((obj = list_pop(busy))) {

        assert(obj->type == CONSOLE);
        assert(obj->writer->type == SOCKET);

        if ((rc = pthread_mutex_lock(&obj->writer->bufLock)) != 0)
            err_msg(rc, "pthread_mutex_lock() failed for [%s]",
                obj->writer->name);
        t = obj->writer->aux.socket.timeLastRead;
        if ((rc = pthread_mutex_unlock(&obj->writer->bufLock)) != 0)
            err_msg(rc, "pthread_mutex_unlock() failed for [%s]",
                obj->writer->name);

        delta = create_time_delta_string(t);
        /*
         *  FIX_ME: Inconsistent use of socket name as <%s>?
         */
        snprintf(buf, sizeof(buf), "Console [%s] in use by <%s> (idle %s).\n",
            obj->name, obj->writer->name, (delta ? delta : "???"));
        if (delta)
            free(delta);

        if (write_n(req->sd, buf, strlen(buf)) < 0) {
            log_msg(0, "Error writing to %s: %s", req->ip, strerror(errno));
            break;
        }
    }
    list_destroy(busy);
    return(-1);

err:
    if (i)
        list_iterator_destroy(i);
    if (busy)
        list_destroy(busy);
    send_rsp(req, CONMAN_ERR_NO_RESOURCES,
        "Insufficient resources to process request.");
    return(-1);
}


static int send_rsp(req_t *req, int errnum, char *errmsg)
{
/*  Sends a response to the given request (req).
 *  If the request is valid and there are no errors,
 *    errnum = CONMAN_ERR_NONE and an "OK" response is sent.
 *  Otherwise, (errnum) identifies the err_type enumeration (in conman.h)
 *    and (errmsg) is a string describing the error in more detail.
 *  Returns 0 if the response is sent OK, or -1 on error.
 */
    char buf[MAX_SOCK_LINE];
    char tmp[MAX_LINE];			/* tmp buffer for lex-encoding strs */
    int n;

    assert(req->sd >= 0);
    assert(errnum >= 0);

    if (errnum == CONMAN_ERR_NONE) {

        char *ptr = buf;
        int len = sizeof(buf) - 2;	/* reserve space for trailing "\n\0" */
        ListIterator i;
        obj_t *console;

        n = snprintf(buf, sizeof(buf), "%s",
            proto_strs[LEX_UNTOK(CONMAN_TOK_OK)]);
        assert(n >= 0 && n < len);
        ptr += n;
        len -= n;

        /*  Note: if buf[] is overflowed by console strings,
         *    a valid rsp will still be sent to the client,
         *    but some console named may be omitted.
         */
        if ((i = list_iterator_create(req->consoles))) {
            while ((console = list_next(i))) {
                n = strlcpy(tmp, console->name, sizeof(tmp));
                assert(n >= 0 && n < sizeof(tmp));
                n = snprintf(ptr, len, " %s='%s'",
                    proto_strs[LEX_UNTOK(CONMAN_TOK_CONSOLE)], lex_encode(tmp));
                if (n < 0 || n >= len)
                    break;
                ptr += n;
                len -= n;
            }
            list_iterator_destroy(i);
        }

        *ptr++ = '\n';
        *ptr++ = '\0';
    }
    else {
    /*
     *  FIX_ME? Should all errors be logged?
     */
        n = strlcpy(tmp, (errmsg ? errmsg : "Doh!"), sizeof(tmp));
        assert(n >= 0 && n < sizeof(tmp));
        n = snprintf(buf, sizeof(buf), "%s %s=%d %s='%s'\n",
            proto_strs[LEX_UNTOK(CONMAN_TOK_ERROR)],
            proto_strs[LEX_UNTOK(CONMAN_TOK_CODE)], errnum,
            proto_strs[LEX_UNTOK(CONMAN_TOK_MESSAGE)], lex_encode(tmp));
        assert(n >= 0 && n < sizeof(buf));
    }

    /*  Write response to socket.
     */
    if (write_n(req->sd, buf, strlen(buf)) < 0) {
        log_msg(0, "Error writing to %s: %s", req->ip, strerror(errno));
        return(-1);
    }

    DPRINTF("Sent response: %s", buf);
    return(0);
}


static void perform_query_cmd(req_t *req)
{
/*  Performs the QUERY command, returning a list of consoles that
 *    matches the console patterns given in the client's request.
 *  Since this cmd is processed entirely by this thread,
 *    the client socket connection is closed once it is finished.
 */
    assert(req->sd >= 0);
    assert(req->command == QUERY);
    assert(!list_is_empty(req->consoles));

    send_rsp(req, CONMAN_ERR_NONE, NULL);

    if (close(req->sd) < 0)
        err_msg(errno, "close(%d) failed", req->sd);
    req->sd = -1;

    return;
}


static void perform_monitor_cmd(req_t *req, server_conf_t *conf)
{
/*  Performs the MONITOR command, placing the client in a
 *    "read-only" session with a single console.
 */
    obj_t *socket;
    obj_t *console;

    assert(req->sd >= 0);
    assert(req->command == MONITOR);
    assert(list_count(req->consoles) == 1);

    console = list_peek(req->consoles);
    socket = create_socket_obj(
        conf->objs, req->user, req->ip, req->port, req->sd);
    if (!socket) {
        send_rsp(req, CONMAN_ERR_NO_RESOURCES,
            "Insufficient resources to process request.");
        return;
    }
    send_rsp(req, CONMAN_ERR_NONE, NULL);
    link_objs(console, socket);
    return;
}


static void perform_connect_cmd(req_t *req, server_conf_t *conf)
{
/*  Performs the CONNECT command.  If a single console is specified,
 *    the client is placed in a "read-write" session with that console.
 *    Otherwise, the client is placed in a "write-only" broadcast session
 *    affecting multiple consoles.
 */
    obj_t *socket;
    obj_t *console;
    ListIterator i;
    char buf[MAX_LINE];
    int n;

    assert(req->sd >= 0);
    assert(req->command == CONNECT);

    socket = create_socket_obj(
        conf->objs, req->user, req->ip, req->port, req->sd);
    if (!socket) {
        send_rsp(req, CONMAN_ERR_NO_RESOURCES,
            "Insufficient resources to process request.");
        return;
    }
    send_rsp(req, CONMAN_ERR_NONE, NULL);

    if (list_count(req->consoles) == 1) {
        /*
         *  Unicast connection.
         */
        console = list_peek(req->consoles);
        link_objs(socket, console);
        link_objs(console, socket);
    }
    else {
        /*
         *  Broadcast connection.
         */
        if (!(i = list_iterator_create(req->consoles)))
            err_msg(0, "Out of memory");
        while ((console = list_next(i))) {
            assert(console->type == CONSOLE);
            n = snprintf(buf, sizeof(buf),
                "\r\n%s Broadcast for console [%s] opened for <%s@%s>.\r\n",
                CONMAN_MSG_PREFIX, console->name, req->user, req->host);
            assert(n >= 0 && n < sizeof(buf));
            if (console->writer) {
                assert(console->writer->type == SOCKET);
                write_obj_data(console->writer, buf, strlen(buf));
            }
        }
        list_iterator_destroy(i);

        /*  Move the req->consoles objs list to socket->readers.
         *    Once moved, req->consoles must be set to NULL in order
         *    to prevent destroy_request() from destroying the list.
         */
        socket->readers = req->consoles;
        req->consoles = NULL;
    }
    return;
}


static void perform_execute_cmd(req_t *req, server_conf_t *conf)
{
    assert(req->sd >= 0);
    assert(req->command == EXECUTE);

    /*  FIX_ME: NOT_IMPLEMENTED_YET
     */

    return;
}
