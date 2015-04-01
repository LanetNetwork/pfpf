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

#pragma once

#ifndef _PFPF_H_
#define _PFPF_H_

#include <arpa/inet.h>
#include <pfcq.h>
#include <pthread.h>

#if !defined(SO_REUSEPORT)
#define _NO_SO_REUSEPORT
#endif /* !defined(SO_REUSEPORT) */

typedef struct pfpf_client_context
{
	void* data;
	char buffer[NET_CHUNK_SIZE];
	ssize_t bytes_read;
	int socket;
	int should_close;
} pfpf_client_context_t;

typedef void (pfpf_initializer_t)(void**);
typedef void (pfpf_handler_t)(pfpf_client_context_t*);
typedef void (pfpf_finalizer_t)(void*);

struct pfpf_worker_context;

typedef struct pfpf_pool
{
	struct pfpf_worker_context* workers;
	int workers_count;
	unsigned short int __padding; /* struct padding */
	volatile unsigned short int should_exit;
} pfpf_pool_t;

typedef struct pfpf_worker_context
{
	pfpf_pool_t* parent_pool;
	pthread_t tid;
	int server_socket;
	int epoll_fd;
	struct sockaddr_in server;
	pfpf_client_context_t** clients;
	int clients_pool_size;
	int clients_count;
	pfpf_initializer_t* initializer;
	pfpf_handler_t* handler;
	pfpf_finalizer_t* finalizer;
} pfpf_worker_context_t;

pfpf_pool_t* pfpf_init(const int _workers_count, const unsigned short _port, pfpf_initializer_t* _initializer, pfpf_handler_t* _handler, pfpf_finalizer_t* _finalizer);
void pfpf_done(pfpf_pool_t* _pool);

#endif /* _pfpf_H_ */

