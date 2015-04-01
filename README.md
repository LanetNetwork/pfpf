pfpf
====

Description
-----------

Preforked workers pool implementation using SO\_REUSEPORT, pthreads and epoll.

Compiling
---------

### Prerequisites

* Linux (3.9+ for SO\_REUSEPORT support, tested with 3.10, 3.19);
* make (tested with GNU Make 3.82)
* gcc (tested with 4.8.2)
* cmake (tested with 2.8.11)

### Compiling

First, init and update git submodules:

`git submodule init`
`git submodule update`

Then create `build` folder, chdir to it and run:

`cmake ..`

Then run `make`.

Distribution and Contribution
-----------------------------

Distributed under terms and conditions of Apache 2.0 license
(see file LICENSE for details).

The following people are involved in development:

* Oleksandr Natalenko &lt;o.natalenko@lanet.ua&gt;

Mail them any suggestions, bugreports and comments.
