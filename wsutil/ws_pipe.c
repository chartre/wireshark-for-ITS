/* ws_pipe.c
 *
 * Routines for handling pipes.
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h> /* for _O_BINARY */
#include <wsutil/win32-utils.h>
#else
#include <unistd.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#endif

#include <glib.h>
#include <log.h>

#include "wsutil/filesystem.h"
#include "wsutil/ws_pipe.h"

gboolean ws_pipe_spawn_sync(gchar *dirname, gchar *command, gint argc, gchar **args, gchar **command_output)
{
    gboolean status = FALSE;
    gboolean result = FALSE;
    gchar **argv = NULL;
    gint cnt = 0;
    gchar *local_output = NULL;
#ifdef _WIN32

#define BUFFER_SIZE 16384

    GString *winargs = g_string_sized_new(200);
    gchar *quoted_arg;

    STARTUPINFO info;
    PROCESS_INFORMATION processInfo;

    SECURITY_ATTRIBUTES sa;
    HANDLE child_stdout_rd = NULL;
    HANDLE child_stdout_wr = NULL;
    HANDLE child_stderr_rd = NULL;
    HANDLE child_stderr_wr = NULL;

    const gchar *oldpath = g_getenv("PATH");
    gchar *newpath = NULL;
#else
    gint exit_status = 0;
#endif

    argv = (gchar **) g_malloc0(sizeof(gchar *) * (argc + 2));

#ifdef _WIN32
    newpath = g_strdup_printf("%s;%s", g_strescape(get_progfile_dir(), NULL), oldpath);
    g_setenv("PATH", newpath, TRUE);

    argv[0] = g_strescape(command, NULL);
#else
    argv[0] = g_strdup(command);
#endif

    for (cnt = 0; cnt < argc; cnt++)
        argv[cnt + 1] = args[cnt];
    argv[argc + 1] = NULL;

#ifdef _WIN32

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&child_stdout_rd, &child_stdout_wr, &sa, 0))
    {
        g_free(argv[0]);
        g_log(LOG_DOMAIN_CAPTURE, G_LOG_LEVEL_DEBUG, "Could not create stdout handle");
        return FALSE;
    }

    if (!CreatePipe(&child_stderr_rd, &child_stderr_wr, &sa, 0))
    {
        CloseHandle(child_stdout_rd);
        CloseHandle(child_stdout_wr);
        g_free(argv[0]);
        g_log(LOG_DOMAIN_CAPTURE, G_LOG_LEVEL_DEBUG, "Could not create stderr handle");
        return FALSE;
    }

    /* convert args array into a single string */
    /* XXX - could change sync_pipe_add_arg() instead */
    /* there is a drawback here: the length is internally limited to 1024 bytes */
    for (cnt = 0; argv[cnt] != 0; cnt++) {
        if (cnt != 0) g_string_append_c(winargs, ' ');    /* don't prepend a space before the path!!! */
        quoted_arg = protect_arg(argv[cnt]);
        g_string_append(winargs, quoted_arg);
        g_free(quoted_arg);
    }

    memset(&processInfo, 0, sizeof(PROCESS_INFORMATION));
    memset(&info, 0, sizeof(STARTUPINFO));

    info.cb = sizeof(STARTUPINFO);
    info.hStdError = child_stderr_wr;
    info.hStdOutput = child_stdout_wr;
    info.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    info.wShowWindow = SW_HIDE;

    if (win32_create_process(NULL, winargs->str, NULL, NULL, TRUE, CREATE_NEW_CONSOLE, NULL, NULL, &info, &processInfo))
    {
        gchar* buffer = (gchar*)g_malloc(BUFFER_SIZE);
        DWORD dw;
        DWORD bytes_read;
        DWORD bytes_avail;
        GString *output_string = g_string_new(NULL);

        for (;;)
        {
            /* Keep peeking at pipes every 100 ms. */
            dw = WaitForSingleObject(processInfo.hProcess, 100000);
            if (dw == WAIT_OBJECT_0)
            {
                /* Process finished. Nothing left to do here. */
                break;
            }
            else if (dw != WAIT_TIMEOUT)
            {
                g_log(LOG_DOMAIN_CAPTURE, G_LOG_LEVEL_DEBUG, "WaitForSingleObject returned 0x%08X. Error %d", dw, GetLastError());
                break;
            }

            if (PeekNamedPipe(child_stdout_rd, NULL, 0, NULL, &bytes_avail, NULL))
            {
                if (bytes_avail > 0)
                {
                    bytes_avail = min(bytes_avail, BUFFER_SIZE);
                    if (ReadFile(child_stdout_rd, &buffer[0], bytes_avail, &bytes_read, NULL))
                    {
                        g_string_append_len(output_string, buffer, bytes_read);
                    }
                }
            }

            /* Discard the stderr data just like non-windows version of this function does. */
            if (PeekNamedPipe(child_stderr_rd, NULL, 0, NULL, &bytes_avail, NULL))
            {
                if (bytes_avail > 0)
                {
                    bytes_avail = min(bytes_avail, BUFFER_SIZE);
                    ReadFile(child_stderr_rd, &buffer[0], bytes_avail, &bytes_read, NULL);
                }
            }
        }

        /* At this point the process is finished but there might still be unread data in the pipe. */
        while (PeekNamedPipe(child_stdout_rd, NULL, 0, NULL, &bytes_avail, NULL))
        {
            if (bytes_avail == 0)
            {
                /* Pipe is drained. */
                break;
            }
            bytes_avail = min(bytes_avail, BUFFER_SIZE);
            if (ReadFile(child_stdout_rd, &buffer[0], BUFFER_SIZE, &bytes_read, NULL))
            {
                g_string_append_len(output_string, buffer, bytes_read);
            }
        }

        g_free(buffer);

        status = GetExitCodeProcess(processInfo.hProcess, &dw);
        if (status && dw != 0)
        {
            status = FALSE;
        }

        local_output = g_string_free(output_string, FALSE);

        CloseHandle(child_stdout_rd);
        CloseHandle(child_stdout_wr);
        CloseHandle(child_stderr_rd);
        CloseHandle(child_stderr_wr);

        CloseHandle(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
    }
    else
        status = FALSE;

    g_setenv("PATH", oldpath, TRUE);
#else

    status = g_spawn_sync(dirname, argv, NULL,
                          (GSpawnFlags) 0, NULL, NULL, &local_output, NULL, &exit_status, NULL);

    if (status && exit_status != 0)
        status = FALSE;
#endif

    if (status)
    {
        if (command_output != NULL && local_output != NULL)
            *command_output = g_strdup(local_output);

        result = TRUE;
    }

    g_free(local_output);
    g_free(argv[0]);
    g_free(argv);

    return result;
}

