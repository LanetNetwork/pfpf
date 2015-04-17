/* vim: set tabstop=4:softtabstop=4:shiftwidth=4:noexpandtab */

/*
 * Copyright 2015 Lanet Network
 * Programmed by Oleksandr Natalenko <o.natalenko@lanet.ua>
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <linux/limits.h>
#include <pfcq.h>
#include <pfpf.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void* pfpf_worker(void* _data)
{
	int epoll_count = -1;
	socklen_t restrict_length = 0;
	int client_socket = -1;
	struct sockaddr_in client;
	struct epoll_event client_epoll_event;
	struct epoll_event epoll_events[EPOLL_MAXEVENTS];
	sigset_t epoll_sigmask_new;
	sigset_t epoll_sigmask_old;
	pfpf_worker_context_t* data = _data;

	pfcq_zero(&client, sizeof(struct sockaddr_in));
	pfcq_zero(&client_epoll_event, sizeof(struct epoll_event));
	pfcq_zero(epoll_events, EPOLL_MAXEVENTS * sizeof(struct epoll_event));
	pfcq_zero(&epoll_sigmask_new, sizeof(sigset_t));
	pfcq_zero(&epoll_sigmask_old, sizeof(sigset_t));

	pthread_setname_np(pthread_self(), "worker");

	if (unlikely(!data))
		goto out;

	if (unlikely(sigemptyset(&epoll_sigmask_new) != 0))
		panic("sigemptyset");
	if (unlikely(sigaddset(&epoll_sigmask_new, SIGTERM) != 0))
		panic("sigaddset");
	if (unlikely(sigaddset(&epoll_sigmask_new, SIGINT) != 0))
		panic("sigaddset");
	if (unlikely(pthread_sigmask(SIG_BLOCK, &epoll_sigmask_new, &epoll_sigmask_old) != 0))
		panic("pthread_sigmask");

	for (;;)
	{
		epoll_count = epoll_pwait(data->epoll_fd, epoll_events, EPOLL_MAXEVENTS, -1, &epoll_sigmask_old);

		if (unlikely(epoll_count == -1))
		{
			if (likely(errno == EINTR))
			{
				if (likely(data->parent_pool->should_exit))
				{
					debug("Worker #%d got interrupt signal, attempting to exit gracefully...\n", data->server_socket);
					goto lfree;
				} else
					continue;
			}
			else
			{
				warning("epoll_pwait");
				continue;
			}
		} else
		{
			for (int i = 0; i < epoll_count; i++)
			{
				int cfd = epoll_events[i].data.fd;
				if (likely(epoll_events[i].events & EPOLLIN && !(epoll_events[i].events & EPOLLERR)))
				{
					if (unlikely(cfd == data->server_socket))
					{
						restrict_length = (socklen_t)sizeof(struct sockaddr_in);
						pfcq_zero(&client, sizeof(struct sockaddr_in));
						pfcq_zero(&client_epoll_event, sizeof(struct epoll_event));

						client_socket = accept4(data->server_socket, (struct sockaddr*)&client, &restrict_length, SOCK_NONBLOCK);
						if (unlikely(client_socket < 0))
						{
							if (likely(errno == EAGAIN || errno == EWOULDBLOCK))
								continue;
							else
							{
								warning("accept4");
								continue;
							}
						}
						pfpf_client_context_t* new_pfpf_client_context = pfcq_alloc(sizeof(pfpf_client_context_t));
						new_pfpf_client_context->socket = client_socket;
						data->initializer(&new_pfpf_client_context->data);
						data->clients[client_socket] = new_pfpf_client_context;
						data->clients_count++;

						debug("Accepted client to #%d socket by #%d server\n", client_socket, data->server_socket);
						debug("Total clients of server #%d: %d\n", data->server_socket, data->clients_count);

						client_epoll_event.data.fd = client_socket;
						client_epoll_event.events = EPOLLIN;
						if (unlikely(epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, client_socket, &client_epoll_event) == -1))
							panic("epoll_ctl");
					} else
					{
						pfpf_client_context_t* current_pfpf_client_context = data->clients[cfd];

						current_pfpf_client_context->bytes_read = read(current_pfpf_client_context->socket, current_pfpf_client_context->buffer, NET_CHUNK_SIZE);
						if (likely(current_pfpf_client_context->bytes_read > 0))
						{
							data->handler(current_pfpf_client_context);
							if (current_pfpf_client_context->should_close)
								goto should_close;
						} else
						{
should_close:
							debug("Saying good bye to socket #%d from server #%d\n", current_pfpf_client_context->socket, data->server_socket);
							if (unlikely(epoll_ctl(data->epoll_fd, EPOLL_CTL_DEL, current_pfpf_client_context->socket, NULL) == -1))
								panic("epoll_ctl");
							if (unlikely(close(current_pfpf_client_context->socket) == -1))
								warning("close");
							if (likely(current_pfpf_client_context->data))
							{
								data->finalizer(current_pfpf_client_context->data);
								pfcq_free(current_pfpf_client_context->data);
							}
							pfcq_free(current_pfpf_client_context);
							data->clients[cfd] = NULL;
							data->clients_count--;
						}
					}
				} else
				{
					warning("epoll_wait");
					continue;
				}
			}
		}
	}

lfree:
	debug("Cleaning up #%d server...\n", data->server_socket);

	for (int i = 0; i < data->clients_pool_size; i++)
	{
		if (unlikely(data->clients[i]))
		{
			debug("Detaching client #%d of server #%d...\n", data->clients[i]->socket, data->server_socket);
			close(data->clients[i]->socket);
			debug("Freeing client #%d data of server #%d...\n", data->clients[i]->socket, data->server_socket);
			if (likely(data->clients[i]->data))
			{
				data->finalizer(data->clients[i]->data);
				pfcq_free(data->clients[i]->data);
			}
			pfcq_free(data->clients[i]);
			data->clients_count--;
		}
	}
	debug("Destroying clients list of server #%d...\n", data->server_socket);
	pfcq_free(data->clients);

	close(data->server_socket);
	close(data->epoll_fd);

	debug("Server #%d cleaned up\n", data->server_socket);

out:

	return NULL;
}

pfpf_pool_t* pfpf_init(const int _workers_count, const unsigned short _port, pfpf_initializer_t* _initializer, pfpf_handler_t* _handler, pfpf_finalizer_t* _finalizer)
{
	int option = 1;
	struct epoll_event epoll_event;
	struct rlimit limits;
	pfpf_pool_t* ret = pfcq_alloc(sizeof(pfpf_pool_t));

#if defined(_NO_SO_REUSEPORT)
	(void)_workers_count;
	ret->workers_count = 1;
#else /* defined(_NO_SO_REUSEPORT) */
	ret->workers_count = pfcq_hint_cpus(_workers_count);
