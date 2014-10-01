/*
 * Copyright (c) 2014, Alexandru Csete <oz9aec@gmail.com>
 * All rights reserved.
 *
 * This software is licensed under the terms and conditions of the
 * Simplified BSD License. See license.txt for details.
 *
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>           // PRId64 and PRIu64
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"


static int      port = DEFAULT_AUDIO_PORT;      /* Network port */
static int      keep_running = 1;       /* set to 0 to exit infinite loop */


void signal_handler(int signo)
{
    if (signo == SIGINT)
        fprintf(stderr, "\nCaught SIGINT\n");
    else if (signo == SIGTERM)
        fprintf(stderr, "\nCaught SIGTERM\n");
    else
        fprintf(stderr, "\nCaught signal: %d\n", signo);

    keep_running = 0;
}

static void help(void)
{
    static const char help_string[] =
        "\n Usage: audio_server [options]\n"
        "\n Possible options are:\n"
        "\n"
        "  -p    Network port number (default is 42001).\n"
        "  -h    This help message.\n\n";

    fprintf(stderr, "%s", help_string);
}

/* Parse command line options */
static void parse_options(int argc, char **argv)
{
    int             option;

    if (argc > 1)
    {
        while ((option = getopt(argc, argv, "p:h")) != -1)
        {
            switch (option)
            {
            case 'p':
                port = atoi(optarg);
                break;

            case 'h':
                help();
                exit(EXIT_SUCCESS);

            default:
                help();
                exit(EXIT_FAILURE);
            }

        }
    }
}


int main(int argc, char **argv)
{
    int             exit_code = EXIT_FAILURE;
    int             sock_fd, audio_fd;
    struct sockaddr_in serv_addr, cli_addr;
    socklen_t       cli_addr_len;

    struct pollfd   poll_fds[3];
    int             num;
    int             connected;

    struct xfr_buf  net_in_buf;
    struct audio_buf abuf;

    net_in_buf.wridx = 0;
    net_in_buf.valid_pkts = 0;
    net_in_buf.invalid_pkts = 0;

    abuf.wridx = 0;
    abuf.bytes_read = 0;
    abuf.avg_read = 0;
    audio_fd = 0;               /* stdin */

    /* setup signal handler */
    if (signal(SIGINT, signal_handler) == SIG_ERR)
        printf("Warning: Can't catch SIGINT\n");
    if (signal(SIGTERM, signal_handler) == SIG_ERR)
        printf("Warning: Can't catch SIGTERM\n");

    parse_options(argc, argv);
    fprintf(stderr, "Using network port %d\n", port);


    /* open and configure network interface */
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1)
    {
        fprintf(stderr, "Error creating socket: %d: %s\n", errno,
                strerror(errno));
        goto cleanup;
    }

    int             yes = 1;

    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1)
        fprintf(stderr, "Error setting SO_REUSEADDR: %d: %s\n", errno,
                strerror(errno));

    /* bind socket to host address */
    memset(&serv_addr, 0, sizeof(struct sockaddr_in));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
    if (bind(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
    {
        fprintf(stderr, "bind() error: %d: %s\n", errno, strerror(errno));
        goto cleanup;
    }

    if (listen(sock_fd, 1) == -1)
    {
        fprintf(stderr, "listen() error: %d: %s\n", errno, strerror(errno));
        goto cleanup;
    }

    memset(&cli_addr, 0, sizeof(struct sockaddr_in));
    cli_addr_len = sizeof(cli_addr);

    /* audio input */
    poll_fds[0].fd = audio_fd;
    poll_fds[0].events = POLLIN;

    /* network socket (listening for connections) */
    poll_fds[1].fd = sock_fd;
    poll_fds[1].events = POLLIN;

    /* networks socket to client (when connected)
     * FIXME: we could use it to check when writing is wont block
     */
    poll_fds[2].fd = -1;

    connected = 0;

    while (keep_running)
    {

        if (poll(poll_fds, 3, 500) < 0)
            continue;

        /* service network socket */
        if (connected && (poll_fds[2].revents & POLLIN))
        {
            switch (read_data(poll_fds[2].fd, &net_in_buf))
            {
            case PKT_TYPE_EOF:
                fprintf(stderr, "Connection closed (FD=%d)\n", poll_fds[2].fd);
                close(poll_fds[2].fd);
                poll_fds[2].fd = -1;
                poll_fds[2].events = 0;
                connected = 0;
                break;
            }

            net_in_buf.wridx = 0;
        }

        /* check if there are any new connections pending */
        if (poll_fds[1].revents & POLLIN)
        {
            int             new;        /* FIXME: we can do it without 'new' */

            new = accept(sock_fd, (struct sockaddr *)&cli_addr, &cli_addr_len);
            if (new == -1)
            {
                fprintf(stderr, "accept() error: %d: %s\n", errno,
                        strerror(errno));
                goto cleanup;
            }

            fprintf(stderr, "New connection from %s\n",
                    inet_ntoa(cli_addr.sin_addr));

            if (!connected)
            {
                fprintf(stderr, "Connection accepted (FD=%d)\n", new);
                poll_fds[2].fd = new;
                poll_fds[2].events = POLLIN;

                connected = 1;
            }
            else
            {
                fprintf(stderr, "Connection refused\n");
                close(new);
            }
        }

        /* read data from audio FD */
        if (poll_fds[0].revents & POLLIN)
        {
            num = read(audio_fd, &abuf.data[abuf.wridx], FRAME_SIZE);

            if (num > 0)
            {
                abuf.wridx += num;
                abuf.bytes_read += num;
            }

            if (abuf.wridx >= FRAME_SIZE)
            {
                if (connected)
                    write(poll_fds[2].fd, abuf.data, abuf.wridx);

                abuf.wridx = 0;
            }
            abuf.avg_read += num;
            abuf.avg_read /= 2;
        }

        usleep(10000);
    }


    fprintf(stderr, "Shutting down...\n");
    exit_code = EXIT_SUCCESS;

  cleanup:
    close(poll_fds[1].fd);
    close(poll_fds[2].fd);

    fprintf(stderr, "  Audio bytes: %" PRIu64 "\n", abuf.bytes_read);
    fprintf(stderr, "  Average read: %" PRIu64 "\n", abuf.avg_read);

    exit(exit_code);
}