void ws_pipe_init(ws_pipe_t *ws_pipe)
{
    if (!ws_pipe) return;
    memset(ws_pipe, 0, sizeof(ws_pipe_t));
    ws_pipe->pid = WS_INVALID_PID;
}

GPid ws_pipe_spawn_async(ws_pipe_t *ws_pipe, GPtrArray *args)
{
    GPid pid = WS_INVALID_PID;

#ifdef _WIN32
    gint cnt = 0;
    gchar **tmp = NULL;

    GString *winargs = g_string_sized_new(200);
    gchar *quoted_arg;

    STARTUPINFO info;
    PROCESS_INFORMATION processInfo;

    SECURITY_ATTRIBUTES sa;
    HANDLE child_stdin_rd = NULL;
    HANDLE child_stdin_wr = NULL;
    HANDLE child_stdout_rd = NULL;
    HANDLE child_stdout_wr = NULL;
    HANDLE child_stderr_rd = NULL;
    HANDLE child_stderr_wr = NULL;

    const gchar *oldpath = g_getenv("PATH");
    gchar *newpath = NULL;

    newpath = g_strdup_printf("%s;%s", g_strescape(get_progfile_dir(), NULL), oldpath);
    g_setenv("PATH", newpath, TRUE);

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&child_stdin_rd, &child_stdin_wr, &sa, 0))
    {
        g_log(LOG_DOMAIN_CAPTURE, G_LOG_LEVEL_DEBUG, "Could not create stdin handle");
        return FALSE;
    }

    if (!CreatePipe(&child_stdout_rd, &child_stdout_wr, &sa, 0))
    {
        g_log(LOG_DOMAIN_CAPTURE, G_LOG_LEVEL_DEBUG, "Could not create stdout handle");
        return FALSE;
    }

    if (!CreatePipe(&child_stderr_rd, &child_stderr_wr, &sa, 0))
    {
        CloseHandle(child_stdout_rd);
        CloseHandle(child_stdout_wr);
        g_log(LOG_DOMAIN_CAPTURE, G_LOG_LEVEL_DEBUG, "Could not create stderr handle");
        return FALSE;
    }

    /* convert args array into a single string */
    /* XXX - could change sync_pipe_add_arg() instead */
    /* there is a drawback here: the length is internally limited to 1024 bytes */
    for (tmp = (gchar **)args->pdata, cnt = 0; *tmp && **tmp; ++cnt, ++tmp) {
        if (cnt != 0) g_string_append_c(winargs, ' ');    /* don't prepend a space before the path!!! */
        quoted_arg = protect_arg(*tmp);
        g_string_append(winargs, quoted_arg);
        g_free(quoted_arg);
    }

    memset(&processInfo, 0, sizeof(PROCESS_INFORMATION));
    memset(&info, 0, sizeof(STARTUPINFO));

    info.cb = sizeof(STARTUPINFO);
    info.hStdInput = child_stdin_rd;
    info.hStdError = child_stderr_wr;
    info.hStdOutput = child_stdout_wr;
    info.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    info.wShowWindow = SW_HIDE;

    if (win32_create_process(NULL, winargs->str, NULL, NULL, TRUE, CREATE_NEW_CONSOLE, NULL, NULL, &info, &processInfo))
    {
        ws_pipe->stdin_fd = _open_osfhandle((intptr_t)(child_stdin_wr), _O_BINARY);
        ws_pipe->stdout_fd = _open_osfhandle((intptr_t)(child_stdout_rd), _O_BINARY);
        ws_pipe->stderr_fd = _open_osfhandle((intptr_t)(child_stderr_rd), _O_BINARY);
        ws_pipe->threadId = processInfo.hThread;
        pid = processInfo.hProcess;
    }

    g_setenv("PATH", oldpath, TRUE);