#endif /* defined(_NO_SO_REUSEPORT) */
	debug("Using %d workers\n", ret->workers_count);

	if (unlikely(getrlimit(RLIMIT_NOFILE, &limits) == -1))
		panic("getrlimit");
	debug("Maximum file descriptors: %lu\n", limits.rlim_cur);

	ret->workers = pfcq_alloc(ret->workers_count * sizeof(pfpf_worker_context_t));
	for (int i = 0; i < ret->workers_count; i++)
	{
		option = 1;
		pfcq_zero(&epoll_event, sizeof(struct epoll_event));

		ret->workers[i].server_socket = socket(AF_INET, SOCK_STREAM, 0);
		debug("Creating server socket #%d\n", ret->workers[i].server_socket);
		if (unlikely(ret->workers[i].server_socket == -1))
			panic("socket");
#if defined(_NO_SO_REUSEPORT)
		if (unlikely(setsockopt(ret->workers[i].server_socket, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) == -1))
#else /* defined(_NO_SO_REUSEPORT) */
		if (unlikely(setsockopt(ret->workers[i].server_socket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &option, sizeof(option)) == -1))
#endif /* defined(_NO_SO_REUSEPORT) */
			panic("setsockopt");

		ret->workers[i].parent_pool = ret;
		ret->workers[i].initializer = _initializer;
		ret->workers[i].handler = _handler;
		ret->workers[i].finalizer = _finalizer;
		ret->workers[i].clients_pool_size = limits.rlim_cur;
		ret->workers[i].clients = pfcq_alloc(limits.rlim_cur * sizeof(pfpf_client_context_t*));
		ret->workers[i].clients_count = 0;

		ret->workers[i].server.sin_family = AF_INET;
		ret->workers[i].server.sin_addr.s_addr = INADDR_ANY;
		ret->workers[i].server.sin_port = htons(_port);
		if (unlikely(bind(ret->workers[i].server_socket, (struct sockaddr*)&ret->workers[i].server, sizeof(struct sockaddr_in)) < 0))
			panic("bind");

		if (unlikely(listen(ret->workers[i].server_socket, SOMAXCONN) == -1))
			panic("listen");

		ret->workers[i].epoll_fd = epoll_create1(0);
		if (unlikely(ret->workers[i].epoll_fd == -1))
			panic("epoll_create");

		epoll_event.data.fd = ret->workers[i].server_socket;
		epoll_event.events = EPOLLIN;
		if (unlikely(epoll_ctl(ret->workers[i].epoll_fd, EPOLL_CTL_ADD, ret->workers[i].server_socket, &epoll_event) == -1))
			panic("epoll_ctl");

		if (unlikely(pthread_create(&ret->workers[i].tid, NULL, pfpf_worker, (void*)&ret->workers[i]) != 0))
			panic("pthread_create");
	}

	return ret;
}

void pfpf_done(pfpf_pool_t* _pool)
{
	_pool->should_exit = 1;
	for (int i = 0; i < _pool->workers_count; i++)
	{
		if (unlikely(pthread_kill(_pool->workers[i].tid, SIGINT) != 0))
			panic("pthread_kill");
		if (unlikely(pthread_join(_pool->workers[i].tid, NULL) != 0))
			panic("pthread_join");
	}
	pfcq_free(_pool->workers);
	pfcq_free(_pool);

	return;
}

