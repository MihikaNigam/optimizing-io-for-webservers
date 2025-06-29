# optimizing-io-for-webservers
This research [thesis](https://digitalcommons.calpoly.edu/theses/3037/) aims to optimize http webservers and analyze performance on existing webservers.

Uses IO_URING


Client Sends HTTP Request -> packets arrive at NIC -> NIC triggers an interrupt to notify kernel -> the driver collects these packets -> req passes thru tcp/ip stacks-> network management places queue is req’s socket buffer -> webserver calls recv to read the req -> kernel returns the HTTP request data to the user-space

![image](https://github.com/user-attachments/assets/11830be2-6e7a-4082-9d58-cde26bd4c22c)