#else
    GError *error = NULL;
    gboolean spawned = g_spawn_async_with_pipes(NULL, (gchar **)args->pdata, NULL,
                             (GSpawnFlags) G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                             &pid, &ws_pipe->stdin_fd, &ws_pipe->stdout_fd, &ws_pipe->stderr_fd, &error);
    if (!spawned) {
        g_log(LOG_DOMAIN_CAPTURE, G_LOG_LEVEL_DEBUG, "Error creating async pipe: %s", error->message);
        g_free(error->message);
    }
#endif

    ws_pipe->pid = pid;

    return pid;
}

#ifdef _WIN32

typedef struct
{
    HANDLE pipeHandle;
    OVERLAPPED ol;
    BOOL pendingIO;
} PIPEINTS;

gboolean
ws_pipe_wait_for_pipe(HANDLE * pipe_handles, int num_pipe_handles, HANDLE pid)
{
    PIPEINTS pipeinsts[3];
    DWORD dw, cbRet;
    HANDLE handles[4];
    int error_code;
    int num_waiting_to_connect = 0;
    int num_handles = num_pipe_handles + 1; // PID handle is also added to list of handles.

    SecureZeroMemory(pipeinsts, sizeof(pipeinsts));

    if (num_pipe_handles == 0 || num_pipe_handles > 3)
    {
        g_log(LOG_DOMAIN_CAPTURE, G_LOG_LEVEL_DEBUG, "Invalid number of pipes given as argument.");
        return FALSE;
    }

    for (int i = 0; i < num_pipe_handles; ++i)
    {
        pipeinsts[i].pipeHandle = pipe_handles[i];
        pipeinsts[i].ol.Pointer = 0;
        pipeinsts[i].ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        pipeinsts[i].pendingIO = FALSE;
        handles[i] = pipeinsts[i].ol.hEvent;
        BOOL connected = ConnectNamedPipe(pipeinsts[i].pipeHandle, &pipeinsts[i].ol);
        if (connected)
        {
            error_code = GetLastError();
            g_log(LOG_DOMAIN_CAPTURE, G_LOG_LEVEL_DEBUG, "ConnectNamedPipe failed with %d \n.", error_code);
            return FALSE;
        }

        switch (GetLastError())
        {
        case ERROR_IO_PENDING:
            num_waiting_to_connect++;
            pipeinsts[i].pendingIO = TRUE;
            break;

        case ERROR_PIPE_CONNECTED:
            if (SetEvent(pipeinsts[i].ol.hEvent))
            {
                break;
            } // Fallthrough if this fails.

        default:
            error_code = GetLastError();
            g_log(LOG_DOMAIN_CAPTURE, G_LOG_LEVEL_DEBUG, "ConnectNamedPipe failed with %d \n.", error_code);
            return FALSE;
        }
    }

    // Store pid of extcap process so it can be monitored in case it fails before the pipes has connceted.
    handles[num_pipe_handles] = pid;

    while(num_waiting_to_connect > 0)
    {
        dw = WaitForMultipleObjects(num_handles, handles, FALSE, 30000);
        int idx = dw - WAIT_OBJECT_0;
        if (dw == WAIT_TIMEOUT)
        {
            g_log(LOG_DOMAIN_CAPTURE, G_LOG_LEVEL_DEBUG, "extcap didn't connect to pipe within 30 seconds.");
            return FALSE;
        }
        // If index points to our handles array
        else if (idx >= 0 && idx < num_handles)
        {
            if (idx < num_pipe_handles)  // Index of pipe handle
            {
                if (pipeinsts[idx].pendingIO)
                {
                    BOOL success = GetOverlappedResult(
                        pipeinsts[idx].pipeHandle, // handle to pipe
                        &pipeinsts[idx].ol,        // OVERLAPPED structure
                        &cbRet,                    // bytes transferred
                        FALSE);                    // do not wait

                    if (!success)
                    {
                        g_log(LOG_DOMAIN_CAPTURE, G_LOG_LEVEL_DEBUG, "Error %d \n.", GetLastError());
                        return FALSE;
                    }
                    else
                    {
                        pipeinsts[idx].pendingIO = FALSE;
                        CloseHandle(pipeinsts[idx].ol.hEvent);
                        num_waiting_to_connect--;
                    }
                }
            }
            else // Index of PID
            {
                // Fail since index of 'pid' indicates that the pid of the extcap process has terminated.
                g_log(LOG_DOMAIN_CAPTURE, G_LOG_LEVEL_DEBUG, "extcap terminated without connecting to pipe.");
                return FALSE;
            }
        }
        else
        {
            g_log(LOG_DOMAIN_CAPTURE, G_LOG_LEVEL_DEBUG, "WaitForMultipleObjects returned 0x%08X. Error %d", dw, GetLastError());
            return FALSE;
        }
    }

    return TRUE;
}
#endif

