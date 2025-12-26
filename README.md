# FastRing

FastRing is an efficient event multiplexing library built upon `io_uring`, designed to provide high-performance asynchronous I/O operations. It offers a comprehensive set of components for various functionalities, including network communication, buffer management, and integration with popular frameworks.

## Features

* **FastRing Core:** Provides core functionalities for submission/completion multiplexing using `io_uring`.
* **FastBuffer:** Implements a fast buffer pool for efficient memory management during I/O operations.
* **CoRing:** Offers C++ coroutine adaptation for writing asynchronous code in a sequential style.

## API Description

### Core Components

* **Event Multiplexing:**
    * **Description:** Manages and multiplexes I/O events using `io_uring` for high-throughput asynchronous operations.
    * **Usage:** Facilitates non-blocking I/O by efficiently handling submission and completion of I/O requests.

* **Buffer Management:**
    * **Description:** Provides `FastBuffer`, a specialized buffer pool for rapid allocation and deallocation of memory buffers, reducing overhead in I/O-intensive applications.
    * **Usage:** Optimizes data transfer operations by providing pre-allocated and reusable buffers.

* **Coroutine Adaptation:**
    * **Description:** `CoRing` integrates C++20 coroutines with `io_uring`, enabling the development of highly concurrent and readable asynchronous code.
    * **Usage:** Allows developers to write asynchronous code that appears synchronous, improving code clarity and maintainability.

### Integration & Utilities

* **FastGLoop:**
    * **Description:** An adapter for integrating `FastRing` with the Glib 2.0 main loop, allowing seamless interoperability with Glib-based applications.
    * **Usage:** Enables the use of `FastRing`'s asynchronous capabilities within a Glib event loop environment.

* **DBusCore:**
    * **Description:** Provides an adapter for D-BUS integration, facilitating inter-process communication within a `FastRing` context.
    * **Usage:** Allows applications to send and receive messages over D-BUS using `FastRing`'s asynchronous model.

* **ThreadCall:**
    * **Description:** A utility for making thread calls, enabling the execution of operations on different threads safely and efficiently.
    * **Usage:** Simplifies managing multi-threaded operations and synchronizing data between threads.

* **FastSemaphore:**
    * **Description:** Implements reactive semaphores for synchronization between concurrent operations.
    * **Usage:** Manages access to shared resources and coordinates the execution of tasks.

* **FastSocket:**
    * **Description:** A generic socket I/O component providing a unified interface for network communication.
    * **Usage:** Supports various socket operations, including TCP/IP and UDP, with `io_uring` support.

* **FastBIO/SSLSocket:**
    * **Description:** Provides TLS/BIO over OpenSSL, enabling secure communication channels.
    * **Usage:** Facilitates encrypted network connections using standard SSL/TLS protocols.

* **Systemd Watchdog:**
    * **Description:** Includes implementation for systemd watchdog, ensuring application stability and responsiveness.
    * **Usage:** Integrates with systemd to signal application liveness, preventing service restarts due to unresponsiveness.

### Additional Features

* **Lua Bindings:** Allows `FastRing` functionalities to be accessed and controlled from Lua scripts.
* **DNS Resolution:** Provides capabilities for asynchronous DNS queries.
* **WebSocket Client Library:** Includes a client-side library for establishing and managing WebSocket connections.
* **CURL Wrapper:** Offers a wrapper for the `libcurl` library, enabling HTTP/HTTPS requests with `io_uring` integration.

For more detailed information, please refer to the source code and examples within the [FastRing GitHub repository](https://github.com/cyanide-burnout/FastRing).