gboolean
ws_pipe_data_available(int pipe_fd)
{
#ifdef _WIN32 /* PeekNamedPipe */
    HANDLE hPipe = (HANDLE) _get_osfhandle(pipe_fd);
    DWORD bytes_avail;

    if (hPipe == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }

    if (! PeekNamedPipe(hPipe, NULL, 0, NULL, &bytes_avail, NULL))
    {
        return FALSE;
    }

    if (bytes_avail > 0)
    {
        return TRUE;
    }
    return FALSE;
#else /* select */
    fd_set rfds;
    struct timeval timeout;

    FD_ZERO(&rfds);
    FD_SET(pipe_fd, &rfds);
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    if (select(pipe_fd + 1, &rfds, NULL, NULL, &timeout) > 0)
    {
        return TRUE;
    }

    return FALSE;
#endif
}

gboolean
ws_read_string_from_pipe(ws_pipe_handle read_pipe, gchar *buffer,
                         size_t buffer_size)
{
    size_t total_bytes_read;
    size_t buffer_bytes_remaining;
#ifdef _WIN32
    DWORD bytes_to_read;
    DWORD bytes_read;
    DWORD bytes_avail;
#else
    size_t bytes_to_read;
    ssize_t bytes_read;
#endif
    int ret = FALSE;

    if (buffer_size == 0)
    {
        /* XXX - provide an error string */
        return FALSE;
    }

    total_bytes_read = 0;
    for (;;)
    {
        /* Leave room for the terminating NUL. */
        buffer_bytes_remaining = buffer_size - total_bytes_read - 1;
        if (buffer_bytes_remaining == 0)
        {
            /* The string won't fit in the buffer. */
            g_log(LOG_DOMAIN_CAPTURE, G_LOG_LEVEL_DEBUG, "Buffer too small (%zd).", buffer_size);
            break;
        }

#ifdef _WIN32
        /*
         * XXX - is there some reason why we do this before reading?
         *
         * If we're not trying to do UN*X-style non-blocking I/O,
         * where we don't block if there isn't data available to
         * read right now, I'm not sure why we do this.
         *
         * If we *are* trying to do UN*X-style non-blocking I/O,
         * 1) we're presumably in an event loop waiting for,
         * among other things, input to be available on the
         * pipe, in which case we should be doing "overlapped"
         * I/O and 2) we need to accumulate data until we have
         * a complete string, rather than just saying "OK, here's
         * the string".)
         */
        if (!PeekNamedPipe(read_pipe, NULL, 0, NULL, &bytes_avail, NULL))
        {
            break;
        }
        if (bytes_avail == 0)
        {
            ret = TRUE;
            break;
        }

        /*
         * Truncate this to whatever fits in a DWORD.
         */
        if (buffer_bytes_remaining > 0x7fffffff)
        {
            bytes_to_read = 0x7fffffff;
        }
        else
        {
            bytes_to_read = (DWORD)buffer_bytes_remaining;
        }
        if (!ReadFile(read_pipe, &buffer[total_bytes_read], bytes_to_read,
            &bytes_read, NULL))
        {
            /* XXX - provide an error string */
            break;
        }
#else
        bytes_to_read = buffer_bytes_remaining;
        bytes_read = read(read_pipe, buffer, bytes_to_read);
        if (bytes_read == -1)
        {
            /* XXX - provide an error string */
            break;
        }
#endif
        if (bytes_read == 0)
        {
            ret = TRUE;
            break;
        }

        total_bytes_read += bytes_read;
    }

    buffer[total_bytes_read] = '\0';
    return ret;
}

/*
 * Editor modelines  -  http://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
